"""ZIP text entries are read with a hard size cap.

restore_backup used zf.read() and import_package pf.read() on entries whose
size the uploader controls. Deflate compresses a run of spaces ~1000:1, so a
few-hundred-KB upload could make the web service allocate gigabytes — on a
1.5 GB Pi 4B, the OOM killer picks the kiosk or the web service. Every entry
is now checked against its declared size AND read with a bounded read().
"""
from __future__ import annotations

import io
import json
import zipfile
from pathlib import Path

import pytest

import admin


BIG = admin._ZIP_TEXT_ENTRY_MAX + 1


def _zip(entries: dict) -> io.BytesIO:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        for name, data in entries.items():
            zf.writestr(name, data)
    buf.seek(0)
    return buf


def _bomb_yaml() -> bytes:
    return b"title: x\nitems: []\n#" + b" " * BIG


def test_cap_is_a_few_megabytes():
    assert 1 * 1024 * 1024 <= admin._ZIP_TEXT_ENTRY_MAX <= 16 * 1024 * 1024


def test_restore_skips_an_oversized_playlist_and_keeps_the_rest(client, temp_data_dir):
    buf = _zip({
        "playlists/huge.yaml": _bomb_yaml(),
        "playlists/ok.yaml": b"title: ok\nitems: []\n",
    })
    resp = client.post("/admin/restore", data={"file": (buf, "backup.zip")},
                       content_type="multipart/form-data")
    assert resp.status_code == 200
    body = resp.get_json()
    assert body["data"]["restored"]["playlists"] == ["ok.yaml"]
    assert any("too large" in w for w in body["data"]["warnings"])
    assert not (temp_data_dir / "playlists" / "huge.yaml").exists()


def test_restore_refuses_oversized_settings_and_device_info(client, temp_data_dir):
    pad = " " * BIG
    buf = _zip({
        "config/settings.json": '{"a": 1}' + pad,
        "data/device_info.json": '{"device_name": "x"}' + pad,
        "manifest.json": "{}" + pad,
    })
    resp = client.post("/admin/restore", data={"file": (buf, "backup.zip")},
                       content_type="multipart/form-data")
    assert resp.status_code == 200
    data = resp.get_json()["data"]
    assert data["restored"]["settings"] is False
    assert data["restored"]["device_info"] is False
    assert sum("too large" in w for w in data["warnings"]) == 3


def test_import_package_refuses_an_oversized_playlist_yaml(client):
    buf = _zip({"playlist.yaml": _bomb_yaml()})
    resp = client.post("/admin/playlists/import-package",
                       data={"file": (buf, "pkg.zip")},
                       content_type="multipart/form-data")
    assert resp.status_code == 400
    assert "too large" in resp.get_json()["error"]["message"].lower()


def test_helper_checks_declared_size_and_reads_bounded():
    buf = _zip({"a.txt": b"y" * 5000})
    with zipfile.ZipFile(buf) as zf:
        with pytest.raises(admin._ZipEntryTooLarge):
            admin._read_zip_entry_capped(zf, "a.txt", cap=100)
        assert admin._read_zip_entry_capped(zf, "a.txt", cap=5000) == b"y" * 5000
        # A central directory that UNDER-declares the size passes the header
        # check, but the bounded read still never yields more than the cap.
        zf.getinfo("a.txt").file_size = 10
        try:
            data = admin._read_zip_entry_capped(zf, "a.txt", cap=100)
        except (admin._ZipEntryTooLarge, zipfile.BadZipFile):
            data = b""
        assert len(data) <= 100
