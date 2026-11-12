"""Playlist CRUD routes and the playlist-format helpers.

GET/POST/DELETE /admin/playlists[/<name>], the YAML formatter the kiosk
expects (format_playlist_yaml), playlist naming/summary helpers and the
emulator_core validation every playlist write path applies.
"""
from __future__ import annotations

import math
import re
import sys
import yaml
from pathlib import Path
from typing import Any, Optional

from flask import request

# Same dual import form as admin.py: flat when tests put web/ on
# sys.path, package-relative in production.
try:
    from admin_common import (
        _sanitize_filename, error_response, success_response,
    )
    from admin_security import _is_within
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from .admin_common import (
        _sanitize_filename, error_response, success_response,
    )
    from .admin_security import _is_within


def _derive_playlist_type(data: dict) -> str:
    """Classify a playlist as 'game' or 'video' from its ITEMS.

    The kiosk never reads the top-level `playlist_type` key — magic_dingus_box_cpp
    /src/app/app_state.h derives everything from item source_types via
    is_game_playlist() / is_video_playlist(), and main.cpp routes on those. The
    web admin, by contrast, trusted `data.get('playlist_type', 'video')`, which
    is only as good as the key being present.

    It usually was not. Seven of the eight game playlists on the box carry no
    playlist_type at all, so they all defaulted to "video" and appeared in the
    Videos tab of the Content Manager. games_n64.yaml was the lone exception —
    it was written later, with the key — which is why N64 was the only console
    that showed up in the right place.

    Mirrors is_game_playlist() exactly: a playlist is a game playlist when it has
    items and EVERY item is emulated_game. A mixed playlist is a video playlist,
    matching the kiosk, which sends it to the main video screen.

    An empty playlist matches neither predicate on the kiosk (it appears
    nowhere), so there is nothing to mirror; fall back to whatever the file
    declares, and to 'video' if it declares nothing.
    """
    items = data.get('items') or []
    if not isinstance(items, list) or not items:
        declared = data.get('playlist_type')
        return declared if declared in ('video', 'game') else 'video'

    for item in items:
        # A bare string item is a path, which playlist_loader.cpp forces to
        # source_type "local" — i.e. a video.
        if not isinstance(item, dict):
            return 'video'
        if item.get('source_type') != 'emulated_game':
            return 'video'
    return 'game'


def _canonical_playlist_name(safe_name: str) -> str:
    """Force a playlist filename to .yaml.

    playlist_loader.cpp scans data/playlists/ with
    `entry.path().extension() == ".yaml"` — a hard equality, so a .yml file is
    invisible to the kiosk. The web admin accepted both, which meant a .yml
    playlist saved with 200 OK, appeared in the Content Manager list, and then
    simply never showed up on the TV, with nothing anywhere explaining why.

    Normalising on WRITE keeps one canonical spelling on disk. The read and
    delete paths deliberately do NOT call this: they must still be able to find
    and remove a .yml file that predates this change.
    """
    if safe_name.lower().endswith(".yml"):
        return safe_name[: -len(".yml")] + ".yaml"
    return safe_name


# Filename used when a title cannot produce one. Shared by every title->file
# derivation so two callers cannot disagree about what "no usable title" means.
UNTITLED_PLAYLIST_FILENAME = "imported_playlist.yaml"


def _playlist_filename_for_title(
        title: str, fallback: str = UNTITLED_PLAYLIST_FILENAME) -> str:
    """Derive the on-box .yaml filename for a playlist title.

    The character class here is deliberately narrow — `[^\\w\\s-]` keeps word
    characters, whitespace and dashes and drops everything else — which means a
    title made ENTIRELY of non-word characters reduces to the empty string. A
    title that is nothing but emoji therefore used to produce the literal file
    '.yaml', written with a 200 OK, and that file is a ghost twice over:

      * the kiosk never loads it. playlist_loader.cpp keeps a file only when
        `entry.path().extension() == ".yaml"`, and the extension of a name that
        is nothing but a suffix is empty — std::filesystem treats a leading dot
        as the start of a hidden file's stem, not an extension.
      * it cannot be removed through the web admin either. The Content Manager
        lists playlists with Path.glob("*.y*ml"), which does NOT hide dotfiles,
        so the row appears; but every GET/POST/DELETE for it runs the name
        through _sanitize_filename, where os.path.splitext('.yaml')[1] == ''
        fails the allowed-extensions check and raises. The row is visible,
        broken, and permanently un-actionable.

    Hence the ordering below, which matters: fall back to a real name FIRST,
    then sanitize. Sanitizing first would turn today's silent success into a
    confusing 400 on a title the user is perfectly entitled to use.

    Note this only bites titles with zero word characters. A leading emoji is
    fine and always was: "<emoji> MDB" -> "MDB.yaml".

    `fallback` is what an unusable title degrades to. Callers that have a better
    answer than a generic name — the single-YAML import has the uploaded
    filename — pass it here rather than reimplementing the slug.

    Raises ValueError (from _sanitize_filename) only if `fallback` itself is not
    a legal .yaml/.yml basename; a slug derived from a title never can be, since
    the character class excludes every path separator.
    """
    slug = re.sub(r'[^\w\s-]', '', title or '').strip()
    slug = re.sub(r'[-\s]+', '_', slug)
    return _canonical_playlist_name(
        _sanitize_filename(f"{slug}.yaml" if slug else fallback,
                           allowed_extensions=['.yaml', '.yml']))


def _playlist_summary(path: Path) -> dict:
    """Best-effort {title, item_count} for a playlist already on disk.

    Used to describe the INCUMBENT file in a filename-collision response, so
    the operator is asked about two named playlists rather than about a
    filename they never typed. A file that will not parse still has to be
    describable — a collision is exactly when the incumbent might be junk — so
    every failure degrades to the stem rather than raising.
    """
    try:
        data = load_playlist_yaml(path.read_text())
    except Exception:
        return {"title": path.stem, "item_count": 0}
    if not isinstance(data, dict):
        return {"title": path.stem, "item_count": 0}
    items = data.get("items")
    return {
        "title": str(data.get("title") or path.stem),
        "item_count": len(items) if isinstance(items, list) else 0,
    }


# ===== emulator_core VALIDATION =====
#
# The kiosk joins emulator_core straight into a shared-object path
# (retroarch_launcher.cpp: libretro_dir + "/" + core_name + ".so") and hands
# it to RetroArch as -L. A value like "../../../tmp/evil" would load an
# arbitrary .so the moment someone picks that game. Every legitimate value is
# a plain libretro core name (mupen64plus_next_libretro, pcsx_rearmed_libretro,
# ...) or the legacy "auto" (resolved per-system by controller.cpp) — all
# word characters — so anything else is refused at every web write path.
_EMULATOR_CORE_RE = re.compile(r"^[A-Za-z0-9_]+$")


def _invalid_emulator_core(playlist: Any) -> Optional[str]:
    """Return the first offending emulator_core in `playlist`, else None.

    Empty / missing values are fine (non-game items have none)."""
    if not isinstance(playlist, dict):
        return None
    items = playlist.get("items")
    if not isinstance(items, list):
        return None
    for item in items:
        if not isinstance(item, dict):
            continue
        core = item.get("emulator_core")
        if core is None or core == "":
            continue
        if not isinstance(core, str) or not _EMULATOR_CORE_RE.fullmatch(core):
            return repr(core)[:80]
    return None


def _emulator_core_error(playlist: Any):
    bad = _invalid_emulator_core(playlist)
    if bad is None:
        return None
    return error_response(
        "VALIDATION_ERROR",
        f"Invalid emulator_core {bad}: must be a libretro core name "
        "(letters, digits, underscores) or 'auto'.")


def _time_bound(value: Any):
    """Coerce an item's start/end to a finite, non-negative number.

    format_playlist_yaml writes start/end unquoted, so a string carrying a
    newline used to inject arbitrary keys (an emulator_core that never met
    the guard above). The kiosk reads both as<double>. None/"" mean
    "absent". Raises ValueError for anything else that is not a number."""
    if value is None or value == "":
        return None
    if isinstance(value, bool):
        raise ValueError("not a number")
    if isinstance(value, str):
        value = value.strip()
    number = float(value)  # ValueError / TypeError for non-numbers
    if not math.isfinite(number) or number < 0:
        raise ValueError("must be finite and non-negative")
    return int(number) if number.is_integer() else number


def _invalid_time_bounds(playlist: Any) -> Optional[str]:
    """Return a description of the first bad start/end, else None."""
    if not isinstance(playlist, dict) or not isinstance(playlist.get("items"), list):
        return None
    for item in playlist["items"]:
        if not isinstance(item, dict):
            continue
        for key in ("start", "end"):
            try:
                _time_bound(item.get(key))
            except (TypeError, ValueError):
                return f"{key} {repr(item.get(key))[:80]}"
    return None


def _time_bounds_error(playlist: Any):
    bad = _invalid_time_bounds(playlist)
    if bad is None:
        return None
    return error_response(
        "VALIDATION_ERROR",
        f"Invalid {bad}: must be a non-negative number of seconds.")


# ===== PLAYLIST YAML EMITTER =====
#
# The kiosk's playlist_loader.cpp is the schema of record; these are the keys
# it (or the Content Manager) gives meaning to, in the order the emitter has
# always written them. Every OTHER key — top-level or per-item — is carried
# through untouched, after the known ones: a save or an import must never be
# the thing that deletes a field a newer kiosk or a hand-edit relies on.
PLAYLIST_TOP_LEVEL_KEYS = (
    "title", "curator", "description", "playlist_type", "loop", "items")
PLAYLIST_ITEM_KEYS = (
    "title", "artist", "source_type", "path", "url", "start", "end", "tags",
    "emulator_core", "emulator_system")

# Line-break characters YAML recognises inside a scalar. A string carrying
# any of them (or any other control character) is written double-quoted
# with backslash escapes on ONE line, never as a multi-line single-quoted or
# block scalar: that is the form both yaml-cpp and PyYAML read back
# byte-for-byte, and it cannot be dedented into a new key.
_YAML_LINE_BREAKS = "\n\r\x85  "


class _PlaylistDumper(yaml.SafeDumper):
    """safe_dump with the house layout: sequences indented under their key
    ("items:\\n  - title: ..."), no anchors/aliases (yaml-cpp 0.7/0.8 and a
    human editor both do better without them)."""

    def increase_indent(self, flow=False, indentless=False):
        return super().increase_indent(flow, False)

    def ignore_aliases(self, data):
        return True


def _represent_str(dumper, value):
    if any(c in _YAML_LINE_BREAKS or ord(c) < 0x20 or ord(c) == 0x7F
           for c in value):
        return dumper.represent_scalar("tag:yaml.org,2002:str", value, style='"')
    # Default style: PyYAML quotes anything its resolver would read back as
    # a non-string (Yes, true, null, ~, 0x1F, 12, 2024-01-01, ...).
    return dumper.represent_str(value)


_PlaylistDumper.add_representer(str, _represent_str)


def _dump(node) -> str:
    return yaml.dump(
        node, Dumper=_PlaylistDumper, sort_keys=False, allow_unicode=True,
        default_flow_style=False, explicit_start=False, explicit_end=False,
        # Never fold: a folded plain/quoted scalar is legal YAML but a long
        # path or title should stay one greppable line.
        width=float("inf"))


def _as_text(value) -> str:
    """A known string field's value as the string the kiosk will read.

    Matches the text the hand-rolled emitter used to write for non-strings
    (None -> '', True -> 'true', 12 -> '12'), so the kiosk's meaning is
    unchanged — but it is now written as a STRING, so the next load does
    not turn it into a bool/None/number again."""
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    return value if isinstance(value, str) else str(value)


# Fields the kiosk reads with yaml-cpp's as<std::string>(), which returns a
# scalar's SOURCE TEXT however it is spelled: `artist: Yes` is the band Yes
# on the TV. PyYAML's safe_load turns the same scalar into True, so a plain
# safe_load -> format round trip rewrote it as the string "true" (shipped:
# The_Nostalgia_Channel.yaml). load_playlist_yaml restores the source text.
_PLAYLIST_TEXT_KEYS = ("title", "curator", "description", "playlist_type")
_ITEM_TEXT_KEYS = ("title", "artist", "source_type", "path", "url",
                   "emulator_core", "emulator_system")


def _restore_text(typed, raw):
    """`typed` with a non-string scalar replaced by its source text `raw`.
    None stays None (a null was never text) and so do collections."""
    if (typed is None or isinstance(typed, (str, dict, list))
            or not isinstance(raw, str)):
        return typed
    return raw


def load_playlist_yaml(text: str):
    """yaml.safe_load, except the fields the kiosk reads as text keep their
    text: `title: Yes`, `artist: 0x1F`, `- 2001` (a bare path) load as the
    strings 'Yes', '0x1F', '2001' — what the kiosk shows — not True/31/2001.

    Everything else (loop, start/end, unknown keys) keeps safe_load's types.
    Raises yaml.YAMLError exactly as safe_load does."""
    typed = yaml.safe_load(text)
    if not isinstance(typed, dict):
        return typed
    try:
        raw = yaml.load(text, Loader=yaml.BaseLoader)  # every scalar a str
    except yaml.YAMLError:  # pragma: no cover - safe_load accepted it
        return typed
    if not isinstance(raw, dict):  # pragma: no cover
        return typed

    def fix(typed_map, raw_map, keys):
        for key in keys:
            if key in typed_map and key in raw_map:
                typed_map[key] = _restore_text(typed_map[key], raw_map[key])

    fix(typed, raw, _PLAYLIST_TEXT_KEYS)
    items, raw_items = typed.get("items"), raw.get("items")
    if not (isinstance(items, list) and isinstance(raw_items, list)
            and len(items) == len(raw_items)):
        return typed
    for i, (item, raw_item) in enumerate(zip(items, raw_items)):
        if isinstance(item, dict) and isinstance(raw_item, dict):
            fix(item, raw_item, _ITEM_TEXT_KEYS)
            tags, raw_tags = item.get("tags"), raw_item.get("tags")
            if (isinstance(tags, list) and isinstance(raw_tags, list)
                    and len(tags) == len(raw_tags)):
                item["tags"] = [_restore_text(t, r)
                                for t, r in zip(tags, raw_tags)]
        else:
            # A bare scalar item is a PATH to the kiosk.
            items[i] = _restore_text(item, raw_item)
    return typed


def _format_item(item):
    """One playlist item as the dict the emitter writes.

    A bare string item (a path; playlist_loader.cpp treats it as a local
    video titled from its filename) is kept bare. Any other non-mapping is
    passed through unchanged rather than guessed at."""
    if not isinstance(item, dict):
        return item

    out = {}
    # No title stays no title: the kiosk then derives one from the filename
    # (title_from_rom_path). Writing "Untitled" overrode that.
    if "title" in item:
        out["title"] = _as_text(item["title"])
    out["artist"] = _as_text(item.get("artist", ""))
    out["source_type"] = _as_text(item.get("source_type", "local"))
    for key in ("path", "url"):
        if item.get(key):
            out[key] = _as_text(item[key])
    # _time_bound yields a plain number or raises ValueError (callers turn
    # that into a 400) — a string never reaches the file.
    for key in ("start", "end"):
        bound = _time_bound(item.get(key))
        if bound is not None:
            out[key] = bound
    tags = item.get("tags")
    if isinstance(tags, (list, tuple)):
        valid_tags = [t for t in tags if isinstance(t, str) and t.strip()]
        if valid_tags:
            out["tags"] = valid_tags
    elif tags:
        out["tags"] = tags  # not a list: not ours to reinterpret
    for key in ("emulator_core", "emulator_system"):
        if item.get(key):
            out[key] = _as_text(item[key])

    for key, value in item.items():
        if key not in PLAYLIST_ITEM_KEYS:
            out[key] = value
    return out


def format_playlist_yaml(data: dict) -> str:
    """Serialize a playlist dict to the YAML the kiosk loads.

    Emitted by PyYAML (so every string round-trips as a string), known keys
    first in the historical order, every unknown top-level and per-item key
    carried through after them. Items are separated by a blank line, as the
    hand-written files are. The output uses only block mappings/sequences,
    plain/single/double-quoted scalars and `[]`/`{}` for empty collections —
    no anchors, tags, document markers or folded/literal blocks.
    """
    items = data.get("items")
    if items is None:
        items = []
    if not isinstance(items, list):
        raise ValueError("Playlist 'items' must be a list")

    head = {
        "title": _as_text(data.get("title", "Untitled")),
        # Default to blank, not the literal "Unknown": a playlist with no
        # curator renders with no byline.
        "curator": _as_text(data.get("curator", "")),
        "description": _as_text(data.get("description", "")),
        # Derive rather than default to 'video' — the kiosk ignores this key,
        # but the Content Manager sorts playlists into tabs by it.
        "playlist_type": _as_text(
            data.get("playlist_type") or _derive_playlist_type(data)),
        "loop": bool(data.get("loop", False)),
    }
    tail = {k: v for k, v in data.items() if k not in PLAYLIST_TOP_LEVEL_KEYS}

    parts = [_dump(head)]
    if not items:
        parts.append("items: []\n")
    else:
        # Each item is dumped as a complete one-entry `items:` document and
        # its entry lines kept, so the blank separator line can only ever
        # fall BETWEEN two entries, never inside a scalar.
        chunks = [_dump({"items": [_format_item(i)]}).split("\n", 1)[1]
                  for i in items]
        parts.append("items:\n" + "\n".join(chunks))
    if tail:
        parts.append(_dump(tail))
    return "".join(parts)


def register(app, ctx) -> None:
    """Register this module's routes on `app`. Called exactly once, from
    admin.create_app(); `ctx` carries the create_app() state these routes
    share with the rest of the Content Manager (see admin.py)."""
    # create_app() state shared across route modules
    _poke_kiosk_reload = ctx._poke_kiosk_reload
    data_dir = ctx.data_dir
    media_dir = ctx.media_dir
    playlists_dir = ctx.playlists_dir
    require_csrf = ctx.require_csrf
    # Defined/re-exported in admin.py; tests monkeypatch admin.<name>, so
    # these are shims that look the name up in admin at call time.
    _atomic_write_text = ctx._atomic_write_text

    # ===== PLAYLIST MANAGEMENT =====

    @app.get("/admin/playlists")
    def list_playlists():  # type: ignore[no-redef]
        """List all playlists with metadata."""
        playlists = []
        if not playlists_dir.exists():
            return success_response(data=playlists)

        for p in sorted(playlists_dir.glob("*.y*ml")):
            try:
                data = load_playlist_yaml(p.read_text())
                playlists.append({
                    'filename': p.name,
                    'title': data.get('title', p.stem),
                    'curator': data.get('curator', ''),
                    'description': data.get('description', ''),
                    'item_count': len(data.get('items', [])),
                    'loop': data.get('loop', False),
                    'playlist_type': _derive_playlist_type(data),
                })
            except Exception:
                playlists.append({
                    'filename': p.name,
                    'title': p.stem,
                    'parse_error': True
                })

        return success_response(data=playlists)

    @app.get("/admin/playlists/<name>")
    def get_playlist(name):  # type: ignore[no-redef]
        """Get full playlist content for editing."""
        try:
            safe_name = _sanitize_filename(name, allowed_extensions=['.yaml', '.yml'])
        except ValueError as e:
            return error_response("VALIDATION_ERROR", str(e))

        p = playlists_dir / safe_name
        # Containment check matching the DELETE handler — defends against any
        # path-traversal that survived _sanitize_filename (symlinks, etc.).
        p_resolved = p.resolve()
        playlists_dir_resolved = playlists_dir.resolve()
        if not _is_within(p_resolved, playlists_dir_resolved):
            return error_response("VALIDATION_ERROR", "Invalid path")

        if not p.exists():
            return error_response("NOT_FOUND", f"Playlist '{name}' not found", status=404)

        try:
            data = load_playlist_yaml(p.read_text())
            return success_response(data=data)
        except Exception as e:
            return error_response("PARSE_ERROR", f"Failed to parse playlist: {e}", status=500)

    @app.post("/admin/playlists/<name>")
    @require_csrf
    def put_playlist(name):  # type: ignore[no-redef]
        """Create or update a playlist."""
        try:
            print(f"Saving playlist: {name}", file=sys.stderr)
            # Sanitize filename to prevent path traversal
            safe_name = _canonical_playlist_name(
                _sanitize_filename(name, allowed_extensions=['.yaml', '.yml']))

            # Accept JSON or YAML
            if request.is_json:
                data = request.get_json()
                if not data:
                    return error_response("VALIDATION_ERROR", "Invalid JSON body")
                bad_core = _emulator_core_error(data) or _time_bounds_error(data)
                if bad_core:
                    return bad_core
                # Convert to clean YAML matching the expected format
                yaml_content = format_playlist_yaml(data)
            else:
                yaml_content = request.get_data(as_text=True)
                # Validate it's valid YAML
                if not yaml_content.strip():
                    return error_response("VALIDATION_ERROR", "Empty content")
                try:
                    raw_doc = yaml.safe_load(yaml_content)
                except yaml.YAMLError as e:
                    return error_response("VALIDATION_ERROR", f"Invalid YAML: {e}")
                bad_core = _emulator_core_error(raw_doc)
                if bad_core:
                    return bad_core
                yaml.safe_load(yaml_content)

            if not yaml_content.strip():
                return error_response("VALIDATION_ERROR", "Generated YAML is empty")

            p = playlists_dir / safe_name
            # Safer check for path traversal:
            if '..' in safe_name or '/' in safe_name or '\\' in safe_name:
                return error_response("VALIDATION_ERROR", "Invalid filename")

            # Overwrite guard. A NEW playlist whose title maps to an
            # already-existing file would silently clobber that other
            # playlist (filename is derived from the title client-side).
            # The client sends overwrite=false when creating-new; if the
            # file already exists we refuse with 409 so the UI can warn and
            # let the user confirm or rename. overwrite defaults to true to
            # preserve the edit-existing path and other API callers.
            overwrite = request.args.get("overwrite", "true").lower() != "false"
            if not overwrite and p.exists():
                return error_response(
                    "CONFLICT",
                    f"A playlist named '{safe_name}' already exists.",
                    status=409)

            _atomic_write_text(p, yaml_content)
            _poke_kiosk_reload("playlists")
            print(f"Successfully saved playlist: {safe_name} ({len(yaml_content)} bytes)", file=sys.stderr)
            return success_response(data={"filename": safe_name}, message="Playlist saved")
        except ValueError as e:
            print(f"ValueError saving playlist {name}: {e}", file=sys.stderr)
            return error_response("VALIDATION_ERROR", str(e))
        except Exception as e:
            print(f"Error saving playlist {name}: {e}", file=sys.stderr)
            import traceback
            traceback.print_exc()
            return error_response("INTERNAL_ERROR", str(e), status=500)

    def _normalize_video_path(path: str) -> str:
        """Normalize video path for comparison - strips prefixes to get just filename."""
        if not path:
            return ''
        clean = path
        # Remove leading slash
        if clean.startswith('/'):
            clean = clean[1:]
        # Remove ../ prefixes
        while clean.startswith('../'):
            clean = clean[3:]
        # Remove magic_dingus_box_cpp/ prefix
        if clean.startswith('magic_dingus_box_cpp/'):
            clean = clean[len('magic_dingus_box_cpp/'):]
        # Remove data/ or dev_data/ prefix
        if clean.startswith('data/'):
            clean = clean[len('data/'):]
        elif clean.startswith('dev_data/'):
            clean = clean[len('dev_data/'):]
        # Remove media/ prefix
        if clean.startswith('media/'):
            clean = clean[len('media/'):]
        return clean

    @app.delete("/admin/playlists/<name>")
    @require_csrf
    def delete_playlist(name):  # type: ignore[no-redef]
        """Delete a playlist, optionally with associated videos.

        Query params:
            delete_videos: If 'true', also delete videos only used in this playlist
        """
        try:
            safe_name = _sanitize_filename(name, allowed_extensions=['.yaml', '.yml'])
        except ValueError as e:
            return error_response("VALIDATION_ERROR", str(e))

        p = playlists_dir / safe_name
        # Ensure path stays within playlists_dir
        p_resolved = p.resolve()
        playlists_dir_resolved = playlists_dir.resolve()
        if not _is_within(p_resolved, playlists_dir_resolved):
            return error_response("VALIDATION_ERROR", "Invalid path")

        if not p.exists():
            return error_response("NOT_FOUND", f"Playlist '{name}' not found", status=404)

        videos_deleted = 0
        delete_videos = request.args.get('delete_videos', 'false').lower() == 'true'

        if delete_videos:
            try:
                # Parse the playlist to get video paths
                playlist_data = yaml.safe_load(p.read_text())
                playlist_videos = set()
                for item in playlist_data.get('items', []):
                    if item.get('source_type') == 'local' and item.get('path'):
                        playlist_videos.add(_normalize_video_path(item['path']))

                # Build set of videos used in OTHER playlists
                videos_used_elsewhere = set()
                for other_playlist in playlists_dir.glob("*.y*ml"):
                    if other_playlist.name == safe_name:
                        continue  # Skip the playlist being deleted
                    try:
                        other_data = yaml.safe_load(other_playlist.read_text())
                        for item in other_data.get('items', []):
                            if item.get('source_type') == 'local' and item.get('path'):
                                videos_used_elsewhere.add(_normalize_video_path(item['path']))
                    except Exception:
                        continue  # Skip problematic playlists

                # Determine orphaned videos (only in this playlist)
                orphaned_videos = playlist_videos - videos_used_elsewhere

                # Delete orphaned videos
                for normalized_filename in orphaned_videos:
                    # Try to find and delete the video file
                    # Check both media_dir and dev_media_dir
                    video_path = media_dir / normalized_filename
                    dev_media_dir = data_dir.parent / "dev_data" / "media"
                    dev_video_path = dev_media_dir / normalized_filename

                    # Security check: each candidate must resolve inside ITS
                    # OWN media directory. This used to accept anything under
                    # data_dir.parent, and _normalize_video_path only strips
                    # LEADING ../ — so a playlist item path like
                    # "media/x/../../flask_secret.key" (or ../config/settings.json)
                    # resolved outside media/ but still inside the install
                    # tree, and delete_videos=true unlinked it. Videos only
                    # ever live in data/media or dev_data/media (list_media
                    # and delete_media scan exactly those two).
                    for candidate, base in ((video_path, media_dir),
                                            (dev_video_path, dev_media_dir)):
                        if candidate.exists() and candidate.is_file():
                            candidate_resolved = candidate.resolve()
                            if (_is_within(candidate_resolved, base.resolve())
                                    and candidate_resolved != base.resolve()):
                                try:
                                    candidate.unlink()
                                    videos_deleted += 1
                                    print(f"Deleted video: {candidate}", file=sys.stderr)
                                    break  # Only delete from one location
                                except Exception as e:
                                    print(f"Failed to delete video {candidate}: {e}", file=sys.stderr)

            except Exception as e:
                print(f"Error processing videos for deletion: {e}", file=sys.stderr)
                # Continue with playlist deletion even if video deletion fails

        # Delete the playlist file
        p.unlink()
        _poke_kiosk_reload("playlists")

        if videos_deleted > 0:
            return success_response(
                data={"videos_deleted": videos_deleted},
                message=f"Playlist deleted along with {videos_deleted} video(s)"
            )
        return success_response(message="Playlist deleted")
