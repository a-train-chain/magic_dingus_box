"""upload_temp startup-sweep regression tests.

upload_temp collects transcode inputs, probe files, import staging ZIPs
and ffmpeg-stderr temps, all cleaned up in-process by daemon worker
threads whose job state is in-memory only. A service restart, OOM kill,
or power cut mid-job used to orphan them permanently — with an 8GB
default upload cap, one interrupted upload could strand the SD card.
create_app now sweeps the directory at startup, when nothing in it can
be live.
"""
from __future__ import annotations

import os
from pathlib import Path

import pytest

import sys
sys.path.insert(0, str(Path(__file__).parent.parent))

from admin import create_app


def test_startup_sweeps_orphaned_upload_temp_files(tmp_path: Path):
    data_dir = tmp_path / "data"
    (data_dir / "playlists").mkdir(parents=True)
    upload_temp = data_dir / "upload_temp"
    upload_temp.mkdir()

    # Orphans of every shape the leak produced: a bare transcode input, a
    # nested tempfile.mkdtemp() directory, and a stale staging ZIP.
    (upload_temp / "transcode_input_deadbeef_movie.mov").write_bytes(b"x" * 4096)
    nested = upload_temp / "tmpabc123"
    nested.mkdir()
    (nested / "part.bin").write_bytes(b"y" * 2048)
    (upload_temp / "import_stage.zip").write_bytes(b"z" * 1024)

    os.environ["MAGIC_DISABLE_CSRF"] = "1"
    try:
        create_app(data_dir, config={"TESTING": True})
    finally:
        del os.environ["MAGIC_DISABLE_CSRF"]

    assert list(upload_temp.iterdir()) == [], (
        "startup must sweep every orphan out of upload_temp")
    # The directory itself must survive — uploads still need it.
    assert upload_temp.is_dir()


def test_startup_sweep_tolerates_empty_dir(app):
    # The conftest app fixture already ran create_app against an empty
    # upload_temp — reaching this assertion proves the sweep is a no-op
    # there rather than a startup failure.
    assert app is not None
