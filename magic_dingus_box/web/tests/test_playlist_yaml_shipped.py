"""Every playlist this repo ships survives a Content Manager save unchanged.

Each tracked playlist YAML is loaded the way the Content Manager loads it,
re-emitted by format_playlist_yaml and compared with the ORIGINAL file as the
kiosk reads it (kiosk_view models yaml-cpp). Before the PyYAML emitter this
failed for The_Nostalgia_Channel.yaml: `artist: Yes` (the band) came back
from safe_load as True and was written as `artist: true`.
"""
from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

from admin_playlists import format_playlist_yaml, load_playlist_yaml

from ._kiosk_playlist_model import kiosk_view
from .test_playlist_yaml_fixture import _assert_nothing_lost_or_retyped

REPO = Path(__file__).resolve().parents[3]


def _tracked_playlists():
    try:
        names = subprocess.run(
            ["git", "-C", str(REPO), "ls-files", "*.yaml", "*.yml"],
            capture_output=True, text=True, check=True).stdout.split()
    except (OSError, subprocess.CalledProcessError):
        names = []
    paths = {REPO / n for n in names if "/playlists/" in f"/{n}"}
    paths |= set((REPO / "magic_dingus_box_cpp" / "data" / "playlists").glob("*.yaml"))
    return sorted(paths)


SHIPPED = _tracked_playlists()


def test_shipped_playlists_were_found():
    assert len(SHIPPED) >= 10


@pytest.mark.parametrize("path", SHIPPED, ids=lambda p: p.name)
def test_shipped_playlist_means_the_same_after_a_save(path):
    text = path.read_text()
    src = load_playlist_yaml(text)
    out = format_playlist_yaml(src)
    assert kiosk_view(out) == kiosk_view(text)
    _assert_nothing_lost_or_retyped(src, load_playlist_yaml(out))
    assert format_playlist_yaml(load_playlist_yaml(out)) == out
