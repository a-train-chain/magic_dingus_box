"""The once-per-process back-fill of every video's length and shape into
media_info.json (web/media_backfill.py).

Videos already on a box before the store existed (old 16:9 `modern` masters,
package imports, raw uploads) have no display_aspect, so the Content Manager
offers them no Crop checkbox; the back-fill probes them, one at a time, in a
low-priority background thread started by create_app.
"""
from __future__ import annotations

import os
import subprocess
import sys
import threading
from pathlib import Path

import pytest

import admin
import media_backfill
import media_info

REPO = Path(__file__).resolve().parents[3]


def _videos(data: Path, *names: str) -> None:
    for n in names:
        f = data / "media" / n
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(b"not really a video")


class FakeProbe:
    def __init__(self, results=None):
        self.results = results or {}
        self.calls: list[str] = []

    def __call__(self, path):
        self.calls.append(Path(path).name)
        return self.results.get(Path(path).name,
                                {"duration_s": 12.5, "display_aspect": 1.7778})


def test_only_videos_missing_a_length_or_shape_are_probed_and_filled(tmp_path):
    _videos(tmp_path, "done.mp4", "new.mp4")
    media_info.update(tmp_path, "media/done.mp4",
                      duration_s=3.0, display_aspect=1.3333)
    probe = FakeProbe()
    assert media_backfill.run_backfill(tmp_path, probe=probe,
                                       paused=lambda: False) == 1
    assert probe.calls == ["new.mp4"]
    assert media_info.load(tmp_path) == {
        "media/done.mp4": {"duration_s": 3.0, "display_aspect": 1.3333},
        "media/new.mp4": {"duration_s": 12.5, "display_aspect": 1.7778}}


def test_a_partial_entry_is_completed_and_its_crop_kept(tmp_path):
    _videos(tmp_path, "a.mp4", "b.mp4")
    media_info.update(tmp_path, "media/a.mp4", crop=False)
    media_info.update(tmp_path, "media/b.mp4", display_aspect=1.5)
    probe = FakeProbe()
    assert media_backfill.run_backfill(tmp_path, probe=probe,
                                       paused=lambda: False) == 2
    store = media_info.load(tmp_path)
    assert store["media/a.mp4"] == {
        "duration_s": 12.5, "display_aspect": 1.7778, "crop": False}
    # Only the missing field is filled; a recorded shape is not replaced.
    assert store["media/b.mp4"] == {"duration_s": 12.5, "display_aspect": 1.5}


def test_an_unprobeable_video_is_skipped_and_the_run_continues(tmp_path):
    _videos(tmp_path, "a.mp4", "bad.mp4", "c.mkv")
    probe = FakeProbe({"bad.mp4": None})
    assert media_backfill.run_backfill(tmp_path, probe=probe,
                                       paused=lambda: False) == 2
    assert sorted(probe.calls) == ["a.mp4", "bad.mp4", "c.mkv"]
    store = media_info.load(tmp_path)
    assert "media/bad.mp4" not in store
    assert set(store) == {"media/a.mp4", "media/c.mkv"}


def test_a_probe_that_raises_is_skipped(tmp_path):
    _videos(tmp_path, "a.mp4", "b.mp4")

    def probe(path):
        if Path(path).name == "a.mp4":
            raise RuntimeError("ffprobe exploded")
        return {"duration_s": 1.0, "display_aspect": 1.7778}

    assert media_backfill.run_backfill(tmp_path, probe=probe,
                                       paused=lambda: False) == 1
    assert set(media_info.load(tmp_path)) == {"media/b.mp4"}


def test_an_unknown_or_zero_length_is_not_written(tmp_path):
    _videos(tmp_path, "a.mp4", "b.mp4")
    probe = FakeProbe({"a.mp4": {"duration_s": None, "display_aspect": 1.7778},
                       "b.mp4": {"duration_s": 0.0, "display_aspect": 0.5625}})
    assert media_backfill.run_backfill(tmp_path, probe=probe,
                                       paused=lambda: False) == 2
    assert media_info.load(tmp_path) == {
        "media/a.mp4": {"display_aspect": 1.7778},
        "media/b.mp4": {"display_aspect": 0.5625}}


def test_it_waits_while_playback_has_the_services_paused(tmp_path):
    _videos(tmp_path, "a.mp4")
    states = iter([True, True, False])
    waits: list[float] = []
    probe = FakeProbe()

    def paused():
        state = next(states, False)
        if state:
            assert probe.calls == [], "no probe while paused"
        return state

    assert media_backfill.run_backfill(tmp_path, probe=probe, paused=paused,
                                       wait=waits.append) == 1
    assert len(waits) == 2 and all(w > 0 for w in waits)
    assert probe.calls == ["a.mp4"]


def test_the_kiosk_is_poked_once_at_the_end_only_when_something_was_filled(
        tmp_path):
    _videos(tmp_path, "a.mp4", "b.mp4")
    pokes: list[int] = []
    probe = FakeProbe()
    media_backfill.run_backfill(tmp_path, probe=probe, paused=lambda: False,
                                poke=lambda: pokes.append(len(probe.calls)))
    assert pokes == [2], "one poke, after both probes"

    # A second run finds nothing to do: no probe, no poke.
    probe2 = FakeProbe()
    assert media_backfill.run_backfill(tmp_path, probe=probe2,
                                       paused=lambda: False,
                                       poke=lambda: pokes.append(-1)) == 0
    assert probe2.calls == []
    assert pokes == [2]


def test_nothing_filled_means_no_poke(tmp_path):
    _videos(tmp_path, "bad.mp4")
    pokes: list[int] = []
    assert media_backfill.run_backfill(
        tmp_path, probe=FakeProbe({"bad.mp4": None}), paused=lambda: False,
        poke=lambda: pokes.append(1)) == 0
    assert pokes == []


def test_only_video_files_under_media_are_probed(tmp_path):
    _videos(tmp_path, "a.mp4", "notes.txt", "a.mp4.part", "sub/deep.webm")
    (tmp_path / "media" / "dir.mp4").mkdir()
    (tmp_path / "roms").mkdir()
    (tmp_path / "roms" / "game.mp4").write_bytes(b"x")
    probe = FakeProbe()
    media_backfill.run_backfill(tmp_path, probe=probe, paused=lambda: False)
    assert sorted(probe.calls) == ["a.mp4", "deep.webm"]
    assert set(media_info.load(tmp_path)) == {"media/a.mp4", "media/sub/deep.webm"}


def test_no_media_dir_is_nothing_to_do(tmp_path):
    assert media_backfill.run_backfill(tmp_path, probe=FakeProbe(),
                                       paused=lambda: False) == 0
    assert not (tmp_path / media_info.FILE_NAME).exists()


def test_a_store_write_failure_is_logged_not_raised(tmp_path, monkeypatch):
    _videos(tmp_path, "a.mp4")

    def refuse(*_a, **_k):
        raise OSError(28, "No space left on device")

    monkeypatch.setattr(media_info, "update", refuse)
    pokes: list[int] = []
    assert media_backfill.run_backfill(tmp_path, probe=FakeProbe(),
                                       paused=lambda: False,
                                       poke=lambda: pokes.append(1)) == 0
    assert pokes == []


def test_the_default_pause_is_the_playback_pause_marker():
    assert media_backfill.PAUSE_MARKER == Path("/tmp/mdb_playback_services_paused")


def test_start_runs_once_in_a_daemon_thread_at_nice_10(tmp_path):
    seen = {}
    done = threading.Event()

    def nice(n):
        seen["nice"] = (n, threading.get_ident())

    def run(data_dir, poke=None):
        seen["run"] = (data_dir, poke, threading.get_ident())
        done.set()
        return 0

    def poke():
        pass

    t = media_backfill.start_backfill_thread(tmp_path, poke=poke, run=run,
                                             nice=nice)
    assert t.daemon
    assert done.wait(5)
    t.join(5)
    assert seen["nice"][0] == 10
    # Both in the worker thread, never the caller's (os.nice on Linux is
    # per-thread: the web admin's request threads keep their priority).
    assert seen["nice"][1] == seen["run"][2] != threading.get_ident()
    assert seen["run"][:2] == (Path(tmp_path), poke)


def test_a_crashing_run_does_not_escape_the_thread(tmp_path, capsys):
    def run(data_dir, poke=None):
        raise RuntimeError("boom")

    t = media_backfill.start_backfill_thread(tmp_path, run=run,
                                             nice=lambda n: None)
    t.join(5)
    assert not t.is_alive()
    assert "boom" in capsys.readouterr().err


@pytest.mark.parametrize("env, starts", [("1", True), (None, True), ("0", False)])
def test_create_app_starts_the_backfill_once(temp_data_dir, monkeypatch,
                                             env, starts):
    calls = []
    monkeypatch.setattr(media_backfill, "start_backfill_thread",
                        lambda data_dir, poke=None, **_k: calls.append(
                            (data_dir, poke)))
    if env is None:
        monkeypatch.delenv("MAGIC_MEDIA_BACKFILL", raising=False)
    else:
        monkeypatch.setenv("MAGIC_MEDIA_BACKFILL", env)
    admin.create_app(temp_data_dir, config={"TESTING": True})
    assert len(calls) == (1 if starts else 0)
    if starts:
        data_dir, poke = calls[0]
        assert data_dir == temp_data_dir
        poke()
        assert (temp_data_dir / "media_info_reload_request").exists()


def test_imports_as_a_package_module_without_web_on_sys_path():
    # Production runs `python3 -m magic_dingus_box.web.wsgi`; web/ is not on
    # sys.path there, so a flat-only import of media_info / media_probe would
    # fail on first import.
    r = subprocess.run(
        [sys.executable, "-c",
         "import magic_dingus_box.web.media_backfill as m; "
         "assert m.media_info.__name__ == 'magic_dingus_box.web.media_info'; "
         "assert m.media_probe.__name__ == 'magic_dingus_box.web.media_probe'"],
        cwd=str(REPO), capture_output=True, text=True,
        env={k: v for k, v in os.environ.items() if k != "PYTHONPATH"})
    assert r.returncode == 0, r.stderr


def test_a_real_video_is_probed_with_ffprobe(tmp_path):
    import shutil
    if not (shutil.which("ffmpeg") and shutil.which("ffprobe")):
        pytest.skip("ffmpeg/ffprobe not installed")
    (tmp_path / "media").mkdir()
    out = tmp_path / "media" / "wide.mp4"
    r = subprocess.run(
        ["ffmpeg", "-v", "error", "-f", "lavfi",
         "-i", "testsrc=size=320x180:rate=10", "-t", "1",
         "-pix_fmt", "yuv420p", str(out)],
        capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    assert media_backfill.run_backfill(tmp_path, paused=lambda: False) == 1
    entry = media_info.load(tmp_path)["media/wide.mp4"]
    assert entry["display_aspect"] == pytest.approx(16 / 9, abs=1e-3)
    assert entry["duration_s"] == pytest.approx(1.0, abs=0.2)
