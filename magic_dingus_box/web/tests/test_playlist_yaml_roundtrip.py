"""A Content Manager save or import must not change what a playlist says.

format_playlist_yaml used to be a hand-rolled emitter over a fixed field
list. Proven before the rewrite (each case below failed then):
  * a title/artist of Yes / true / null / ~ / 0x1F / 12 / 2024-01-01 was
    written unquoted and came back as True / None / 31 / 12 / a date, and the
    NEXT save wrote that back as `true`, `''`, `31` ... for the kiosk to show;
  * an unknown top-level or per-item key was dropped;
  * a bare-string item (a path, which the kiosk accepts) raised
    AttributeError;
  * an item with no title was written as `title: Untitled`, overriding the
    kiosk's filename-derived title.

kiosk_view (_kiosk_playlist_model.py) models what the kiosk's yaml-cpp
loader reads, so "means the same on the TV" is checked, not assumed.
"""
from __future__ import annotations

import datetime

import pytest
import yaml

from admin_playlists import format_playlist_yaml, load_playlist_yaml

from ._kiosk_playlist_model import kiosk_view

AWKWARD = ["Yes", "yes", "No", "on", "off", "true", "False", "null", "Null",
           "~", "", "0x1F", "0o17", "12", "-3", "1.5", "1e3", ".inf", ".nan",
           "2024-01-01", "2024-01-01 10:00:00", "=", "<<", "- dash", "a: b",
           "#hash", "x #y", "it's", '"quoted"', "[x]", "{x}", "&a", "*a",
           "!tag", "|", ">", "%", "@", "`", "?", " lead", "trail ",
           "two\nlines", "tab\there", "cr\r", "\x00nul", "\x7fdel",
           "nel\x85", "ls ", "bom﻿", "café \U0001F3B8",
           "x" * 300, "word " * 40, "...", "---", "--- x", "... x"]


def _emit_reload(data):
    return load_playlist_yaml(format_playlist_yaml(data))


# ── awkward strings ──────────────────────────────────────────────────────────

@pytest.mark.parametrize("value", AWKWARD, ids=repr)
def test_strings_round_trip_as_the_same_string(value):
    data = {"title": value, "curator": value, "description": value,
            "items": [{"title": value, "artist": value, "path": "m/a.mp4",
                       "tags": [value or "t"], "custom": value}]}
    out = format_playlist_yaml(data)
    for doc in (yaml.safe_load(out), load_playlist_yaml(out)):
        assert doc["title"] == value and type(doc["title"]) is str
        assert doc["curator"] == value
        assert doc["description"] == value
        item = doc["items"][0]
        assert item["title"] == value and type(item["title"]) is str
        assert item["artist"] == value and type(item["artist"]) is str
        assert item["tags"] == [value or "t"]
        assert item["custom"] == value
    # And the kiosk (yaml-cpp's as<std::string>) reads the same text.
    view = kiosk_view(out)
    assert view["title"] == value
    assert view["items"][0]["title"] == value
    assert view["items"][0]["artist"] == value
    # Twice is the same as once.
    assert format_playlist_yaml(load_playlist_yaml(out)) == out


@pytest.mark.parametrize("source,text", [
    ("Yes", "Yes"), ("true", "true"), ("0x1F", "0x1F"), ("12", "12"),
    ("1.50", "1.50"), ("2024-01-01", "2024-01-01")])
def test_unquoted_scalars_in_a_file_keep_the_text_the_kiosk_shows(source, text):
    # Hand-written / shipped files carry these unquoted (The Nostalgia
    # Channel: `artist: Yes`). yaml-cpp shows the source text; safe_load
    # would turn it into True / 31 / a date and the save would write that.
    doc = (f"title: {source}\nitems:\n  - title: {source}\n    artist: {source}\n"
           f"    path: m/a.mp4\n    tags:\n      - {source}\n  - {source}\n")
    data = load_playlist_yaml(doc)
    assert data["title"] == text
    assert data["items"][0]["title"] == text
    assert data["items"][0]["artist"] == text
    assert data["items"][0]["tags"] == [text]
    assert data["items"][1] == text  # bare path
    out = format_playlist_yaml(data)
    assert kiosk_view(out)["items"] == kiosk_view(doc)["items"]
    assert kiosk_view(out)["title"] == kiosk_view(doc)["title"]


def test_non_string_values_in_known_text_fields_keep_their_old_text():
    # A JSON caller can send a number/bool/null. The old emitter wrote
    # 12 / true / '' — same kiosk text now, but written as a string so it
    # stays one.
    out = format_playlist_yaml({"title": 12, "curator": None, "items": [
        {"title": True, "artist": None, "path": "m/a.mp4"}]})
    doc = yaml.safe_load(out)
    assert doc["title"] == "12" and doc["curator"] == ""
    assert doc["items"][0]["title"] == "true"
    assert doc["items"][0]["artist"] == ""


# ── unknown keys, bare items, missing titles ─────────────────────────────────

def test_unknown_keys_are_carried_through_with_their_types():
    extra = {"n": 1, "f": 1.5, "b": False, "none": None, "s": "Yes",
             "list": [1, "two", {"three": 3}], "d": datetime.date(2024, 1, 2)}
    data = {"title": "P", "loop": True, "zz_top": extra, "aa": "first",
            "items": [{"title": "t", "path": "m/a.mp4", "rating": 5,
                       "meta": extra}]}
    out = format_playlist_yaml(data)
    doc = yaml.safe_load(out)
    assert doc["zz_top"] == extra and doc["aa"] == "first"
    assert doc["items"][0]["rating"] == 5
    assert doc["items"][0]["meta"] == extra
    # Known keys first, in the historical order; unknown ones after.
    assert list(doc) == ["title", "curator", "description", "playlist_type",
                         "loop", "items", "zz_top", "aa"]
    assert list(doc["items"][0])[-2:] == ["rating", "meta"]


def test_bare_string_item_stays_bare():
    out = format_playlist_yaml({"title": "P", "items": [
        "media/a b (USA).mp4", {"title": "x", "path": "media/x.mp4"}]})
    assert "\n  - media/a b (USA).mp4\n" in out
    doc = yaml.safe_load(out)
    assert doc["items"][0] == "media/a b (USA).mp4"
    view = kiosk_view(out)["items"][0]
    assert view["path"] == "media/a b (USA).mp4"
    assert view["source_type"] == "local"
    assert view["title"] == ("<derived from>", "media/a b (USA).mp4")


def test_bare_items_count_as_video_for_playlist_type():
    out = format_playlist_yaml({"title": "P", "items": ["media/a.mp4"]})
    assert yaml.safe_load(out)["playlist_type"] == "video"


def test_item_without_title_stays_without_one():
    out = format_playlist_yaml({"title": "P", "items": [
        {"path": "data/roms/n64/Super Mario 64 (USA).z64",
         "source_type": "emulated_game", "emulator_core": "mupen64plus_next_libretro"}]})
    item = yaml.safe_load(out)["items"][0]
    assert "title" not in item
    assert "Untitled" not in out


def test_top_level_defaults_are_unchanged():
    doc = yaml.safe_load(format_playlist_yaml({"items": []}))
    assert doc == {"title": "Untitled", "curator": "", "description": "",
                   "playlist_type": "video", "loop": False, "items": []}


def test_known_item_fields_still_dropped_when_empty():
    # Same as the old emitter: blank path/url/core/system and empty or
    # all-blank tags are not written; artist and source_type always are.
    doc = yaml.safe_load(format_playlist_yaml({"title": "P", "items": [
        {"title": "t", "path": "", "url": None, "tags": ["", " ", 3],
         "emulator_core": "", "emulator_system": None}]}))
    assert doc["items"][0] == {"title": "t", "artist": "", "source_type": "local"}


def test_items_must_be_a_list():
    with pytest.raises(ValueError):
        format_playlist_yaml({"title": "P", "items": {"a": 1}})


def test_output_uses_no_constructs_yaml_cpp_mishandles():
    items = ["media/bare.mp4"] + [
        {"title": v, "artist": v, "path": "m/a.mp4", "tags": [v or "t"]}
        for v in AWKWARD]
    out = format_playlist_yaml(
        {"title": "P", "extra": {"l": [1, "Yes"]}, "items": items})
    events = list(yaml.parse(out))
    for ev in events:
        assert getattr(ev, "anchor", None) is None
        assert getattr(ev, "tag", None) is None
        if isinstance(ev, yaml.ScalarEvent):
            assert ev.style in (None, "'", '"')  # never | or >
    starts = [e for e in events if isinstance(e, yaml.DocumentStartEvent)]
    ends = [e for e in events if isinstance(e, yaml.DocumentEndEvent)]
    assert not starts[0].explicit and not ends[0].explicit
    # No folded continuation lines: every line is a key, a list entry or blank.
    for line in out.splitlines():
        stripped = line.strip()
        assert (not stripped or stripped.startswith("- ")
                or ":" in stripped.split(" ", 1)[0] + " "), line
