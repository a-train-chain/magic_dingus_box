"""device_info.json shape is enforced on every way in, and tolerated on read.

A restored device_info.json that was valid JSON but not an OBJECT (a list, a
string) was written as-is, and from then on /admin/device/info — the very
first call the Content Manager makes to find the box — returned 500 forever
(`info['hostname'] = ...` on a list). /admin/device/name accepted any JSON
type and any length as the name.
"""
from __future__ import annotations

import io
import json
import zipfile

import pytest


def _restore(client, entries: dict):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        for name, data in entries.items():
            zf.writestr(name, data)
    buf.seek(0)
    return client.post("/admin/restore", data={"file": (buf, "backup.zip")},
                       content_type="multipart/form-data")


@pytest.mark.parametrize("doc", ["[1, 2]", '"just a string"', "42",
                                 '{"device_name": ["x"]}',
                                 '{"device_name": "' + "n" * 500 + '"}'])
def test_restore_rejects_non_object_device_info(client, temp_data_dir, doc):
    resp = _restore(client, {"data/device_info.json": doc})
    assert resp.status_code == 200
    data = resp.get_json()["data"]
    assert data["restored"]["device_info"] is False
    assert data["warnings"]
    assert client.get("/admin/device/info").status_code == 200


def test_restore_rejects_non_object_settings(client, temp_data_dir):
    resp = _restore(client, {"config/settings.json": "[]"})
    assert resp.get_json()["data"]["restored"]["settings"] is False


def test_a_bad_device_info_already_on_disk_no_longer_500s(client, temp_data_dir):
    (temp_data_dir / "device_info.json").write_text("[1, 2, 3]")
    resp = client.get("/admin/device/info")
    assert resp.status_code == 200
    assert isinstance(resp.get_json()["data"]["device_name"], str)
    # And a rename heals the file.
    assert client.post("/admin/device/name", json={"name": "Den"}).status_code == 200
    assert json.loads((temp_data_dir / "device_info.json").read_text())["device_name"] == "Den"


@pytest.mark.parametrize("name", [123, ["Den"], {"n": 1}, None, "", "   ",
                                  "x" * 65, "bad\x00name", "line\nbreak"])
def test_set_device_name_validates(client, name):
    resp = client.post("/admin/device/name", json={"name": name})
    assert resp.status_code == 400


def test_set_device_name_rejects_non_object_body(client):
    assert client.post("/admin/device/name", json=["Den"]).status_code == 400


def test_set_device_name_trims_and_keeps_device_id(client, temp_data_dir):
    (temp_data_dir / "device_info.json").write_text(
        json.dumps({"device_id": "abc", "device_name": "Old"}))
    resp = client.post("/admin/device/name", json={"name": "  Living Room  "})
    assert resp.status_code == 200
    info = json.loads((temp_data_dir / "device_info.json").read_text())
    assert info == {"device_id": "abc", "device_name": "Living Room"}
