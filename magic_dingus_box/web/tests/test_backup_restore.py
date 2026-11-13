"""Backup/restore settings.json path regression tests.

The kiosk reads settings.json from <base>/config — TWO levels up from
data_dir (production: /opt/magic_dingus_box/config, the same base the
VERSION lookup uses). Backup and restore used to derive the path as
data_dir.parent / "config" (= magic_dingus_box_cpp/config), a directory
the kiosk never touches: settings were silently omitted from every
backup ZIP ever produced, and a restore wrote settings.json somewhere
nothing would read it while reporting success. These tests pin the
corrected path — and pin that the legacy wrong path stays dead.
"""
from __future__ import annotations

import io
import json
import shutil
import zipfile
from pathlib import Path

import pytest


@pytest.fixture
def kiosk_config_dir(temp_data_dir: Path):
    """The kiosk's real config dir for the fixture layout.

    Mirrors production /opt/magic_dingus_box/config: two levels above
    data_dir. That places it OUTSIDE the fixture's TemporaryDirectory
    (same as the VERSION file the conftest already writes there), so it
    is created empty here and removed afterwards to keep runs hermetic.
    """
    d = temp_data_dir.parent.parent / "config"
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True)
    yield d
    shutil.rmtree(d, ignore_errors=True)


def test_backup_includes_settings_from_kiosk_config_dir(client, kiosk_config_dir):
    (kiosk_config_dir / "settings.json").write_text(
        json.dumps({"playback": {"volume": 55}}))

    resp = client.get("/admin/backup")
    assert resp.status_code == 200

    zf = zipfile.ZipFile(io.BytesIO(resp.data))
    assert "config/settings.json" in zf.namelist()
    manifest = json.loads(zf.read("manifest.json"))
    assert manifest["contents"]["settings"] is True
    assert json.loads(zf.read("config/settings.json")) == {
        "playback": {"volume": 55}}


def test_backup_ignores_legacy_wrong_config_dir(client, temp_data_dir,
                                                kiosk_config_dir):
    # A settings.json at the OLD wrong location (data_dir.parent/config)
    # must not satisfy the backup — only the kiosk's real path counts.
    legacy = temp_data_dir.parent / "config"
    legacy.mkdir(parents=True, exist_ok=True)
    (legacy / "settings.json").write_text("{}")

    resp = client.get("/admin/backup")
    assert resp.status_code == 200

    manifest = json.loads(
        zipfile.ZipFile(io.BytesIO(resp.data)).read("manifest.json"))
    assert manifest["contents"]["settings"] is False


def test_restore_writes_settings_to_kiosk_config_dir(client, temp_data_dir,
                                                     kiosk_config_dir):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr("config/settings.json",
                    json.dumps({"playback": {"volume": 77}}))
    buf.seek(0)

    resp = client.post(
        "/admin/restore",
        data={"file": (buf, "backup.zip")},
        content_type="multipart/form-data")
    assert resp.status_code == 200

    restored = kiosk_config_dir / "settings.json"
    assert restored.exists(), "settings.json must land in the kiosk's config dir"
    assert json.loads(restored.read_text()) == {"playback": {"volume": 77}}
    # And nothing may resurrect the legacy wrong path.
    assert not (temp_data_dir.parent / "config" / "settings.json").exists()


def test_restore_pokes_the_kiosk_to_reload_settings(client, temp_data_dir,
                                                    kiosk_config_dir):
    """The kiosk polls data/settings_reload_request (~1s) and re-reads
    settings.json when it appears — without the poke, its next
    operator-action save clobbers the restore with stale memory."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr("config/settings.json", json.dumps({"playback": {}}))
    buf.seek(0)

    resp = client.post(
        "/admin/restore",
        data={"file": (buf, "backup.zip")},
        content_type="multipart/form-data")
    assert resp.status_code == 200
    assert (temp_data_dir / "settings_reload_request").exists()


def test_restore_without_settings_does_not_poke(client, temp_data_dir,
                                                kiosk_config_dir):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr("playlists/mix.yaml", "title: Mix\nitems: []\n")
    buf.seek(0)

    resp = client.post(
        "/admin/restore",
        data={"file": (buf, "backup.zip")},
        content_type="multipart/form-data")
    assert resp.status_code == 200
    assert not (temp_data_dir / "settings_reload_request").exists()


# ---- media_info.json: per-video store travels with the backup ----

def test_backup_includes_media_info_when_present(client, temp_data_dir,
                                                 kiosk_config_dir):
    doc = {"version": 1, "videos": {"media/x.mp4": {"crop": False}}}
    (temp_data_dir / "media_info.json").write_text(json.dumps(doc))

    resp = client.get("/admin/backup")
    zf = zipfile.ZipFile(io.BytesIO(resp.data))
    assert "data/media_info.json" in zf.namelist()
    assert json.loads(zf.read("data/media_info.json")) == doc
    assert json.loads(zf.read("manifest.json"))["contents"]["media_info"] is True


def test_backup_without_media_info_omits_it(client, kiosk_config_dir):
    zf = zipfile.ZipFile(io.BytesIO(client.get("/admin/backup").data))
    assert "data/media_info.json" not in zf.namelist()
    assert json.loads(zf.read("manifest.json"))["contents"]["media_info"] is False


def _restore(client, files):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        for name, content in files.items():
            zf.writestr(name, content)
    buf.seek(0)
    return client.post("/admin/restore",
                       data={"file": (buf, "backup.zip")},
                       content_type="multipart/form-data")


def test_restore_writes_media_info_and_pokes_the_kiosk(client, temp_data_dir,
                                                       kiosk_config_dir):
    doc = {"version": 1, "videos": {"media/x.mp4": {"crop": False}}}
    resp = _restore(client, {"data/media_info.json": json.dumps(doc)})
    assert resp.status_code == 200
    assert json.loads((temp_data_dir / "media_info.json").read_text()) == doc
    assert (temp_data_dir / "media_info_reload_request").exists()
    assert resp.get_json()["data"]["restored"]["media_info"] is True


def test_restore_rejects_a_media_info_that_is_not_an_object(client, temp_data_dir,
                                                            kiosk_config_dir):
    resp = _restore(client, {"data/media_info.json": "[1, 2]"})
    assert resp.status_code == 200
    assert not (temp_data_dir / "media_info.json").exists()
    assert not (temp_data_dir / "media_info_reload_request").exists()
    assert resp.get_json()["data"]["errors"]
