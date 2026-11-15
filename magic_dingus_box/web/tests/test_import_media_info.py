"""Package import and the per-video store: every imported video is probed
into media_info.json, and a package-root media_info.json carries its Crop
settings by basename."""
from __future__ import annotations

import io
import json
import zipfile

import media_info
import media_probe


def _package(extra=None, prefix="", video="clip.mp4", item_path=None):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr(prefix + "playlist.yaml",
                    "title: Pkg\nitems:\n- title: Clip\n"
                    f"  path: {item_path or 'media/' + video}\n"
                    "  source_type: local\n")
        zf.writestr(prefix + "media/" + video, b"not really a video")
        for name, content in (extra or {}).items():
            zf.writestr(name, content)
    buf.seek(0)
    return buf


def _import(client, buf):
    return client.post("/admin/playlists/import-package",
                       data={"file": (buf, "pkg.zip")},
                       content_type="multipart/form-data")


def _fake_probe(monkeypatch):
    monkeypatch.setattr(media_probe, "probe_source",
                        lambda path: {"duration_s": 12.5, "display_aspect": 1.7778})


def test_import_without_media_info_still_probes_each_video(client, temp_data_dir,
                                                           monkeypatch):
    _fake_probe(monkeypatch)
    resp = _import(client, _package())
    assert resp.status_code == 200, resp.get_json()
    entry = media_info.load(temp_data_dir)["media/clip.mp4"]
    assert entry["duration_s"] == 12.5
    assert entry["display_aspect"] == 1.7778
    assert "crop" not in entry
    assert (temp_data_dir / "media_info_reload_request").exists()


def test_import_merges_crop_from_the_packages_media_info(client, temp_data_dir,
                                                         monkeypatch):
    _fake_probe(monkeypatch)
    doc = {"version": 1, "videos": {"clip.mp4": {"crop": False},
                                    "other.mp4": {"crop": False}}}
    resp = _import(client, _package({"media_info.json": json.dumps(doc)}))
    assert resp.status_code == 200, resp.get_json()
    store = media_info.load(temp_data_dir)
    assert store["media/clip.mp4"]["crop"] is False
    assert store["media/clip.mp4"]["duration_s"] == 12.5
    # Entries for videos the package did not carry are not imported.
    assert "media/other.mp4" not in store


def test_import_with_an_unprobeable_video_still_succeeds(client, temp_data_dir,
                                                         monkeypatch):
    monkeypatch.setattr(media_probe, "probe_source", lambda path: None)
    resp = _import(client, _package())
    assert resp.status_code == 200
    assert "media/clip.mp4" not in media_info.load(temp_data_dir)


def test_a_malformed_package_media_info_is_ignored(client, temp_data_dir,
                                                   monkeypatch):
    _fake_probe(monkeypatch)
    resp = _import(client, _package({"media_info.json": "{nope"}))
    assert resp.status_code == 200
    assert media_info.load(temp_data_dir)["media/clip.mp4"]["duration_s"] == 12.5


def test_nested_package_media_info_beside_the_playlist_is_used(client, temp_data_dir,
                                                               monkeypatch):
    _fake_probe(monkeypatch)
    doc = {"videos": {"clip.mp4": {"crop": False}}}
    pkg = _package({"MyPkg/media_info.json": json.dumps(doc)}, prefix="MyPkg/")
    assert _import(client, pkg).status_code == 200
    assert media_info.load(temp_data_dir)["media/clip.mp4"]["crop"] is False


def test_package_key_that_is_a_path_lands_on_the_imported_key(client, temp_data_dir,
                                                              monkeypatch):
    _fake_probe(monkeypatch)
    doc = {"videos": {"media/clip.mp4": {"crop": False}}}
    assert _import(client, _package({"media_info.json": json.dumps(doc)})
                   ).status_code == 200
    assert media_info.load(temp_data_dir)["media/clip.mp4"]["crop"] is False


def test_package_key_is_matched_after_sanitising(client, temp_data_dir,
                                                 monkeypatch):
    _fake_probe(monkeypatch)
    doc = {"videos": {"clip's.mp4": {"crop": False}}}
    pkg = _package({"media_info.json": json.dumps(doc)}, video="clip's.mp4")
    assert _import(client, pkg).status_code == 200
    # Saved as clips.mp4 (the apostrophe is dropped).
    assert media_info.load(temp_data_dir)["media/clips.mp4"]["crop"] is False
