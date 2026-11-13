"""The web side of <data>/media_info.json, shared with the kiosk.

The fixture is also loaded by magic_dingus_box_cpp/tests/app/test_media_info.cpp;
the values asserted here are the ones that test asserts.
"""
from __future__ import annotations

import fcntl
import json
import threading
import time
from pathlib import Path

import pytest

import media_info

REPO = Path(__file__).resolve().parents[3]
FIXTURE = REPO / "magic_dingus_box_cpp" / "tests" / "app" / "fixtures" / "media_info.json"


def test_shared_fixture_loads_to_the_kiosks_values(tmp_path):
    (tmp_path / "media_info.json").write_text(FIXTURE.read_text())
    m = media_info.load(tmp_path)
    assert m["media/classic.mp4"] == {"duration_s": 212.4, "display_aspect": 1.3333}
    assert m["media/widescreen.mp4"] == {
        "duration_s": 95.5, "display_aspect": 1.7778, "crop": False}
    assert m["media/not_probed_yet.mp4"] == {"crop": False}
    assert m["media/music videos/long one.mp4"] == {"duration_s": 3600}


@pytest.mark.parametrize("content", [None, "", " \n", "{oops", "[]",
                                     '{"version":1,"videos":[]}'])
def test_load_of_missing_empty_or_corrupt_is_empty(tmp_path, content):
    if content is not None:
        (tmp_path / "media_info.json").write_text(content)
    assert media_info.load(tmp_path) == {}


def test_load_treats_wrong_typed_fields_as_absent(tmp_path):
    (tmp_path / "media_info.json").write_text(json.dumps({"version": 1, "videos": {
        "media/a.mp4": {"crop": "no", "duration_s": -3, "display_aspect": 1.5},
        "media/b.mp4": 7,
        "media/c.mp4": {"crop": False}}}))
    m = media_info.load(tmp_path)
    assert m["media/a.mp4"] == {"display_aspect": 1.5}
    assert "media/b.mp4" not in m
    assert m["media/c.mp4"] == {"crop": False}


def test_update_keeps_other_keys_and_fields_including_unknown(tmp_path):
    (tmp_path / "media_info.json").write_text(json.dumps({"version": 1, "videos": {
        "media/a.mp4": {"duration_s": 10, "future": "x"},
        "media/b.mp4": {"crop": False}}}))
    media_info.update(tmp_path, "media/a.mp4", crop=False)
    raw = json.loads((tmp_path / "media_info.json").read_text())
    assert raw["version"] == 1
    assert raw["videos"]["media/a.mp4"] == {
        "duration_s": 10, "future": "x", "crop": False}
    assert raw["videos"]["media/b.mp4"] == {"crop": False}


def test_update_none_removes_field_and_empty_entry(tmp_path):
    media_info.update(tmp_path, "media/a.mp4", crop=False, duration_s=5.5)
    media_info.update(tmp_path, "media/a.mp4", crop=None)
    assert media_info.load(tmp_path) == {"media/a.mp4": {"duration_s": 5.5}}
    media_info.update(tmp_path, "media/a.mp4", duration_s=None)
    assert media_info.load(tmp_path) == {}


def test_update_creates_data_dir_file_and_remove_deletes_entry(tmp_path):
    d = tmp_path / "fresh"
    media_info.update(d, "media/a.mp4", display_aspect=1.5)
    media_info.update(d, "media/b.mp4", crop=False)
    media_info.remove(d, "media/a.mp4")
    media_info.remove(d, "media/never.mp4")
    assert media_info.load(d) == {"media/b.mp4": {"crop": False}}


def test_media_key_is_lexical_and_none_outside(tmp_path):
    assert media_info.media_key(tmp_path / "media" / "a.mp4", tmp_path) == "media/a.mp4"
    assert media_info.media_key(
        tmp_path / "media" / "x y" / ".." / "a.mp4", tmp_path) == "media/a.mp4"
    assert media_info.media_key(Path(str(tmp_path) + "/"), tmp_path) is None
    assert media_info.media_key(tmp_path.parent / "other.mp4", tmp_path) is None
    assert media_info.media_key(tmp_path / ".." / "x.mp4", tmp_path) is None


def test_media_key_does_not_resolve_symlinks(tmp_path):
    real = tmp_path / "elsewhere"
    real.mkdir()
    data = tmp_path / "data"
    data.mkdir()
    (data / "media").symlink_to(real)
    assert media_info.media_key(data / "media" / "a.mp4", data) == "media/a.mp4"


def test_update_waits_for_the_sidecar_lock_across_read_and_write(tmp_path):
    lock_fd = (tmp_path / "media_info.json.lock").open("a+")
    fcntl.flock(lock_fd, fcntl.LOCK_EX)
    released = {}

    def release():
        time.sleep(0.2)
        released["at"] = time.monotonic()
        fcntl.flock(lock_fd, fcntl.LOCK_UN)

    t = threading.Thread(target=release)
    t.start()
    media_info.update(tmp_path, "media/a.mp4", crop=False)
    returned = time.monotonic()
    t.join()
    lock_fd.close()
    assert returned >= released["at"]
    assert media_info.load(tmp_path) == {"media/a.mp4": {"crop": False}}


def test_imports_as_a_package_module_without_web_on_sys_path():
    # Production runs `python3 -m magic_dingus_box.web.wsgi`; web/ is not on
    # sys.path there (conftest adds it for the tests), so a flat-only
    # `from admin_common import ...` would fail on first import.
    import subprocess
    import sys
    r = subprocess.run(
        [sys.executable, "-c", "import magic_dingus_box.web.media_info"],
        cwd=str(REPO), capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
