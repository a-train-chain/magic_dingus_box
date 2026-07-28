"""
Tests for _atomic_write_text — the durability guarantee behind every playlist,
device_info.json, the phone-remote HMAC secret and services/.env.

Why this is worth testing at all: the Magic Dingus Box is a plug-it-in
appliance with no shutdown ritual. It gets powered off by pulling the cord,
routinely, and that can land mid-save. Before this helper, playlists were
written with a bare Path.write_text(), which truncates the target and *then*
streams into it — so a power cut part-way through left a half-written playlist
on the SD card.

The resulting failure is silent, which is what makes it worth guarding:
playlist_loader.cpp parses each playlist in a try/catch and only logs to stderr
on a parse error, so a corrupted playlist raises nothing the user can see. The
playlist just stops appearing on the kiosk.
"""
from __future__ import annotations

import os
import stat
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

from admin import _atomic_write_text  # noqa: E402


def test_writes_content(tmp_path):
    target = tmp_path / "playlist.yaml"
    _atomic_write_text(target, "title: Test\n")
    assert target.read_text() == "title: Test\n"


def test_creates_missing_parent_directories(tmp_path):
    target = tmp_path / "data" / "playlists" / "p.yaml"
    _atomic_write_text(target, "k: v\n")
    assert target.read_text() == "k: v\n"


def test_default_mode_is_world_readable(tmp_path):
    """0644, matching what write_text produced under the default umask.

    mkstemp creates at 0600. Without the explicit chmod, every playlist would
    silently become unreadable to any account other than the one the web admin
    runs as — which would break the kiosk rather than protect it.
    """
    target = tmp_path / "p.yaml"
    _atomic_write_text(target, "x\n")
    assert stat.S_IMODE(target.stat().st_mode) == 0o644


def test_explicit_restrictive_mode_is_honoured(tmp_path):
    """The HMAC secret and services/.env rely on this."""
    target = tmp_path / "flask_secret.key"
    _atomic_write_text(target, "deadbeef", mode=0o600)
    assert stat.S_IMODE(target.stat().st_mode) == 0o600


def test_secret_is_never_visible_at_a_permissive_mode(tmp_path):
    """The file must not appear under its real name until it is already 0600.

    The previous code wrote the secret and chmod-ed afterwards, leaving a window
    where the key that authenticates every paired phone sat at 0644.
    """
    target = tmp_path / "secret.key"
    observed = []

    real_replace = os.replace

    def spy(src, dst):
        # Permissions the file carries at the instant it becomes visible.
        observed.append(stat.S_IMODE(os.stat(src).st_mode))
        return real_replace(src, dst)

    import admin

    original = admin.os.replace
    admin.os.replace = spy
    try:
        _atomic_write_text(target, "s3cret", mode=0o600)
    finally:
        admin.os.replace = original

    assert observed == [0o600]


def test_overwrite_replaces_content(tmp_path):
    target = tmp_path / "p.yaml"
    _atomic_write_text(target, "old: 1\n")
    _atomic_write_text(target, "new: 2\n")
    assert target.read_text() == "new: 2\n"


def test_no_temp_file_left_behind_on_success(tmp_path):
    target = tmp_path / "p.yaml"
    _atomic_write_text(target, "x\n")
    assert [p.name for p in tmp_path.iterdir()] == ["p.yaml"]


def test_original_survives_a_failed_write(tmp_path):
    """The whole point: a crash mid-write must not damage what is already there.

    An unpaired surrogate cannot be encoded to UTF-8, so the write raises after
    the temp file is created but before the rename — exactly the window a power
    cut would land in.
    """
    target = tmp_path / "p.yaml"
    _atomic_write_text(target, "good: yes\n")

    with pytest.raises(UnicodeEncodeError):
        _atomic_write_text(target, "bad \ud800 value")

    assert target.read_text() == "good: yes\n"


def test_no_temp_file_left_behind_after_failure(tmp_path):
    target = tmp_path / "p.yaml"
    _atomic_write_text(target, "good\n")

    with pytest.raises(UnicodeEncodeError):
        _atomic_write_text(target, "bad \ud800 value")

    assert sorted(p.name for p in tmp_path.iterdir()) == ["p.yaml"]


def test_temp_file_is_staged_in_the_target_directory(tmp_path):
    """os.replace is only atomic within a single filesystem.

    Staging in /tmp would make the rename a cross-device copy on any box where
    /tmp is a separate mount (it is tmpfs on the Pi), silently losing atomicity
    — the exact property this helper exists to provide.
    """
    import tempfile as _tempfile

    import admin

    seen = {}
    real_mkstemp = _tempfile.mkstemp

    def spy(*args, **kwargs):
        seen.update(kwargs)
        return real_mkstemp(*args, **kwargs)

    original = admin.tempfile.mkstemp
    admin.tempfile.mkstemp = spy
    try:
        _atomic_write_text(tmp_path / "p.yaml", "x\n")
    finally:
        admin.tempfile.mkstemp = original

    assert seen.get("dir") == str(tmp_path)


def test_survives_a_directory_that_cannot_be_fsynced(tmp_path, monkeypatch):
    """Failing to fsync the directory must not fail a save that already landed.

    Some filesystems refuse to open a directory for fsync. The data is on disk
    and the rename has happened by that point, so the write is still a success.
    """
    import admin

    real_open = os.open

    def refuse_dir_open(path, flags, *args, **kwargs):
        if os.path.isdir(path):
            raise OSError("directory fsync unsupported here")
        return real_open(path, flags, *args, **kwargs)

    monkeypatch.setattr(admin.os, "open", refuse_dir_open)

    target = tmp_path / "p.yaml"
    _atomic_write_text(target, "still: written\n")
    assert target.read_text() == "still: written\n"
