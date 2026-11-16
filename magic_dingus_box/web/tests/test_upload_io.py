"""Upload writes that leave the SD card to the kiosk (upload_io).

A 2.57 GB upload froze Channel Guide playback for seconds at a time on a
Pi 5 (2026-11-15): the request body was spooled to the SD card, then
copied AGAIN beside it before ffmpeg started, both as fast as the card
would take them, while the kiosk read its video from the same card.

These tests pin the three halves of the fix:
  * the spool is written in bounded slices that are pushed to the card
    and dropped from the page cache as it goes (SpoolFile),
  * the upload routes hand that spool on by hard link instead of copying
    it (save_upload),
  * the upload thread's I/O runs in the idle class while it writes
    (idle_io).
"""
from __future__ import annotations

import io
import os
import sys
from pathlib import Path

import pytest
from flask import Flask, request
from werkzeug.datastructures import FileStorage

sys.path.insert(0, str(Path(__file__).parent.parent))

import upload_io  # noqa: E402
from upload_io import SpoolFile, save_upload  # noqa: E402

MiB = 1024 * 1024


@pytest.fixture
def flushes(monkeypatch):
    """Record every write-back and cache drop instead of calling the OS."""
    calls = []
    monkeypatch.setattr(upload_io, "_writeback",
                        lambda fd, off, n: calls.append(("wb", off, n)))
    monkeypatch.setattr(upload_io, "_drop_cache",
                        lambda fd, off, n: calls.append(("drop", off, n)))
    return calls


# ---------------------------------------------------------------- SpoolFile

def test_spool_keeps_every_byte_and_reads_back(tmp_path):
    data = os.urandom(3 * MiB + 123)
    spool = SpoolFile(tmp_path, flush_every=MiB)
    # werkzeug hands over bytes, but accept any buffer
    spool.write(data[:1000])
    spool.write(memoryview(data)[1000:2 * MiB])
    spool.write(bytearray(data[2 * MiB:]))
    spool.seek(0)
    assert spool.read() == data
    spool.close()


def test_spool_is_pushed_to_the_card_in_bounded_slices(tmp_path, flushes):
    spool = SpoolFile(tmp_path, flush_every=MiB)
    chunk = b"x" * (64 * 1024)
    for _ in range(40):            # 2.5 MiB, in werkzeug-sized writes
        spool.write(chunk)
    spool.close()
    # Each slice is written back, then dropped from the page cache; the
    # last half slice is left to the kernel (nothing waits on it).
    assert flushes == [
        ("wb", 0, MiB), ("drop", 0, MiB),
        ("wb", MiB, MiB), ("drop", MiB, MiB),
    ]


def test_flush_every_zero_turns_pacing_off(tmp_path, flushes):
    spool = SpoolFile(tmp_path, flush_every=0)
    spool.write(b"x" * (3 * MiB))
    spool.close()
    assert flushes == []


def test_flush_interval_comes_from_the_environment(monkeypatch):
    monkeypatch.delenv("MAGIC_UPLOAD_FLUSH_MIB", raising=False)
    assert upload_io.flush_every_bytes() == upload_io.DEFAULT_FLUSH_MIB * MiB
    monkeypatch.setenv("MAGIC_UPLOAD_FLUSH_MIB", "4")
    assert upload_io.flush_every_bytes() == 4 * MiB
    monkeypatch.setenv("MAGIC_UPLOAD_FLUSH_MIB", "0")
    assert upload_io.flush_every_bytes() == 0
    for bad in ("", "lots", "-3"):
        monkeypatch.setenv("MAGIC_UPLOAD_FLUSH_MIB", bad)
        assert upload_io.flush_every_bytes() == upload_io.DEFAULT_FLUSH_MIB * MiB


def test_closing_the_spool_removes_it(tmp_path):
    spool = SpoolFile(tmp_path)
    spool.write(b"abc")
    assert spool.path.exists() and spool.path.parent == tmp_path
    assert spool.path.name.startswith(upload_io.SPOOL_PREFIX)
    spool.close()
    assert list(tmp_path.iterdir()) == []
    spool.close()  # idempotent


def test_a_dropped_spool_is_removed_too(tmp_path):
    # An aborted parse (client gone mid-body) never reaches FileStorage, so
    # nothing closes the spool explicitly; garbage collection must.
    spool = SpoolFile(tmp_path)
    spool.write(b"abc")
    del spool
    assert list(tmp_path.iterdir()) == []


def test_a_failed_write_back_never_fails_the_upload(tmp_path, monkeypatch):
    # Pacing is a courtesy to the kiosk; the upload itself must not depend
    # on it (an old kernel, an odd filesystem).
    def boom(fd, off, n):
        raise OSError(22, "Invalid argument")
    monkeypatch.setattr(upload_io, "_writeback", boom)
    spool = SpoolFile(tmp_path, flush_every=MiB)
    data = b"y" * (2 * MiB)
    spool.write(data)
    spool.seek(0)
    assert spool.read() == data
    spool.close()


# -------------------------------------------------------------- save_upload

def test_save_upload_links_the_spool_instead_of_copying(tmp_path):
    data = os.urandom(MiB)
    spool = SpoolFile(tmp_path)
    spool.write(data)
    spool.seek(0)
    upload = FileStorage(stream=spool, filename="clip.mov")
    dest = tmp_path / "transcode_input_clip.mov"

    save_upload(upload, dest)

    assert os.stat(dest).st_ino == os.fstat(spool.fileno()).st_ino, (
        "the destination must BE the spool (a hard link), not a copy of it")
    upload.close()  # end of request: werkzeug closes the spool
    assert dest.read_bytes() == data
    assert sorted(p.name for p in tmp_path.iterdir()) == [dest.name]


def test_save_upload_copies_an_in_memory_upload(tmp_path):
    upload = FileStorage(stream=io.BytesIO(b"small"), filename="a.mp4")
    dest = tmp_path / "a.mp4"
    save_upload(upload, dest)
    assert dest.read_bytes() == b"small"


def test_save_upload_copies_when_the_link_is_refused(tmp_path, monkeypatch):
    # e.g. EXDEV: the destination is on another filesystem.
    def refuse(src, dst):
        raise OSError(18, "Invalid cross-device link")
    monkeypatch.setattr(upload_io.os, "link", refuse)
    spool = SpoolFile(tmp_path)
    spool.write(b"payload")
    spool.seek(0)
    dest = tmp_path / "out.bin"
    save_upload(FileStorage(stream=spool, filename="x"), dest)
    assert dest.read_bytes() == b"payload"
    spool.close()


# ------------------------------------------------------- the request class

def _app(spool_dir: Path) -> Flask:
    app = Flask(__name__)
    app.request_class = upload_io.PacedUploadRequest
    app.config["UPLOAD_SPOOL_DIR"] = str(spool_dir)
    seen = {}

    @app.post("/up")
    def up():
        f = request.files["file"]
        seen["stream"] = type(f.stream).__name__
        seen["files_during_request"] = sorted(p.name for p in spool_dir.iterdir())
        return "ok"

    app.seen = seen
    return app


def test_large_uploads_spool_to_a_paced_file_in_upload_temp(tmp_path):
    app = _app(tmp_path)
    body = os.urandom(2 * MiB)
    resp = app.test_client().post(
        "/up", data={"file": (io.BytesIO(body), "big.mov")},
        content_type="multipart/form-data")
    assert resp.status_code == 200
    assert app.seen["stream"] == "SpoolFile"
    assert len(app.seen["files_during_request"]) == 1
    assert app.seen["files_during_request"][0].startswith(upload_io.SPOOL_PREFIX)
    assert list(tmp_path.iterdir()) == [], "the spool must go with the request"


def test_small_uploads_stay_in_memory(tmp_path):
    app = _app(tmp_path)
    resp = app.test_client().post(
        "/up", data={"file": (io.BytesIO(b"tiny"), "tiny.yaml")},
        content_type="multipart/form-data")
    assert resp.status_code == 200
    assert app.seen["stream"] != "SpoolFile"
    assert app.seen["files_during_request"] == []


# ------------------------------------------------------------------ idle_io

def test_idle_io_runs_the_block_in_the_idle_class_then_restores(monkeypatch):
    calls = []
    monkeypatch.setattr(upload_io, "_ioprio_get", lambda: 0x4004)
    monkeypatch.setattr(upload_io, "_ioprio_set", lambda v: calls.append(v))
    with upload_io.idle_io():
        calls.append("body")
    assert calls == [upload_io.IOPRIO_IDLE, "body", 0x4004]


def test_idle_io_restores_after_an_exception(monkeypatch):
    calls = []
    monkeypatch.setattr(upload_io, "_ioprio_get", lambda: 0)
    monkeypatch.setattr(upload_io, "_ioprio_set", lambda v: calls.append(v))
    with pytest.raises(RuntimeError):
        with upload_io.idle_io():
            raise RuntimeError("client went away")
    assert calls == [upload_io.IOPRIO_IDLE, 0]


def test_idle_io_is_a_no_op_where_io_priorities_do_not_exist(monkeypatch):
    calls = []
    monkeypatch.setattr(upload_io, "_ioprio_get", lambda: None)
    monkeypatch.setattr(upload_io, "_ioprio_set", lambda v: calls.append(v))
    with upload_io.idle_io():
        pass
    assert calls == []


def test_background_io_decorator_wraps_the_route(monkeypatch):
    calls = []
    monkeypatch.setattr(upload_io, "_ioprio_get", lambda: 0)
    monkeypatch.setattr(upload_io, "_ioprio_set", lambda v: calls.append(v))

    @upload_io.background_io
    def route():
        calls.append("route")
        return "ok"

    assert route() == "ok"
    assert route.__name__ == "route"  # Flask endpoint names come from this
    assert calls == [upload_io.IOPRIO_IDLE, "route", 0]


# ----------------------------------------------------------- WriteBackPacer

def test_pacer_flushes_python_buffer_before_each_write_back(tmp_path,
                                                             monkeypatch):
    seen = []

    def wb(fd, off, n):
        # The slice must already be in the file, not in Python's buffer.
        seen.append((off, n, os.fstat(fd).st_size))
    monkeypatch.setattr(upload_io, "_writeback", wb)
    monkeypatch.setattr(upload_io, "_drop_cache", lambda fd, off, n: None)
    pacer = upload_io.WriteBackPacer(flush_every=MiB)
    data = os.urandom(MiB // 2)
    with open(tmp_path / "out.bin", "wb") as f:   # buffered, like extraction
        for _ in range(5):                        # 2.5 MiB
            f.write(data)
            pacer.wrote(f)
    assert [(o, n) for o, n, _ in seen] == [(0, MiB), (MiB, MiB)]
    assert all(size >= off + n for off, n, size in seen)
    assert (tmp_path / "out.bin").stat().st_size == 5 * len(data)


def test_pacer_never_swallows_a_failed_write(tmp_path, flushes):
    # A full card is the caller's error: the flush ahead of the write-back
    # must raise, not turn pacing off quietly.
    class FullCard(io.BytesIO):
        def fileno(self):
            return 0

        def flush(self):
            raise OSError(28, "No space left on device")

    f = FullCard()
    f.write(b"x" * (2 * MiB))
    with pytest.raises(OSError):
        upload_io.WriteBackPacer(flush_every=MiB).wrote(f)
    assert flushes == []


# ------------------------------------------- package import extraction

def _package_with(video: bytes, rom: bytes) -> io.BytesIO:
    import zipfile
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as zf:
        zf.writestr("playlist.yaml",
                    "title: Pkg\nitems:\n- title: Clip\n"
                    "  path: media/clip.mp4\n  source_type: local\n")
        zf.writestr("media/clip.mp4", video)
        zf.writestr("roms/nes/game.nes", rom)
    buf.seek(0)
    return buf


def test_package_import_extracts_in_paced_slices(client, temp_data_dir,
                                                 monkeypatch):
    import media_probe
    monkeypatch.setattr(media_probe, "probe_source", lambda path: None)
    slices = []    # (inode, offset, length): which file got which slice

    def wb(fd, off, n):
        slices.append((os.fstat(fd).st_ino, off, n))
    monkeypatch.setattr(upload_io, "_writeback", wb)
    monkeypatch.setattr(upload_io, "_drop_cache", lambda fd, off, n: None)
    calls = []
    monkeypatch.setattr(upload_io, "_ioprio_get", lambda: 0)
    monkeypatch.setattr(upload_io, "_ioprio_set", calls.append)
    video, rom = os.urandom(3 * MiB), os.urandom(2 * MiB)

    resp = client.post("/admin/playlists/import-package",
                       data={"file": (_package_with(video, rom), "pkg.zip")},
                       content_type="multipart/form-data")

    assert resp.status_code == 200, resp.get_json()
    out_video = temp_data_dir / "media" / "clip.mp4"
    out_rom = temp_data_dir / "roms" / "nes" / "game.nes"
    assert out_video.read_bytes() == video
    assert out_rom.read_bytes() == rom

    def paced(path):
        ino = path.stat().st_ino   # os.replace keeps the staging inode
        return [(off, n) for i, off, n in slices if i == ino]
    assert paced(out_video) == [(0, MiB), (MiB, MiB), (2 * MiB, MiB)]
    assert paced(out_rom) == [(0, MiB), (MiB, MiB)]
    assert calls == [upload_io.IOPRIO_IDLE, 0]
