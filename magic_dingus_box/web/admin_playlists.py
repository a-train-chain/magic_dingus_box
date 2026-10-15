"""Playlist CRUD routes and the playlist-format helpers.

GET/POST/DELETE /admin/playlists[/<name>], the YAML formatter the kiosk
expects (format_playlist_yaml), playlist naming/summary helpers and the
emulator_core validation every playlist write path applies.
"""
from __future__ import annotations

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
        data = yaml.safe_load(path.read_text())
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


def format_playlist_yaml(data: dict) -> str:
    """Format playlist data as clean YAML matching the expected format.
    
    This ensures the YAML output matches the format that PlaylistLibrary expects,
    with consistent structure and blank fields where no data exists.
    """
    
    def yaml_quote(value) -> str:
        """Quote a YAML value if it contains special characters that would break parsing."""
        # Handle non-string values
        if value is None:
            return "''"
        if isinstance(value, bool):
            return 'true' if value else 'false'
        if isinstance(value, (int, float)):
            return str(value)
        # Convert to string if not already
        value = str(value)
        if not value:
            return "''"
        # Control characters (newline, tab, CR, etc.) CANNOT be represented in
        # a single-quoted YAML scalar — embedding a literal newline there
        # produces broken or injectable YAML (a dedented continuation line
        # parses as a brand-new key). Emit a double-quoted scalar with proper
        # backslash escapes instead, which round-trips control chars safely.
        if any(ord(c) < 0x20 for c in value):
            escaped = (value.replace('\\', '\\\\')
                            .replace('"', '\\"')
                            .replace('\n', '\\n')
                            .replace('\t', '\\t')
                            .replace('\r', '\\r'))
            return f'"{escaped}"'
        # Characters that need quoting: # (comment), : (key separator), leading/trailing spaces
        needs_quoting = any(c in value for c in ['#', ':', '[', ']', '{', '}', '&', '*', '!', '|', '>', "'", '"', '%', '@', '`'])
        needs_quoting = needs_quoting or value.startswith(' ') or value.endswith(' ')
        needs_quoting = needs_quoting or value.startswith('-') or value.startswith('?')
        if needs_quoting:
            # Use single quotes and escape any single quotes in the value
            escaped = value.replace("'", "''")
            return f"'{escaped}'"
        return value
    
    lines = []
    
    # Top-level fields in expected order (always include for consistency)
    lines.append(f"title: {yaml_quote(data.get('title', 'Untitled'))}")
    # Default to blank, not the literal "Unknown". A playlist with no curator
    # should render with no byline (the cards omit it entirely when empty),
    # rather than being permanently attributed to a person called Unknown.
    lines.append(f"curator: {yaml_quote(data.get('curator', ''))}")
    
    # Always include description field (blank if empty, for consistency)
    description = data.get('description', '')
    lines.append(f"description: {yaml_quote(description)}")
    
    # Playlist type (video or game). Quote like every other user-supplied
    # field — an unquoted value could inject newlines / extra YAML keys.
    # Derive rather than default to 'video'. Writing 'video' onto a playlist
    # whose items are all games is what produced the mismatch this key is
    # supposed to describe.
    lines.append(f"playlist_type: {yaml_quote(data.get('playlist_type') or _derive_playlist_type(data))}")
    
    # Loop as lowercase boolean
    loop_value = 'true' if data.get('loop', False) else 'false'
    lines.append(f"loop: {loop_value}")
    
    # Items list
    lines.append("items:")
    
    items = data.get('items', [])
    for item in items:
        # Each item starts with "  - title:" - quote the title
        lines.append(f"  - title: {yaml_quote(item.get('title', 'Untitled'))}")
        
        # Artist field (right after title, for music videos)
        artist = item.get('artist', '')
        lines.append(f"    artist: {yaml_quote(artist)}")
        
        lines.append(f"    source_type: {yaml_quote(item.get('source_type', 'local'))}")
        
        # Path is required for local/emulated_game types - MUST quote as paths often contain #
        if item.get('path'):
            lines.append(f"    path: {yaml_quote(item['path'])}")
        
        # Optional fields - only include if present
        if item.get('url'):
            lines.append(f"    url: {yaml_quote(item['url'])}")
        
        if item.get('start') is not None:
            lines.append(f"    start: {item['start']}")
        
        if item.get('end') is not None:
            lines.append(f"    end: {item['end']}")
        
        if item.get('tags'):
            # Format tags as YAML list - filter out invalid tags
            valid_tags = [t for t in item['tags'] if isinstance(t, str) and t.strip()]
            if valid_tags:
                lines.append("    tags:")
                for tag in valid_tags:
                    lines.append(f"      - {yaml_quote(tag)}")
        
        # Emulator fields for games — quote like every other field.
        if item.get('emulator_core'):
            lines.append(f"    emulator_core: {yaml_quote(item['emulator_core'])}")

        if item.get('emulator_system'):
            lines.append(f"    emulator_system: {yaml_quote(item['emulator_system'])}")
        
        # Add blank line between items for readability
        if item != items[-1]:  # Not the last item
            lines.append("")
    
    return '\n'.join(lines) + '\n'


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
                data = yaml.safe_load(p.read_text())
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
            data = yaml.safe_load(p.read_text())
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
                bad_core = _emulator_core_error(data)
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
