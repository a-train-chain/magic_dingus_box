"""Per-video Crop in the Content Manager: list fields, the toggle route and
the store entry's removal on delete."""
from __future__ import annotations

import json
from pathlib import Path

import media_info


def _video(data_dir: Path, name="x.mp4") -> Path:
    p = data_dir / "media" / name
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(b"v")
    return p


def _marker(data_dir: Path) -> Path:
    return data_dir / "media_info_reload_request"


def _post(client, path, crop):
    return client.post("/admin/media/picture", json={"path": path, "crop": crop})


def test_list_reports_crop_true_by_default_and_aspect(client, temp_data_dir):
    _video(temp_data_dir)
    row = client.get("/admin/media").get_json()["data"][0]
    assert row["crop"] is True
    assert row["display_aspect"] is None
    media_info.update(temp_data_dir, "media/x.mp4", display_aspect=1.7778)
    media_info.update(temp_data_dir, "media/x.mp4", crop=False)
    row = client.get("/admin/media").get_json()["data"][0]
    assert row["crop"] is False
    assert row["display_aspect"] == 1.7778


def test_route_sets_and_clears_crop(client, temp_data_dir):
    _video(temp_data_dir)
    resp = _post(client, "data/media/x.mp4", False)
    assert resp.status_code == 200
    assert resp.get_json()["data"] == {"crop": False}
    assert media_info.load(temp_data_dir)["media/x.mp4"]["crop"] is False
    assert _marker(temp_data_dir).exists()
    _marker(temp_data_dir).unlink()
    resp = _post(client, "data/media/x.mp4", True)
    assert resp.get_json()["data"] == {"crop": True}
    assert "crop" not in media_info.load(temp_data_dir).get("media/x.mp4", {})
    assert _marker(temp_data_dir).exists()


def test_path_outside_media_is_rejected(client, temp_data_dir):
    (temp_data_dir / "playlists").mkdir(exist_ok=True)
    (temp_data_dir / "playlists" / "x.mp4").write_bytes(b"v")
    for bad in ("data/playlists/x.mp4", "data/media/../playlists/x.mp4",
                "../etc/passwd", "/etc/passwd"):
        resp = _post(client, bad, False)
        assert resp.status_code == 400, bad
        assert resp.get_json()["error"]["code"] == "VALIDATION_ERROR"
    assert not (temp_data_dir / media_info.FILE_NAME).exists()


def test_crop_must_be_a_boolean(client, temp_data_dir):
    _video(temp_data_dir)
    resp = _post(client, "data/media/x.mp4", "no")
    assert resp.status_code == 400
    assert resp.get_json()["error"]["code"] == "VALIDATION_ERROR"


def test_clearing_the_only_field_leaves_no_empty_entry(client, temp_data_dir):
    _video(temp_data_dir)
    media_info.update(temp_data_dir, "media/x.mp4", crop=False)
    media_info.update(temp_data_dir, "media/x.mp4", crop=None)
    doc = json.loads((temp_data_dir / media_info.FILE_NAME).read_text())
    assert "media/x.mp4" not in doc["videos"]


def test_missing_video_is_404(client, temp_data_dir):
    (temp_data_dir / "media").mkdir(exist_ok=True)
    assert _post(client, "data/media/nope.mp4", False).status_code == 404


def test_store_failure_is_a_clear_error_not_a_500(client, temp_data_dir, monkeypatch):
    _video(temp_data_dir)

    def boom(*a, **k):
        raise OSError("disk full")
    monkeypatch.setattr(media_info, "update", boom)
    resp = _post(client, "data/media/x.mp4", False)
    assert resp.status_code == 500
    assert resp.get_json()["ok"] is False


def test_delete_removes_the_store_entry(client, temp_data_dir):
    _video(temp_data_dir)
    media_info.update(temp_data_dir, "media/x.mp4", crop=False, duration_s=3.0)
    assert client.delete("/admin/media/data/media/x.mp4").status_code == 200
    assert "media/x.mp4" not in media_info.load(temp_data_dir)


def test_delete_tolerates_a_store_error(client, temp_data_dir, monkeypatch):
    _video(temp_data_dir)

    def boom(*a, **k):
        raise OSError("read-only")
    monkeypatch.setattr(media_info, "remove", boom)
    assert client.delete("/admin/media/data/media/x.mp4").status_code == 200
    assert not (temp_data_dir / "media" / "x.mp4").exists()
