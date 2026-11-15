"""One playlist fixture, two readers.

magic_dingus_box_cpp/tests/app/fixtures/playlist_all_fields.yaml carries
every known top-level and per-item key, a bare-string item, an item with no
title, unknown top-level and per-item keys and the awkward strings. It is
loaded here AND by magic_dingus_box_cpp/tests/app/test_playlist_yaml_fixture.cpp
(the kiosk's real PlaylistLoader). playlist_all_fields.emitted.yaml is this
emitter's output for it, committed so the C++ test can load the Python form
without running Python; test_emitter_output_matches_committed_fixture keeps
the two from drifting.
"""
from __future__ import annotations

from pathlib import Path

import yaml

from admin_playlists import (
    PLAYLIST_ITEM_KEYS, format_playlist_yaml, load_playlist_yaml)

from ._kiosk_playlist_model import kiosk_view

REPO = Path(__file__).resolve().parents[3]
FIXTURES = REPO / "magic_dingus_box_cpp" / "tests" / "app" / "fixtures"
FIXTURE = FIXTURES / "playlist_all_fields.yaml"
EMITTED = FIXTURES / "playlist_all_fields.emitted.yaml"


def _assert_nothing_lost_or_retyped(src: dict, back: dict):
    assert src.keys() <= back.keys()
    for key, value in src.items():
        if key == "items":
            continue
        assert back[key] == value and type(back[key]) is type(value), key
    assert len(back["items"]) == len(src["items"])
    for s, b in zip(src["items"], back["items"]):
        if not isinstance(s, dict):
            assert b == s and type(b) is type(s)
            continue
        for key, value in s.items():
            assert b[key] == value and type(b[key]) is type(value), (key, s)
        # The only additions are the defaults the emitter always writes.
        for key in b.keys() - s.keys():
            assert (key, b[key]) in {("artist", ""), ("source_type", "local")}


def test_fixture_round_trips_without_loss():
    text = FIXTURE.read_text()
    src = load_playlist_yaml(text)
    # The fixture exercises what it claims to.
    item_keys = {k for i in src["items"] if isinstance(i, dict) for k in i}
    assert set(PLAYLIST_ITEM_KEYS) <= item_keys
    assert item_keys - set(PLAYLIST_ITEM_KEYS)                 # unknown per-item
    assert "unknown_top_level" in src
    assert any(isinstance(i, str) for i in src["items"])       # bare item
    assert any(isinstance(i, dict) and "title" not in i for i in src["items"])

    out = format_playlist_yaml(src)
    _assert_nothing_lost_or_retyped(src, load_playlist_yaml(out))
    _assert_nothing_lost_or_retyped(src, yaml.safe_load(out))
    assert kiosk_view(out) == kiosk_view(text)
    assert format_playlist_yaml(load_playlist_yaml(out)) == out


def test_emitter_output_matches_committed_fixture():
    expected = format_playlist_yaml(load_playlist_yaml(FIXTURE.read_text()))
    assert EMITTED.read_text() == expected, (
        "playlist_all_fields.emitted.yaml is stale. Regenerate it with:\n"
        "  cd magic_dingus_box/web && python3 -c 'import admin_playlists as a; "
        "p=\"../../magic_dingus_box_cpp/tests/app/fixtures/playlist_all_fields\"; "
        "open(p+\".emitted.yaml\",\"w\").write(a.format_playlist_yaml("
        "a.load_playlist_yaml(open(p+\".yaml\").read())))'")
