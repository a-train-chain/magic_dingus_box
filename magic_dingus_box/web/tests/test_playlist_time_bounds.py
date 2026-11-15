"""Playlist item start/end must never carry YAML into the written file.

format_playlist_yaml writes start/end unquoted. A JSON string value with a
newline used to inject arbitrary keys — e.g. an emulator_core the
_invalid_emulator_core guard never saw, because the guard reads the JSON
while the kiosk reads the YAML. Every path that feeds format_playlist_yaml
(the JSON save, the single-YAML import, the package import) now refuses a
start/end that is not a finite, non-negative number with VALIDATION_ERROR,
before anything is written or extracted.
"""
from __future__ import annotations

import io
import json
import zipfile

import pytest
import yaml

from admin_playlists import format_playlist_yaml

INJECT = "0\n    emulator_core: ../../../tmp/evil"
BAD = [INJECT, "abc", -1, "-5", float("inf"), "nan", True, [1], {"a": 1}]


def _playlist(**bounds):
    item = {"title": "clip", "source_type": "local", "path": "media/clip.mp4"}
    item.update(bounds)
    return {"title": "Mix", "items": [item]}


def _assert_validation_error(rv):
    assert rv.status_code == 400, rv.get_data(as_text=True)
    body = rv.get_json()
    assert body["ok"] is False
    assert body["error"]["code"] == "VALIDATION_ERROR"


@pytest.mark.parametrize("key", ["start", "end"])
@pytest.mark.parametrize("value", BAD, ids=repr)
def test_json_save_refuses_bad_bound(client, temp_data_dir, key, value):
    rv = client.post("/admin/playlists/mix.yaml",
                     data=json.dumps(_playlist(**{key: value})),
                     content_type="application/json")
    _assert_validation_error(rv)
    assert not (temp_data_dir / "playlists" / "mix.yaml").exists()


@pytest.mark.parametrize("value,written", [
    (0, 0), (236, 236), (12.5, 12.5), ("42", 42), (" 7.25 ", 7.25)])
def test_json_save_writes_a_plain_number(client, temp_data_dir, value, written):
    rv = client.post("/admin/playlists/mix.yaml",
                     data=json.dumps(_playlist(start=value, end=value)),
                     content_type="application/json")
    assert rv.status_code == 200, rv.get_json()
    doc = yaml.safe_load((temp_data_dir / "playlists" / "mix.yaml").read_text())
    assert doc["items"][0]["start"] == written
    assert doc["items"][0]["end"] == written


def test_absent_or_empty_bounds_are_omitted():
    out = yaml.safe_load(format_playlist_yaml(_playlist(start=None, end="")))
    assert "start" not in out["items"][0] and "end" not in out["items"][0]


def test_formatter_never_emits_injected_keys():
    with pytest.raises(ValueError):
        format_playlist_yaml(_playlist(start=INJECT))


def test_yaml_import_refuses_bad_bound(client, temp_data_dir):
    doc = yaml.safe_dump(_playlist(end=INJECT)).encode()
    rv = client.post("/admin/playlists/import",
                     data={"file": (io.BytesIO(doc), "mix.yaml")},
                     content_type="multipart/form-data")
    _assert_validation_error(rv)
    assert not list((temp_data_dir / "playlists").glob("*.yaml"))


def test_package_import_refuses_bad_bound_before_extracting(client, temp_data_dir):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr("playlist.yaml", yaml.safe_dump(_playlist(start=INJECT)))
        zf.writestr("media/clip.mp4", b"video")
        zf.writestr("roms/nes/g.nes", b"rom")
    buf.seek(0)
    rv = client.post("/admin/playlists/import-package",
                     data={"file": (buf, "p.zip")},
                     content_type="multipart/form-data")
    _assert_validation_error(rv)
    assert not (temp_data_dir / "media" / "clip.mp4").exists()
    assert not (temp_data_dir / "roms" / "nes").exists()
    assert not list((temp_data_dir / "playlists").glob("*.yaml"))
