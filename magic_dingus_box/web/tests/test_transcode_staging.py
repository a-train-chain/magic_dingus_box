"""Transcode staging regression tests.

run_transcode_job used to point ffmpeg straight at media_dir/<name>.mp4:
for the whole multi-minute encode a growing, moov-less file was listed
by /admin/media and addable to a playlist, and a crash mid-encode
stranded it there forever, indistinguishable from a real video. The job
now encodes into <name>.mp4.part (invisible to every *.mp4 glob) with an
explicit `-f mp4` muxer, publishes via atomic same-directory os.replace
on success, unlinks the staging file on failure, and create_app sweeps
crashed leftovers at startup.
"""
from __future__ import annotations

import json
import os
import time
from pathlib import Path

import pytest

import sys
sys.path.insert(0, str(Path(__file__).parent.parent))

from admin import create_app


FFMPEG_STUB = """#!/usr/bin/env python3
import os, pathlib, sys
argv = sys.argv[1:]
log = os.environ.get("FFMPEG_STUB_LOG")
if log:
    pathlib.Path(log).write_text("\\n".join(argv))
if os.environ.get("FFMPEG_STUB_MODE") == "fail":
    sys.exit(1)
pathlib.Path(argv[-1]).write_bytes(b"encoded-video-bytes")
print("out_time_ms=1000000")
sys.exit(0)
"""

FFPROBE_STUB = """#!/usr/bin/env python3
print("10.0")
"""


@pytest.fixture
def stub_bin(temp_data_dir: Path, monkeypatch):
    # Deliberately NOT tmp_path: create_app points tempfile.tempdir into
    # upload_temp, and pytest's lazy tmp_path basetemp would then be
    # created inside a directory this fixture's teardown deletes —
    # breaking every later tmp_path-using test. Living inside the
    # temp_data_dir sandbox sidesteps that entirely.
    scratch = temp_data_dir.parent / "stub_scratch"
    bin_dir = scratch / "bin"
    bin_dir.mkdir(parents=True)
    for name, body in (("ffmpeg", FFMPEG_STUB), ("ffprobe", FFPROBE_STUB)):
        stub = bin_dir / name
        stub.write_text(body)
        stub.chmod(0o755)
    monkeypatch.setenv("PATH", str(bin_dir) + os.pathsep + os.environ["PATH"])
    monkeypatch.setenv("FFMPEG_STUB_LOG", str(scratch / "ffmpeg_argv.log"))
    return scratch / "ffmpeg_argv.log"


def _run_job(client, filename="clip.mov"):
    import io
    resp = client.post(
        "/admin/upload-and-transcode",
        data={"file": (io.BytesIO(b"raw-upload"), filename)},
        content_type="multipart/form-data")
    assert resp.status_code == 200
    job_id = resp.get_json()["data"]["job_id"]
    deadline = time.time() + 10
    while time.time() < deadline:
        status = client.get(f"/admin/transcode-status/{job_id}").get_json()["data"]
        if status["status"] in ("complete", "error"):
            return status
        time.sleep(0.05)
    pytest.fail("transcode job did not reach a terminal state")


def test_success_encodes_via_part_staging_then_publishes(
        client, temp_data_dir: Path, stub_bin: Path):
    media = temp_data_dir / "media"
    status = _run_job(client)

    assert status["status"] == "complete"
    assert (media / "clip.mp4").read_bytes() == b"encoded-video-bytes"
    assert list(media.glob("*.part")) == [], "staging file must not survive success"

    argv = stub_bin.read_text().splitlines()
    staging = Path(argv[-1]).name
    assert staging.startswith("clip.mp4.") and staging.endswith(".part"), (
        "ffmpeg must write to the .part staging name, never the final path")
    # .part defeats ffmpeg's extension-based muxer inference, so the
    # command must carry the explicit format.
    f_idx = argv.index("-f")
    assert argv[f_idx + 1] == "mp4"


def test_failure_leaves_no_file_in_media_dir(
        client, temp_data_dir: Path, stub_bin: Path, monkeypatch):
    monkeypatch.setenv("FFMPEG_STUB_MODE", "fail")
    media = temp_data_dir / "media"
    status = _run_job(client)

    assert status["status"] == "error"
    assert list(media.iterdir()) == [], (
        "a failed encode must leave neither a partial .mp4 nor a .part behind")


def test_startup_sweeps_crashed_part_files(temp_data_dir: Path):
    # Reuse the conftest sandbox (not tmp_path — see stub_bin) but run
    # create_app against a sibling data dir we control from scratch.
    data_dir = temp_data_dir.parent / "sweep_data"
    (data_dir / "playlists").mkdir(parents=True)
    media = data_dir / "media"
    media.mkdir()
    (data_dir / "upload_temp").mkdir()

    (media / "Interrupted Movie.mp4.part").write_bytes(b"x" * 4096)
    (media / "Real Movie.mp4").write_bytes(b"y" * 128)

    os.environ["MAGIC_DISABLE_CSRF"] = "1"
    try:
        create_app(data_dir, config={"TESTING": True})
    finally:
        del os.environ["MAGIC_DISABLE_CSRF"]

    assert not (media / "Interrupted Movie.mp4.part").exists(), (
        "startup must sweep crashed transcode staging files")
    assert (media / "Real Movie.mp4").exists(), (
        "the sweep must never touch finished videos")


# ---------------------------------------------------------------------------
# Concurrent uploads of the same filename.
#
# Output names used to be chosen by output_path.exists() at REQUEST time, but
# a transcode's output only appears at its final os.replace — minutes later.
# Two uploads of "clip.mov" in that window (two phones, a double-submit, or
# simply a queued second job behind the encoder semaphore) both picked
# clip.mp4: the second replace silently destroyed the first video, and both
# encoders shared clip.mp4.part, so one job's error path unlinked the other's
# in-progress encode.
# ---------------------------------------------------------------------------

SLOW_ECHO_FFMPEG = """#!/usr/bin/env python3
import pathlib, sys, time
argv = sys.argv[1:]
src = argv[argv.index("-i") + 1]
time.sleep(0.4)
pathlib.Path(argv[-1]).write_bytes(pathlib.Path(src).read_bytes())
print("out_time_ms=1000000")
"""


@pytest.fixture
def slow_echo_bin(temp_data_dir: Path, monkeypatch):
    bin_dir = temp_data_dir.parent / "slow_scratch" / "bin"
    bin_dir.mkdir(parents=True)
    for name, body in (("ffmpeg", SLOW_ECHO_FFMPEG), ("ffprobe", FFPROBE_STUB)):
        stub = bin_dir / name
        stub.write_text(body)
        stub.chmod(0o755)
    monkeypatch.setenv("PATH", str(bin_dir) + os.pathsep + os.environ["PATH"])


def _start(client, payload: bytes, filename="clip.mov", endpoint="/admin/upload-and-transcode"):
    import io
    data = {"file": (io.BytesIO(payload), filename)}
    if endpoint == "/admin/smart-upload":
        data["normalize_audio"] = "true"  # forces the transcode branch
    resp = client.post(endpoint, data=data, content_type="multipart/form-data")
    assert resp.status_code == 200, resp.get_json()
    return resp.get_json()["data"]["job_id"]


def _wait_all(client, job_ids):
    deadline = time.time() + 20
    out = {}
    while time.time() < deadline and len(out) < len(job_ids):
        for jid in job_ids:
            st = client.get(f"/admin/transcode-status/{jid}").get_json()["data"]
            if st["status"] in ("complete", "error"):
                out[jid] = st
        time.sleep(0.05)
    assert len(out) == len(job_ids), "transcode jobs did not finish"
    return out


@pytest.mark.parametrize("endpoint", ["/admin/upload-and-transcode", "/admin/smart-upload"])
def test_same_name_uploads_never_overwrite_each_other(
        client, temp_data_dir: Path, slow_echo_bin, endpoint):
    media = temp_data_dir / "media"
    a = _start(client, b"first-video", endpoint=endpoint)
    b = _start(client, b"second-video", endpoint=endpoint)
    results = _wait_all(client, [a, b])

    assert {r["status"] for r in results.values()} == {"complete"}
    names = {results[a]["output_filename"], results[b]["output_filename"]}
    assert names == {"clip.mp4", "clip_1.mp4"}
    contents = {p.read_bytes() for p in media.glob("*.mp4")}
    assert contents == {b"first-video", b"second-video"}, (
        "both uploads must survive as distinct files")
    assert list(media.glob("*.part")) == []


def test_name_is_released_after_the_job_finishes(
        client, temp_data_dir: Path, slow_echo_bin):
    """A finished job's reservation must not leak: deleting its output and
    uploading the same name again gets the plain name back."""
    media = temp_data_dir / "media"
    a = _start(client, b"one")
    _wait_all(client, [a])
    (media / "clip.mp4").unlink()
    b = _start(client, b"two")
    assert _wait_all(client, [b])[b]["output_filename"] == "clip.mp4"
