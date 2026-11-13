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

# Reports ONE video for every path it is asked about (the upload and, after
# publish, the published master), shaped by FFPROBE_STUB_* so a test can be a
# 16:9 clip, a sideways phone clip or an already-compatible master. Rotation
# is given clockwise, like the legacy rotate tag, and printed as the display
# matrix ffprobe >= 5 reports (counter-clockwise). FFPROBE_STUB_MODE=fail
# makes it an unreadable file.
FFPROBE_STUB = """#!/usr/bin/env python3
import json, os, sys
env = os.environ.get
if env("FFPROBE_STUB_MODE") == "fail":
    sys.exit(1)
stream = {
    "codec_type": "video", "codec_name": env("FFPROBE_STUB_CODEC", "h264"),
    "width": int(env("FFPROBE_STUB_WIDTH", "1920")),
    "height": int(env("FFPROBE_STUB_HEIGHT", "1080")),
    "sample_aspect_ratio": env("FFPROBE_STUB_SAR", "1:1"),
    "pix_fmt": "yuv420p", "profile": "High", "level": 40,
    "avg_frame_rate": "30/1", "r_frame_rate": "30/1",
}
rotate = int(env("FFPROBE_STUB_ROTATE", "0"))
if rotate:
    stream["side_data_list"] = [{"side_data_type": "Display Matrix",
                                 "rotation": -rotate}]
print(json.dumps({"streams": [stream], "format": {
    "format_name": "mov,mp4,m4a,3gp,3g2,mj2",
    "duration": env("FFPROBE_STUB_DURATION", "10.0")}}))
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


def test_unknown_job_is_a_404_the_uploader_can_act_on(client):
    """Transcode jobs are in-memory and their encoder dies with the web
    service, so after a restart the job is gone for good. manager.js keys
    its "failed — upload it again" handling on exactly this 404 +
    NOT_FOUND; it used to read the missing .data as "still running" and
    poll forever."""
    resp = client.get("/admin/transcode-status/0f0e0d0c-lost-after-restart")
    assert resp.status_code == 404
    body = resp.get_json()
    assert body["ok"] is False
    assert body["error"]["code"] == "NOT_FOUND"
    assert "restart" in body["error"]["message"].lower()


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


# ---------------------------------------------------------------------------
# Uploads keep their aspect ratio.
#
# The transcode used to crop or pad every upload into a fixed 4:3 frame and
# delete the original, so a "Fill screen" choice was permanent. The master now
# keeps the source's shape (sized by media_probe.master_size), and the upload's
# Framing choice becomes the new video's starting Crop setting in
# media_info.json, which the kiosk reads.
# ---------------------------------------------------------------------------

import io

import media_info


def _vf(argv_log: Path) -> str:
    argv = argv_log.read_text().splitlines()
    return argv[argv.index("-vf") + 1]


def test_widescreen_upload_keeps_its_aspect(client, stub_bin: Path):
    assert _run_job(client)["status"] == "complete"
    assert _vf(stub_bin) == "scale=1280:720,setsar=1"
    argv = stub_bin.read_text()
    assert "crop=" not in argv and "pad=" not in argv


def test_rotated_source_is_sized_upright(client, stub_bin: Path, monkeypatch):
    # A phone's portrait clip: stored 1920x1080, rotate tag 90. ffmpeg
    # auto-rotates its input, so the scale target is the UPRIGHT size.
    monkeypatch.setenv("FFPROBE_STUB_ROTATE", "90")
    assert _run_job(client)["status"] == "complete"
    assert _vf(stub_bin) == "scale=608:1080,setsar=1"


def _marker(data_dir: Path) -> Path:
    return data_dir / "media_info_reload_request"


@pytest.mark.parametrize("fit_mode,crop", [("fit", {"crop": False}),
                                           ("crop", {})])
def test_transcode_publish_records_media_info(
        client, temp_data_dir: Path, stub_bin: Path, fit_mode, crop):
    resp = client.post(
        "/admin/upload-and-transcode",
        data={"file": (io.BytesIO(b"raw-upload"), "clip.mov"),
              "fit_mode": fit_mode},
        content_type="multipart/form-data")
    assert resp.status_code == 200
    status = _wait_all(client, [resp.get_json()["data"]["job_id"]])
    assert list(status.values())[0]["status"] == "complete"

    assert media_info.load(temp_data_dir) == {"media/clip.mp4": {
        "duration_s": 10.0, "display_aspect": 1.7778, **crop}}
    assert _marker(temp_data_dir).exists(), "the kiosk must be poked"


def test_a_crop_upload_clears_a_stale_entrys_crop(
        client, temp_data_dir: Path, stub_bin: Path):
    # A key left behind by a deleted video must not hand its setting to the
    # new video published under the same name.
    media_info.update(temp_data_dir, "media/clip.mp4", crop=False)
    assert _run_job(client)["status"] == "complete"
    assert "crop" not in media_info.load(temp_data_dir)["media/clip.mp4"]


def _smart_upload(client, fit_mode="crop", resolution="crt_hd"):
    resp = client.post(
        "/admin/smart-upload",
        data={"file": (io.BytesIO(b"already-a-master"), "clip.mp4"),
              "fit_mode": fit_mode, "resolution": resolution,
              "normalize_audio": "false"},
        content_type="multipart/form-data")
    assert resp.status_code == 200, resp.get_json()
    return resp.get_json()["data"]


@pytest.mark.parametrize("env,direct", [
    ({"FFPROBE_STUB_WIDTH": "1280", "FFPROBE_STUB_HEIGHT": "720"}, True),
    ({"FFPROBE_STUB_WIDTH": "960", "FFPROBE_STUB_HEIGHT": "720"}, True),
    ({"FFPROBE_STUB_WIDTH": "608", "FFPROBE_STUB_HEIGHT": "1080"}, True),
    ({}, False),                                       # 1920x1080 > 1280x720
    ({"FFPROBE_STUB_WIDTH": "1280", "FFPROBE_STUB_HEIGHT": "720",
      "FFPROBE_STUB_ROTATE": "90"}, False),            # must be turned upright
    ({"FFPROBE_STUB_WIDTH": "960", "FFPROBE_STUB_HEIGHT": "720",
      "FFPROBE_STUB_SAR": "4:3"}, False),              # non-square pixels
    ({"FFPROBE_STUB_WIDTH": "1280", "FFPROBE_STUB_HEIGHT": "720",
      "FFPROBE_STUB_CODEC": "hevc"}, False),
])
def test_smart_upload_saves_a_master_sized_h264_source_directly(
        client, temp_data_dir: Path, stub_bin: Path, monkeypatch, env, direct):
    for k, v in env.items():
        monkeypatch.setenv(k, v)
    data = _smart_upload(client)
    assert data["needs_transcode"] is not direct, data["probe"]
    if direct:
        assert data["action"] == "direct"
        assert not stub_bin.exists(), "a direct save must not run ffmpeg"
        assert (temp_data_dir / "media" / "clip.mp4").read_bytes() == b"already-a-master"
    else:
        _wait_all(client, [data["job_id"]])


@pytest.mark.parametrize("fit_mode,crop", [("fit", {"crop": False}),
                                           ("crop", {})])
def test_direct_save_records_media_info(
        client, temp_data_dir: Path, stub_bin: Path, monkeypatch, fit_mode, crop):
    monkeypatch.setenv("FFPROBE_STUB_WIDTH", "1280")
    monkeypatch.setenv("FFPROBE_STUB_HEIGHT", "720")
    monkeypatch.setenv("FFPROBE_STUB_DURATION", "95.5")
    assert _smart_upload(client, fit_mode)["action"] == "direct"
    assert media_info.load(temp_data_dir) == {"media/clip.mp4": {
        "duration_s": 95.5, "display_aspect": 1.7778, **crop}}
    assert _marker(temp_data_dir).exists()


def _raw_upload(client):
    resp = client.post("/admin/upload",
                       data={"file": (io.BytesIO(b"raw"), "clip.mp4")},
                       content_type="multipart/form-data")
    assert resp.status_code == 200, resp.get_json()


def test_raw_upload_records_media_info(client, temp_data_dir: Path,
                                       stub_bin: Path, monkeypatch):
    monkeypatch.setenv("FFPROBE_STUB_WIDTH", "640")
    monkeypatch.setenv("FFPROBE_STUB_HEIGHT", "480")
    _raw_upload(client)
    assert media_info.load(temp_data_dir) == {"media/clip.mp4": {
        "duration_s": 10.0, "display_aspect": 1.3333}}
    assert _marker(temp_data_dir).exists()


def test_unreadable_published_file_writes_nothing(
        client, temp_data_dir: Path, stub_bin: Path, monkeypatch):
    monkeypatch.setenv("FFPROBE_STUB_MODE", "fail")
    _raw_upload(client)
    assert (temp_data_dir / "media" / "clip.mp4").exists()
    assert not (temp_data_dir / media_info.FILE_NAME).exists()
    assert not _marker(temp_data_dir).exists()


def test_unreadable_source_is_bounded_by_ffmpeg_and_writes_nothing(
        client, temp_data_dir: Path, stub_bin: Path, monkeypatch):
    # No probe, no master_size: ffmpeg keeps the shape inside 1920x1080 on
    # its own, and nothing is recorded for a file we could not read.
    monkeypatch.setenv("FFPROBE_STUB_MODE", "fail")
    assert _run_job(client)["status"] == "complete"
    assert _vf(stub_bin) == ("scale=w='min(iw,1920)':h='min(ih,1080)':"
                             "force_original_aspect_ratio=decrease:"
                             "force_divisible_by=2")
    assert not (temp_data_dir / media_info.FILE_NAME).exists()
    assert not _marker(temp_data_dir).exists()


@pytest.mark.parametrize("publish", ["raw", "direct", "transcode"])
def test_a_store_write_failure_never_fails_the_upload(
        client, temp_data_dir: Path, stub_bin: Path, monkeypatch, publish):
    def refuse(*_a, **_k):
        raise OSError(28, "No space left on device")
    monkeypatch.setattr(media_info, "update", refuse)
    if publish == "raw":
        _raw_upload(client)
    elif publish == "direct":
        monkeypatch.setenv("FFPROBE_STUB_WIDTH", "1280")
        monkeypatch.setenv("FFPROBE_STUB_HEIGHT", "720")
        assert _smart_upload(client)["action"] == "direct"
    else:
        assert _run_job(client)["status"] == "complete"
    assert (temp_data_dir / "media" / "clip.mp4").exists()
    assert not _marker(temp_data_dir).exists(), "no poke without a write"
