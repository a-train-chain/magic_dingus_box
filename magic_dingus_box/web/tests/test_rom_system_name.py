"""A ROM system (folder) name must never become markup in the Content Manager.

The playlist editor's ROM library renders each system name into a
data-system attribute, visible text and an inline onclick handler. The
name is a directory under data/roms, created by the ROM upload URL or by a
playlist-package ZIP entry (roms/<system>/<file>) — so a hostile package
could plant a directory whose name was HTML/JS. Both ends are covered:

  * the server refuses any system name outside a strict allowlist, and
  * the renderer escapes whatever name it is handed (a box may already
    carry such a directory from before the allowlist).

The renderer test runs the REAL renderROMLibraryPanel from
manager_playlists.js under node; skipped where node is absent.
"""
from __future__ import annotations

import io
import json
import re
import shutil
import subprocess
import zipfile
from html.parser import HTMLParser
from pathlib import Path

import pytest
import yaml

from admin_common import _valid_rom_system

STATIC = Path(__file__).resolve().parents[1] / "static"
PLAYLISTS = (Path(__file__).resolve().parents[3]
             / "magic_dingus_box_cpp" / "data" / "playlists")

HOSTILE = [
    'x" onmouseover="alert(1)" y="',
    "x');alert(1);//",
    "x&#39;);alert(1);//",
    "<img src=x onerror=alert(1)>",
    "Atari 7800",
    "",
    ".",
    "..",
]

KNOWN = ["nes", "snes", "genesis", "ps1", "atari7800", "pcengine",
         "arcade", "n64", "dreamcast"]


# ── server: the allowlist ────────────────────────────────────────────────────

@pytest.mark.parametrize("name", KNOWN)
def test_every_known_system_is_allowed(name):
    assert _valid_rom_system(name)


def test_every_system_the_shipped_playlists_use_is_allowed():
    # The shipped playlists' ROM paths are the ground truth for real names.
    systems = set()
    for f in PLAYLISTS.glob("*.yaml"):
        systems.update(re.findall(r"roms/([^/\s\"']+)/", f.read_text()))
    if not systems:
        pytest.skip("no shipped playlists in this checkout")
    assert all(_valid_rom_system(s) for s in systems), systems


@pytest.mark.parametrize("name", HOSTILE)
def test_hostile_system_names_are_refused(name):
    assert not _valid_rom_system(name)


# (A '/' never reaches the route: Flask's router 404s an encoded slash.)
@pytest.mark.parametrize("name", ['x" onmouseover="alert(1)', "x');alert(1);",
                                  "a b", "x<y"])
def test_upload_rom_refuses_bad_system(client, temp_data_dir, name):
    from urllib.parse import quote
    rv = client.post("/admin/upload/rom/" + quote(name, safe=""),
                     data={"file": (io.BytesIO(b"rom"), "g.nes")},
                     content_type="multipart/form-data")
    assert rv.status_code == 400, rv.get_json()
    assert rv.get_json()["error"]["code"] == "VALIDATION_ERROR"
    assert [p.name for p in (temp_data_dir / "roms").iterdir()] == []


def test_upload_rom_accepts_good_system(client, temp_data_dir):
    rv = client.post("/admin/upload/rom/nes",
                     data={"file": (io.BytesIO(b"rom"), "g.nes")},
                     content_type="multipart/form-data")
    assert rv.status_code == 200, rv.get_json()
    assert (temp_data_dir / "roms" / "nes" / "g.nes").exists()


def test_package_import_skips_rom_in_bad_system_dir(client, temp_data_dir):
    evil = 'x" onmouseover="alert(1)'
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr("playlist.yaml", yaml.safe_dump({
            "title": "Games", "items": [
                {"title": "g", "source_type": "emulated_game",
                 "path": "data/roms/nes/good.nes",
                 "emulator_core": "nestopia_libretro"}]}))
        zf.writestr(f"roms/{evil}/bad.nes", b"rom")
        zf.writestr("roms/nes/good.nes", b"rom")
    buf.seek(0)
    rv = client.post("/admin/playlists/import-package",
                     data={"file": (buf, "p.zip")},
                     content_type="multipart/form-data")
    assert rv.status_code == 200, rv.get_json()
    roms = temp_data_dir / "roms"
    assert (roms / "nes" / "good.nes").exists()
    assert sorted(p.name for p in roms.iterdir()) == ["nes"]


# ── client: the real renderer ────────────────────────────────────────────────

def _js_function(path: Path, name: str) -> str:
    src = path.read_text(encoding="utf-8")
    m = re.search(rf"^function {name}\(.*?^\}}", src, re.S | re.M)
    assert m, f"{name} not found in {path.name}"
    return m.group(0)


class _Collect(HTMLParser):
    def __init__(self):
        super().__init__()
        self.items = []   # attrs of each .library-item div
        self.buttons = []  # attrs of each add button
        self.text = []
        self.foreign = []  # any element the template does not write

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "div" and "library-item" in (a.get("class") or ""):
            self.items.append(a)
        if tag == "button":
            self.buttons.append(a)
        if tag not in ("div", "span", "button", "svg", "use"):
            self.foreign.append(tag)

    def handle_data(self, data):
        self.text.append(data)


@pytest.fixture(scope="module")
def rendered():
    node = shutil.which("node")
    if node is None:
        pytest.skip("node not installed")
    manager = STATIC / "manager.js"
    script = "\n".join([
        _js_function(manager, "escapeHtml"),
        _js_function(manager, "escapeJs"),
        _js_function(manager, "spriteIcon"),
        _js_function(manager, "normalizeVideoPath"),
        _js_function(STATIC / "manager_playlists.js", "renderROMLibraryPanel"),
        """
const systems = JSON.parse(require('fs').readFileSync(0, 'utf8'));
const out = {};
const AppState = { playlists: { getItems: () => [] } };
let availableROMs;
for (const s of systems) {
    const el = { innerHTML: '', textContent: '' };
    global.document = { getElementById: id => id === 'romLibraryList' ? el : null };
    availableROMs = { [s]: [{ filename: 'g.bin', path: 'data/roms/x/g.bin' }] };
    renderROMLibraryPanel();
    out[s] = el.innerHTML;
}
process.stdout.write(JSON.stringify(out));
""",
    ])
    names = [n for n in HOSTILE if n] + ["nes"]
    res = subprocess.run([node, "-e", script], input=json.dumps(names),
                         capture_output=True, text=True, check=True)
    return json.loads(res.stdout)


def _parse(html):
    p = _Collect()
    p.feed(html)
    p.close()
    return p


@pytest.mark.parametrize("name", [n for n in HOSTILE if n] + ["nes"])
def test_system_name_stays_data_in_rendered_row(rendered, name):
    p = _parse(rendered[name])
    assert len(p.items) == 1 and len(p.buttons) == 1
    item, button = p.items[0], p.buttons[0]
    # No attribute was injected: exactly the ones the template writes.
    assert set(item) == {"class", "draggable", "data-index", "data-system",
                         "data-type", "data-path", "ondragstart", "ondragend"}
    assert set(button) == {"class", "onclick", "title"}
    assert item["data-system"] == name
    assert p.foreign == [], p.foreign
    assert name.upper() in "".join(p.text)
    # The handler, as the JS engine sees it after HTML decoding, must call
    # addROMToPlaylist with the exact name as a single string argument.
    m = re.fullmatch(r"addROMToPlaylist\('((?:[^'\\]|\\.)*)', 0\)",
                     button["onclick"])
    assert m, button["onclick"]
    out = subprocess.run(
        [shutil.which("node"), "-e",
         f"process.stdout.write(JSON.stringify('{m.group(1)}'))"],
        capture_output=True, text=True, check=True)
    assert json.loads(out.stdout) == name
