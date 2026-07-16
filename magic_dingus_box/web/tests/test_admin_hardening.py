"""Regression tests for the Content Manager hardening pass:
  - playlist put overwrite guard (no silent clobber of a same-named playlist)
  - YAML quoting of previously-raw fields (injection)
  - _is_within() path containment (no sibling-prefix bypass)

Uses the shared conftest fixtures (client has CSRF disabled via
MAGIC_DISABLE_CSRF).
"""
from pathlib import Path

import yaml

from admin import format_playlist_yaml, _is_within


# ── Overwrite guard ──────────────────────────────────────────────────────────

def _playlist_body(title):
    return {"title": title, "curator": "t", "items": []}


def test_put_new_playlist_refuses_overwrite_by_default(client, temp_data_dir):
    # Seed an existing playlist file.
    (temp_data_dir / "playlists" / "family.yaml").write_text("title: Family\nitems: []\n")

    # Creating a NEW playlist that maps to the same filename with
    # overwrite=false must be refused (409), not silently clobber it.
    rv = client.post("/admin/playlists/family.yaml?overwrite=false",
                     json=_playlist_body("Family"))
    assert rv.status_code == 409
    # Original file untouched.
    assert "title: Family" in (temp_data_dir / "playlists" / "family.yaml").read_text()


def test_put_playlist_overwrite_true_replaces(client, temp_data_dir):
    (temp_data_dir / "playlists" / "family.yaml").write_text("title: Old\nitems: []\n")
    rv = client.post("/admin/playlists/family.yaml?overwrite=true",
                     json=_playlist_body("New Name"))
    assert rv.status_code == 200
    assert "New Name" in (temp_data_dir / "playlists" / "family.yaml").read_text()


def test_put_playlist_default_is_overwrite(client, temp_data_dir):
    # No overwrite param → backward-compatible upsert (edit path / API callers).
    (temp_data_dir / "playlists" / "x.yaml").write_text("title: Old\nitems: []\n")
    rv = client.post("/admin/playlists/x.yaml", json=_playlist_body("Fresh"))
    assert rv.status_code == 200
    assert "Fresh" in (temp_data_dir / "playlists" / "x.yaml").read_text()


# ── YAML injection ───────────────────────────────────────────────────────────

def test_emulator_fields_are_quoted_no_injection():
    malicious = "nestopia\nsource_type: emulated_game\npath: /evil/rom.bin"
    out = format_playlist_yaml({
        "title": "P", "curator": "c",
        "items": [{"title": "g", "emulator_core": malicious}],
    })
    # Must parse as valid YAML with the malicious value contained in the
    # single emulator_core field, NOT split into new keys.
    doc = yaml.safe_load(out)
    item = doc["items"][0]
    assert item["emulator_core"] == malicious
    # The injected keys must NOT have leaked to item level.
    assert item.get("path") != "/evil/rom.bin"


def test_source_type_and_playlist_type_quoted():
    out = format_playlist_yaml({
        "title": "P", "curator": "c",
        "playlist_type": "video\nmalicious: 1",
        "items": [{"title": "i", "source_type": "local\nx: 2"}],
    })
    doc = yaml.safe_load(out)
    assert "malicious" not in doc
    assert "x" not in doc["items"][0]


# ── Path containment ─────────────────────────────────────────────────────────

def test_is_within_rejects_sibling_prefix(tmp_path):
    media = tmp_path / "media"
    media.mkdir()
    sibling = tmp_path / "media_backup"
    sibling.mkdir()
    inside = media / "sub" / "f.mp4"
    inside.parent.mkdir(parents=True)
    inside.write_text("x")

    assert _is_within(inside, media) is True
    assert _is_within(media, media) is True          # itself
    # The classic startswith bypass: media_backup is NOT inside media.
    assert _is_within(sibling / "f.mp4", media) is False
    assert _is_within(tmp_path / "other", media) is False
