"""Staged-upload regression tests (_staged_save_upload).

upload_media and upload_rom used to f.save() straight onto the final
path: an interrupted transfer left a truncated file that listed as real
content, and re-uploading over an existing file destroyed the original
the moment the transfer STARTED rather than when it succeeded. The
helper stages into a same-directory dot-tmp file and os.replace()s on
success only.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

from admin import _staged_save_upload  # noqa: E402


class _GoodUpload:
    def __init__(self, data: bytes):
        self._data = data

    def save(self, path):
        Path(path).write_bytes(self._data)


class _DyingUpload:
    """Writes half the payload, then dies — a dropped connection."""

    def __init__(self, data: bytes):
        self._data = data

    def save(self, path):
        Path(path).write_bytes(self._data[: len(self._data) // 2])
        raise ConnectionError("client went away mid-transfer")


def test_successful_upload_lands_complete_with_no_staging_leftovers(tmp_path):
    dest = tmp_path / "video.mp4"
    _staged_save_upload(_GoodUpload(b"full-content"), dest)
    assert dest.read_bytes() == b"full-content"
    assert [p.name for p in tmp_path.iterdir()] == ["video.mp4"]


def test_interrupted_upload_leaves_no_file_under_the_final_name(tmp_path):
    dest = tmp_path / "video.mp4"
    with pytest.raises(ConnectionError):
        _staged_save_upload(_DyingUpload(b"full-content"), dest)
    assert not dest.exists(), "a truncated upload must never list as content"
    assert list(tmp_path.iterdir()) == [], "staging file must be cleaned up"


def test_interrupted_reupload_preserves_the_existing_file(tmp_path):
    # The nastiest old behavior: re-uploading over an existing video
    # truncated the ORIGINAL at transfer start. The original must survive
    # a failed replacement untouched.
    dest = tmp_path / "video.mp4"
    dest.write_bytes(b"the-original")
    with pytest.raises(ConnectionError):
        _staged_save_upload(_DyingUpload(b"replacement-content"), dest)
    assert dest.read_bytes() == b"the-original"


def test_creates_missing_parent_directories(tmp_path):
    dest = tmp_path / "nes" / "game.zip"
    _staged_save_upload(_GoodUpload(b"rom"), dest)
    assert dest.read_bytes() == b"rom"
