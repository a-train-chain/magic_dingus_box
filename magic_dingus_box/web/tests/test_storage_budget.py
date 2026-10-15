"""Free-space budgets must count the copies an upload really makes.

Werkzeug spools every multipart file over 500 KB to a temp file on the card
(TMPDIR is the SD card on the box), and that spool lives until the request
ends. So:
  * /admin/upload: spool + the staged copy in data/media = TWO copies, not
    the one its comment claimed.
  * /admin/playlists/import-package: the spool, plus a SECOND copy of the
    ZIP (file.save to a mkstemp, or the whole thing read into RAM under
    100 MB), plus the extracted media = THREE, against a 2x budget. It now
    opens the ZIP straight from the spool, so 2x is the truth.
The 512 MB reserve on top is unchanged.
"""
from __future__ import annotations

import io
import zipfile

import pytest
import yaml

import admin

MB = 1024 * 1024


def test_plain_upload_budgets_spool_plus_staged_copy(client, temp_data_dir, monkeypatch):
    body = b"x" * (4 * MB)
    # Room for ONE copy and a half beyond the reserve: enough under the old
    # one-copy budget, not enough for the two copies that really coexist.
    monkeypatch.setattr(admin, "get_free_bytes",
                        lambda _p: 512 * MB + int(len(body) * 1.5))
    resp = client.post("/admin/upload",
                       data={"file": (io.BytesIO(body), "clip.mp4")},
                       content_type="multipart/form-data")
    assert resp.status_code == 507, resp.get_data(as_text=True)[:200]
    assert not (temp_data_dir / "media" / "clip.mp4").exists()


def test_plain_upload_still_fits_with_room_for_two_copies(client, temp_data_dir, monkeypatch):
    body = b"x" * (4 * MB)
    monkeypatch.setattr(admin, "get_free_bytes",
                        lambda _p: 512 * MB + int(len(body) * 2.5))
    resp = client.post("/admin/upload",
                       data={"file": (io.BytesIO(body), "clip.mp4")},
                       content_type="multipart/form-data")
    assert resp.status_code == 200, resp.get_data(as_text=True)[:200]


def _package(size: int) -> io.BytesIO:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as zf:
        zf.writestr("playlist.yaml", yaml.safe_dump({
            "title": "Budget Mix",
            "items": [{"title": "a", "source_type": "local",
                       "path": "media/a.mp4", "start": 0}],
        }))
        zf.writestr("media/a.mp4", b"v" * size)
    buf.seek(0)
    return buf


@pytest.mark.parametrize("size", [64 * 1024, 2 * MB], ids=["small", "spooled"])
def test_import_package_reads_the_zip_without_a_second_copy(
        client, temp_data_dir, monkeypatch, size):
    staged = []
    real_mkstemp = admin.tempfile.mkstemp

    def spy(*a, **k):
        if k.get("suffix") == ".zip":
            staged.append(1)
        return real_mkstemp(*a, **k)

    monkeypatch.setattr(admin.tempfile, "mkstemp", spy)
    package = _package(size)  # built BEFORE the ZipFile spy below

    opened_from = []
    real_zipfile = admin.zipfile.ZipFile

    def zip_spy(file, *a, **k):
        opened_from.append(file)
        return real_zipfile(file, *a, **k)

    monkeypatch.setattr(admin.zipfile, "ZipFile", zip_spy)

    resp = client.post("/admin/playlists/import-package",
                       data={"file": (package, "pkg.zip")},
                       content_type="multipart/form-data")
    assert resp.status_code == 200, resp.get_data(as_text=True)[:300]
    assert staged == [], "the ZIP must be opened from the upload spool, not copied"
    # Not a path to a staged copy, and not a BytesIO holding a full read()
    # of the upload (the old <100 MB "RAM path": another whole copy, in RAM).
    assert opened_from and not isinstance(opened_from[0], (str, bytes, io.BytesIO))
    assert (temp_data_dir / "media" / "a.mp4").read_bytes() == b"v" * size
