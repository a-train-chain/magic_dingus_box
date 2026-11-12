"""Opening a playlist in the Content Manager editor and saving it loses nothing.

Runs the REAL cleanPlaylistItems / unknownPlaylistKeys from
static/manager_playlists.js under node (skipped where node is absent) over
what GET /admin/playlists/<name> returns for the all-fields fixture, builds
the body exactly as performSavePlaylist does, POSTs it, and checks the saved
file. Before the change the editor dropped every unknown top-level and
per-item key and every bare-string item.
"""
from __future__ import annotations

import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest
import yaml

from admin_playlists import load_playlist_yaml

from ._kiosk_playlist_model import kiosk_view
from .test_playlist_yaml_fixture import FIXTURE

MANAGER_PLAYLISTS_JS = (Path(__file__).resolve().parents[1] / "static"
                        / "manager_playlists.js")


def _editor_source() -> str:
    src = MANAGER_PLAYLISTS_JS.read_text(encoding="utf-8")
    parts = []
    for pattern in (r"^const PLAYLIST_TOP_LEVEL_KNOWN_KEYS = .*?;$",
                    r"^const PLAYLIST_ITEM_KNOWN_KEYS = .*?;$",
                    r"^function unknownPlaylistKeys\(.*?^\}$",
                    r"^function cleanPlaylistItems\(.*?^\}$"):
        m = re.search(pattern, src, re.S | re.M)
        assert m, f"{pattern} not found in manager_playlists.js"
        parts.append(m.group(0))
    return "\n".join(parts)


def _editor_round_trip(loaded: dict, type_: str) -> dict:
    """What performSavePlaylist POSTs for `loaded` with the form untouched."""
    node = shutil.which("node")
    if node is None:
        pytest.skip("node not installed")
    script = _editor_source() + r"""
const d = JSON.parse(require('fs').readFileSync(0, 'utf8'));
const type = process.argv[1];
const items = cleanPlaylistItems(d.items || []);          // editPlaylist
const extras = unknownPlaylistKeys(d);
const description = (d.description || '').trim();       // the form fields
const body = {
    ...extras,
    title: (d.title || '').trim(),
    curator: (d.curator || '').trim(),
    loop: d.loop || false,
    playlist_type: type,
    items: cleanPlaylistItems(items),                      // performSavePlaylist
};
if (description) body.description = description;
process.stdout.write(JSON.stringify(body));
"""
    out = subprocess.run([node, "-e", script, type_], input=json.dumps(loaded),
                         capture_output=True, text=True, check=True)
    return json.loads(out.stdout)


def test_editor_save_keeps_unknown_keys_and_bare_items(client, temp_data_dir):
    (temp_data_dir / "playlists" / "all.yaml").write_text(FIXTURE.read_text())
    rv = client.get("/admin/playlists/all.yaml")
    assert rv.status_code == 200
    loaded = rv.get_json()["data"]

    body = _editor_round_trip(loaded, "video")
    rv = client.post("/admin/playlists/all.yaml", json=body)
    assert rv.status_code == 200, rv.get_json()
    saved_text = (temp_data_dir / "playlists" / "all.yaml").read_text()
    saved = yaml.safe_load(saved_text)
    src = load_playlist_yaml(FIXTURE.read_text())

    assert saved["unknown_top_level"] == src["unknown_top_level"]
    assert saved["items"][0]["rating"] == 5
    assert saved["items"][0]["note"] == "kept by every writer"
    # The bare path became {path} — the kiosk reads both identically.
    assert saved["items"][3] == {"artist": "", "source_type": "local",
                                 "path": "media/Bare Path Video (USA).mp4"}
    # No title stays no title; awkward strings stay strings.
    assert "title" not in saved["items"][4]
    assert saved["items"][5]["title"] == "Yes"
    assert saved["items"][5]["artist"] == "0x1F"
    assert saved["items"][7]["title"] == "~"
    # And the TV sees exactly what it saw before the save.
    assert kiosk_view(saved_text) == kiosk_view(FIXTURE.read_text())


def test_editor_drops_empty_slots_but_not_unknown_item_keys():
    body = _editor_round_trip({"title": "P", "items": [
        {"isEmpty": True},
        {"title": "", "path": "", "url": None, "tags": [], "start": None,
         "x_custom": {"a": [1, 2]}, "zero": 0, "blank": ""},
        "media/bare.mp4",
    ]}, "video")
    assert body["items"] == [
        {"artist": "", "x_custom": {"a": [1, 2]}, "zero": 0, "blank": ""},
        {"artist": "", "path": "media/bare.mp4"},
    ]
