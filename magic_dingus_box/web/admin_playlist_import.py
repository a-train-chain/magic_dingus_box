"""Playlist import routes: a single YAML file, and the playlist package
(ZIP of playlist + videos/ROMs) with its storage precheck.

POST /admin/playlists/import, /admin/playlists/import-package/precheck and
/admin/playlists/import-package.
"""
from __future__ import annotations

import io
import os
import re
import sys
import tempfile
import yaml
import zipfile
from pathlib import Path

from flask import request
from werkzeug.exceptions import HTTPException

# Same dual import form as admin.py: flat when tests put web/ on
# sys.path, package-relative in production.
try:
    from admin_common import (
        _ExtractTooLarge, _read_zip_entry_capped, _sanitize_filename,
        _ZIP_TEXT_ENTRY_MAX, _ZipEntryTooLarge, BYTES_PER_GB, error_response,
        success_response,
    )
    from admin_playlists import (
        _emulator_core_error, _playlist_filename_for_title, _playlist_summary,
        format_playlist_yaml,
    )
    from admin_security import _is_within
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from .admin_common import (
        _ExtractTooLarge, _read_zip_entry_capped, _sanitize_filename,
        _ZIP_TEXT_ENTRY_MAX, _ZipEntryTooLarge, BYTES_PER_GB, error_response,
        success_response,
    )
    from .admin_playlists import (
        _emulator_core_error, _playlist_filename_for_title, _playlist_summary,
        format_playlist_yaml,
    )
    from .admin_security import _is_within


def register(app, ctx) -> None:
    """Register this module's routes on `app`. Called exactly once, from
    admin.create_app(); `ctx` carries the create_app() state these routes
    share with the rest of the Content Manager (see admin.py)."""
    # create_app() state shared across route modules
    IMPORT_PEAK_MULTIPLIER = ctx.IMPORT_PEAK_MULTIPLIER
    STORAGE_HEADROOM_BYTES = ctx.STORAGE_HEADROOM_BYTES
    _poke_kiosk_reload = ctx._poke_kiosk_reload
    _storage_precondition = ctx._storage_precondition
    data_dir = ctx.data_dir
    media_dir = ctx.media_dir
    playlists_dir = ctx.playlists_dir
    require_csrf = ctx.require_csrf
    roms_dir = ctx.roms_dir
    # Defined/re-exported in admin.py; tests monkeypatch admin.<name>, so
    # these are shims that look the name up in admin at call time.
    _atomic_write_text = ctx._atomic_write_text
    get_free_bytes = ctx.get_free_bytes

    @app.post("/admin/playlists/import")
    @require_csrf
    def import_playlist():  # type: ignore[no-redef]
        """Import a playlist from a YAML file.
        
        Accepts multipart form upload with a .yaml or .yml file.
        The playlist will be saved with either the filename from the upload
        or extracted from the 'title' field in the YAML.
        
        Query params:
            overwrite: If 'true', overwrite existing playlist with same name
        """
        try:
            if 'file' not in request.files:
                return error_response("VALIDATION_ERROR", "No file provided")
            
            file = request.files['file']
            if not file.filename:
                return error_response("VALIDATION_ERROR", "No filename")
            
            # Validate file extension
            original_filename = file.filename
            if not original_filename.lower().endswith(('.yaml', '.yml')):
                return error_response(
                    "VALIDATION_ERROR", 
                    "File must be a .yaml or .yml file"
                )
            
            # Read and parse YAML content
            try:
                yaml_content = file.read().decode('utf-8')
                data = yaml.safe_load(yaml_content)
            except UnicodeDecodeError:
                return error_response("VALIDATION_ERROR", "File must be valid UTF-8 text")
            except yaml.YAMLError as e:
                return error_response("VALIDATION_ERROR", f"Invalid YAML: {e}")
            
            if not data:
                return error_response("VALIDATION_ERROR", "Empty YAML file")
            
            # Validate basic playlist structure
            if not isinstance(data, dict):
                return error_response("VALIDATION_ERROR", "Playlist must be a YAML object")
            
            if 'items' not in data or not isinstance(data.get('items'), list):
                return error_response(
                    "VALIDATION_ERROR", 
                    "Playlist must have an 'items' list"
                )

            bad_core = _emulator_core_error(data)
            if bad_core:
                return bad_core
            
            # Determine output filename.
            # Prefer 'title' from YAML, fall back to the uploaded filename —
            # which now also covers a title that slugs to nothing (emoji-only).
            # That case used to reach _sanitize_filename as the bare string
            # '.yaml' and come back as an extension error, which is a baffling
            # thing to tell someone who supplied a perfectly good title.
            title = (data.get('title') or '').strip()
            try:
                safe_name = _playlist_filename_for_title(
                    title, fallback=original_filename)
            except ValueError as e:
                return error_response("VALIDATION_ERROR", str(e))

            # Check if already exists
            output_path = playlists_dir / safe_name
            overwrite = request.args.get('overwrite', 'false').lower() == 'true'
            
            if output_path.exists() and not overwrite:
                return error_response(
                    "ALREADY_EXISTS", 
                    f"Playlist '{safe_name}' already exists. Set overwrite=true to replace.",
                    status=409
                )
            
            # Re-format YAML through our formatter for consistency
            formatted_yaml = format_playlist_yaml(data)
            
            # Save
            _atomic_write_text(output_path, formatted_yaml)
            _poke_kiosk_reload("playlists")

            item_count = len(data.get('items', []))
            print(f"Imported playlist: {safe_name} ({item_count} items)", file=sys.stderr)
            
            return success_response(
                data={
                    "filename": safe_name,
                    "title": data.get('title', safe_name),
                    "item_count": item_count,
                    "overwritten": output_path.exists() and overwrite,
                },
                message=f"Playlist imported successfully with {item_count} items"
            )
            
        except HTTPException:
            # Werkzeug raises RequestEntityTooLarge (a 413 HTTPException)
            # when MAX_CONTENT_LENGTH is exceeded. Swallowing it here would
            # turn it into a 500 with a raw message and bypass the
            # errorhandler(413) that returns the standard JSON envelope.
            raise
        except Exception as e:
            print(f"Error importing playlist: {e}", file=sys.stderr)
            import traceback
            traceback.print_exc()
            return error_response("INTERNAL_ERROR", str(e), status=500)

    @app.post("/admin/playlists/import-package/precheck")
    @require_csrf
    def import_package_precheck():  # type: ignore[no-redef]
        """Answer the two questions an import raises, BEFORE the bytes move.

        A package is gigabytes over Wi-Fi. Discovering at the end of that
        upload that the filename was already taken, or that the card was never
        going to hold it, wastes the whole transfer and — in the storage case —
        does damage on the way, because the staged ZIP is on the card by then.

        Deliberately takes JSON only and refuses a file body: an endpoint that
        accepted the upload in order to tell you not to send the upload would
        answer the question too late to be worth asking.
        """
        if request.files:
            return error_response(
                "VALIDATION_ERROR",
                "Precheck takes JSON only; do not attach the package")

        body = request.get_json(silent=True)
        if not isinstance(body, dict):
            return error_response("VALIDATION_ERROR", "JSON body required")

        title = str(body.get("title") or "").strip()
        try:
            size_bytes = int(body.get("size_bytes") or 0)
        except (TypeError, ValueError):
            return error_response("VALIDATION_ERROR", "size_bytes must be an integer")
        if size_bytes < 0:
            return error_response("VALIDATION_ERROR", "size_bytes must not be negative")

        try:
            filename = _playlist_filename_for_title(title)
        except ValueError as e:
            return error_response("VALIDATION_ERROR", str(e))

        existing = playlists_dir / filename
        exists = existing.is_file()
        summary = _playlist_summary(existing) if exists else None

        # Same arithmetic the import itself will apply, so a green precheck and
        # a 507 from the real POST cannot disagree.
        needed = size_bytes * IMPORT_PEAK_MULTIPLIER
        free = get_free_bytes(data_dir)
        if free is None or needed <= 0:
            # Unknown, not full — see _storage_precondition. Report optimistic
            # so the client does not block an upload we would have accepted.
            enough_space = True
        else:
            enough_space = (free - STORAGE_HEADROOM_BYTES) >= needed

        return success_response(data={
            "exists": exists,
            "existing_filename": filename if exists else None,
            "existing_title": summary["title"] if exists else None,
            "existing_item_count": summary["item_count"] if exists else None,
            "free_gb": round((free or 0) / BYTES_PER_GB, 2),
            "enough_space": enough_space,
            "max_upload_mb": int(
                (app.config.get("MAX_CONTENT_LENGTH") or 0) // (1024 * 1024)),
        })

    @app.post("/admin/playlists/import-package")
    @require_csrf
    def import_package():  # type: ignore[no-redef]
        """Import a complete playlist package (ZIP with videos + playlist YAML).

        Accepts a ZIP file containing:
        - playlist.yaml (required) - the playlist definition
        - media/*.mp4 (optional) - video files to upload

        Videos are extracted to data/media/, then the playlist is imported.

        Query params:
            overwrite: If 'true', overwrite existing playlist/videos with same names
        """
        # Free-space precondition, deliberately ahead of the extension check.
        # Reading request.files makes werkzeug spool the whole multipart body
        # onto the SD card, so any validation placed above this line has
        # already spent the space we are trying to protect. Budget two copies:
        # werkzeug's spool of the ZIP and the extracted media coexist at the
        # peak. (That is only true because the ZIP is opened straight from
        # the spool below — it used to be copied a second time first.)
        _no_space = _storage_precondition(
            (request.content_length or 0) * IMPORT_PEAK_MULTIPLIER)
        if _no_space:
            return _no_space

        try:
            if 'file' not in request.files:
                return error_response("VALIDATION_ERROR", "No file provided")

            file = request.files['file']
            if not file.filename:
                return error_response("VALIDATION_ERROR", "No filename")

            # Validate file extension
            if not file.filename.lower().endswith('.zip'):
                return error_response(
                    "VALIDATION_ERROR",
                    "File must be a .zip file"
                )

            overwrite = request.args.get('overwrite', 'false').lower() == 'true'

            # Open the ZIP straight from werkzeug's upload spool. It is
            # already a complete, seekable copy of the upload (on the card
            # once past 500 KB), and it lives until the request ends anyway.
            # The old "hybrid" handling made a SECOND full copy first —
            # file.save() to a mkstemp at >=100 MB (three copies on the card
            # at the peak against a two-copy space budget), or the whole
            # upload read() into RAM below 100 MB (up to 100 MB of heap on a
            # 1.5 GB Pi 4B). The staged-copy path survives only as a
            # fallback for a stream zipfile cannot seek.
            temp_path = None

            try:
                zf = None
                stream = file.stream
                if hasattr(stream, "seek") and hasattr(stream, "tell"):
                    try:
                        stream.seek(0)
                        zf = zipfile.ZipFile(stream, 'r')
                    except (io.UnsupportedOperation, AttributeError, OSError):
                        zf = None
                if zf is None:
                    fd, temp_path = tempfile.mkstemp(suffix='.zip')
                    os.close(fd)
                    file.save(temp_path)
                    zf = zipfile.ZipFile(temp_path, 'r')

                with zf:
                    # List contents
                    namelist = zf.namelist()

                    # Find playlist YAML (can be at root or in a subfolder)
                    playlist_file = None
                    for name in namelist:
                        basename = os.path.basename(name)
                        if basename.lower() in ('playlist.yaml', 'playlist.yml'):
                            playlist_file = name
                            break

                    if not playlist_file:
                        return error_response(
                            "VALIDATION_ERROR",
                            "ZIP must contain a playlist.yaml file"
                        )

                    # Parse playlist YAML
                    try:
                        # Capped: see _ZIP_TEXT_ENTRY_MAX.
                        yaml_content = _read_zip_entry_capped(
                            zf, playlist_file).decode('utf-8')
                        playlist_data = yaml.safe_load(yaml_content)
                    except _ZipEntryTooLarge:
                        return error_response(
                            "VALIDATION_ERROR",
                            "playlist.yaml is too large to be a playlist "
                            f"(limit {_ZIP_TEXT_ENTRY_MAX // (1024 * 1024)} MB)")
                    except UnicodeDecodeError:
                        return error_response("VALIDATION_ERROR", "playlist.yaml must be valid UTF-8")
                    except yaml.YAMLError as e:
                        return error_response("VALIDATION_ERROR", f"Invalid YAML: {e}")

                    if not playlist_data or not isinstance(playlist_data, dict):
                        return error_response("VALIDATION_ERROR", "Invalid playlist structure")

                    if 'items' not in playlist_data or not isinstance(playlist_data.get('items'), list):
                        return error_response("VALIDATION_ERROR", "Playlist must have an 'items' list")

                    # Before ANY file is extracted: a refused package must
                    # leave nothing behind.
                    bad_core = _emulator_core_error(playlist_data)
                    if bad_core:
                        return bad_core

                    # Determine output filename for playlist BEFORE extracting
                    # videos. This slugged the title inline and skipped
                    # _sanitize_filename entirely — unlike the single-YAML
                    # import path — so a title with no word characters wrote
                    # the literal file '.yaml' and reported success. See
                    # _playlist_filename_for_title for why that file is both
                    # invisible to the kiosk and impossible to delete again.
                    title = (playlist_data.get('title') or '').strip()
                    try:
                        output_name = _playlist_filename_for_title(title)
                    except ValueError as e:
                        return error_response("VALIDATION_ERROR", str(e))

                    # Check if playlist exists BEFORE extracting any files.
                    #
                    # The slug is lossy in a way the operator cannot see: it
                    # drops every non-word character, so 'Rock & Roll',
                    # 'Rock, Roll' and 'Rock Roll' all land on Rock_Roll.yaml.
                    # The old message named only the FILENAME, which is not
                    # something the operator ever typed — so "Rock_Roll.yaml
                    # already exists" gave them no way to tell an accidental
                    # re-import of the same playlist from a genuine collision
                    # with a different one they would be destroying. Report
                    # both titles and let the UI ask a question worth
                    # answering.
                    playlist_path = playlists_dir / output_name
                    if playlist_path.exists() and not overwrite:
                        incumbent = _playlist_summary(playlist_path)
                        incoming_title = title or output_name
                        if incumbent["title"] == incoming_title:
                            detail = (f"Playlist '{incumbent['title']}' is already on "
                                      "this box. Set overwrite=true to replace it.")
                        else:
                            detail = (f"'{incoming_title}' and '{incumbent['title']}' "
                                      f"both save as {output_name}. Importing would "
                                      f"replace '{incumbent['title']}'.")
                        return error_response(
                            "ALREADY_EXISTS",
                            detail,
                            status=409,
                            details={
                                "existing_title": incumbent["title"],
                                "existing_filename": output_name,
                                "incoming_title": incoming_title,
                            },
                        )

                    # Find media files in the ZIP
                    media_files = []
                    rom_files = []
                    for name in namelist:
                        lower_name = name.lower()
                        if lower_name.endswith(('.mp4', '.mkv', '.avi', '.mov', '.webm')):
                            # Accept files in media/ folder or root level video files
                            media_files.append(name)
                            continue
                        # ROM entries are identified by LOCATION, not extension.
                        # ROM extensions are wildly varied (.7z .z64 .n64 .v64
                        # .sfc .smc .nes .md .gg .pce .a78 .bin .cue .iso .gdi
                        # .chd) and .zip collides with the package itself, so a
                        # suffix list would be both incomplete and ambiguous.
                        # The archive layout mirrors the on-disk one:
                        #     roms/<system>/<file>   or   data/roms/<system>/<file>
                        # which is also exactly what a game playlist's own
                        # `path:` values look like, so a package can be built by
                        # copying the referenced files in at their stated paths.
                        parts = name.split('/')
                        if parts[:1] == ['data']:
                            parts = parts[1:]
                        if len(parts) >= 3 and parts[0] == 'roms' and parts[-1]:
                            rom_files.append((name, parts[1], parts[-1]))

                    # Guard against ZIP bombs: limit total extracted size to
                    # the configured cap (default 10GB) AND to what the card
                    # can actually take. The fixed 10 GB alone let a bomb fill
                    # a box with 3 GB free to the last byte — and the kiosk's
                    # settings.json, RetroArch saves and paired-remote state
                    # live on the same volume. Measured NOW, after the ZIP is
                    # staged, and keeping the same reserve every upload keeps.
                    MAX_EXTRACT_BYTES = int(os.getenv("MAGIC_MAX_EXTRACT_MB", "10240")) * 1024 * 1024
                    _free_now = [
                        fb for fb in (get_free_bytes(d) for d in (
                            media_dir if media_dir.exists() else data_dir,
                            roms_dir if roms_dir.exists() else data_dir))
                        if fb is not None
                    ]
                    extract_limited_by_space = False
                    if _free_now:
                        _space_cap = max(0, min(_free_now) - STORAGE_HEADROOM_BYTES)
                        if _space_cap < MAX_EXTRACT_BYTES:
                            MAX_EXTRACT_BYTES = _space_cap
                            extract_limited_by_space = True
                    total_extracted = 0

                    def _too_large_response():
                        if extract_limited_by_space:
                            return error_response(
                                "INSUFFICIENT_STORAGE",
                                "Not enough free space on this box to unpack that package.",
                                status=507,
                                details={"free_gb": round(MAX_EXTRACT_BYTES / BYTES_PER_GB, 2)},
                            )
                        return error_response(
                            "VALIDATION_ERROR",
                            f"Package exceeds maximum extraction size ({MAX_EXTRACT_BYTES // (1024*1024)}MB)",
                            status=413
                        )

                    def _stage_extract(member, dest_path):
                        """Extract one ZIP member to dest_path, atomically.

                        Stages into a temp file in the destination directory and
                        only os.replace()s it into position once the whole entry
                        is written and fsynced, so a failure never damages a file
                        that is already there. Returns bytes written. Raises
                        _ExtractTooLarge if the entry would exceed the cap.
                        """
                        nonlocal total_extracted
                        written = 0
                        remaining = MAX_EXTRACT_BYTES - total_extracted
                        dest_path.parent.mkdir(parents=True, exist_ok=True)
                        fd, tmp_name = tempfile.mkstemp(
                            dir=str(dest_path.parent),
                            prefix=f".{dest_path.name}.",
                            suffix=".part",
                        )
                        tmp = Path(tmp_name)
                        try:
                            with zf.open(member) as src, os.fdopen(fd, 'wb') as dst:
                                while True:
                                    chunk = src.read(64 * 1024)
                                    if not chunk:
                                        break
                                    written += len(chunk)
                                    if written > remaining:
                                        raise _ExtractTooLarge()
                                    dst.write(chunk)
                                dst.flush()
                                os.fsync(dst.fileno())
                        except BaseException:
                            tmp.unlink(missing_ok=True)
                            raise
                        os.chmod(tmp, 0o644)
                        os.replace(tmp, dest_path)
                        total_extracted += written
                        return written

                    # Extract ROM files. Placement matches upload_rom() exactly —
                    # roms_dir/<system>/<file> — so a ROM that arrives in a
                    # package is indistinguishable from one uploaded directly.
                    roms_imported = 0
                    roms_skipped = 0
                    rom_renames = {}
                    for rom_entry, rom_system, rom_name in rom_files:
                        try:
                            safe_system = _sanitize_filename(rom_system)
                            safe_rom = _sanitize_filename(rom_name)
                        except ValueError:
                            continue

                        rom_out = roms_dir / safe_system / safe_rom
                        if not _is_within(rom_out.resolve(), roms_dir.resolve()):
                            return error_response("VALIDATION_ERROR", "Invalid ROM path in package")

                        if rom_out.exists() and not overwrite:
                            roms_skipped += 1
                            rom_renames[rom_name] = f"data/roms/{safe_system}/{safe_rom}"
                            continue

                        try:
                            _stage_extract(rom_entry, rom_out)
                        except _ExtractTooLarge:
                            return _too_large_response()
                        rom_renames[rom_name] = f"data/roms/{safe_system}/{safe_rom}"
                        roms_imported += 1

                    # Extract media files
                    videos_imported = 0
                    videos_skipped = 0
                    for media_file in media_files:
                        # Get just the filename (strip directory path)
                        filename = os.path.basename(media_file)
                        if not filename:
                            continue

                        # Sanitize filename
                        safe_filename = re.sub(r'[^\w\s\-\.\[\]]', '', filename)
                        safe_filename = safe_filename.strip()
                        if not safe_filename:
                            continue

                        # Check declared size as a cheap pre-check, but DO NOT
                        # rely on it — `info.file_size` is attacker-controlled
                        # in a crafted ZIP and can be set to 0 to bypass the
                        # quota. Real enforcement happens during streaming
                        # below by counting bytes actually written.
                        info = zf.getinfo(media_file)
                        if total_extracted + info.file_size > MAX_EXTRACT_BYTES:
                            return _too_large_response()

                        output_path = media_dir / safe_filename

                        # Check if exists
                        if output_path.exists() and not overwrite:
                            videos_skipped += 1
                            continue

                        # Extract to media directory
                        media_dir.mkdir(parents=True, exist_ok=True)

                        # Stream extraction with a hard byte cap. Aborts and
                        # deletes the partial file if any single entry would
                        # push us past MAX_EXTRACT_BYTES — defends against ZIP
                        # bombs that lie in the central directory's file_size.
                        # Stage into a temp file in the SAME directory and only
                        # move it into place once the whole entry is written.
                        #
                        # This used to open(output_path, 'wb') directly, which
                        # truncates an existing video the instant it is called —
                        # before anything has verified the replacement. Both
                        # bail-out paths below then unlink()ed it. So an operator
                        # re-importing with overwrite=true against a package that
                        # trips the size cap lost videos that were already on the
                        # box and got nothing back: the original was destroyed at
                        # open() and the partial was deleted on abort.
                        #
                        # Staging makes every failure a no-op for the existing
                        # file, and makes the swap atomic so a reader never sees
                        # a half-written video.
                        actual_written = 0
                        remaining = MAX_EXTRACT_BYTES - total_extracted
                        tmp_fd, tmp_name = tempfile.mkstemp(
                            dir=str(output_path.parent),
                            prefix=f".{output_path.name}.",
                            suffix=".part",
                        )
                        tmp_media = Path(tmp_name)
                        try:
                            with zf.open(media_file) as src, os.fdopen(tmp_fd, 'wb') as dst:
                                while True:
                                    chunk = src.read(64 * 1024)
                                    if not chunk:
                                        break
                                    actual_written += len(chunk)
                                    if actual_written > remaining:
                                        raise _ExtractTooLarge()
                                    dst.write(chunk)
                                dst.flush()
                                os.fsync(dst.fileno())
                        except _ExtractTooLarge:
                            tmp_media.unlink(missing_ok=True)
                            return _too_large_response()
                        except BaseException:
                            tmp_media.unlink(missing_ok=True)
                            raise

                        # mkstemp creates 0600; media must stay readable by the
                        # kiosk process, matching what f.save() produced before.
                        os.chmod(tmp_media, 0o644)
                        os.replace(tmp_media, output_path)

                        total_extracted += actual_written
                        videos_imported += 1

                # Update playlist paths to use sanitized filenames (matching what we saved)
                for item in playlist_data.get('items', []):
                    if not isinstance(item, dict) or 'path' not in item:
                        continue
                    path = item['path']
                    source_type = item.get('source_type')

                    if source_type == 'emulated_game':
                        # Point at wherever the ROM actually landed. Without
                        # this the item keeps the packager's path, which is only
                        # correct by luck.
                        new_path = rom_renames.get(os.path.basename(path))
                        if new_path:
                            item['path'] = new_path
                    elif source_type in (None, '', 'local', 'video'):
                        # None/'' and the legacy 'video' alias are all treated as
                        # local by playlist_loader.cpp (source_type defaults to
                        # "local" when absent). This branch tested only for an
                        # explicit 'local', so a third-party package that omitted
                        # source_type had its videos copied onto the box while the
                        # playlist kept pointing at the packager's original paths
                        # — every item silently unplayable.
                        basename = os.path.basename(path)
                        # Sanitize the basename the same way we sanitized the files
                        safe_basename = re.sub(r'[^\w\s\-\.\[\]]', '', basename)
                        safe_basename = safe_basename.strip()
                        if safe_basename:
                            item['path'] = f"media/{safe_basename}"

                # Format and save playlist
                formatted_yaml = format_playlist_yaml(playlist_data)
                _atomic_write_text(playlist_path, formatted_yaml)
                _poke_kiosk_reload("playlists")

                item_count = len(playlist_data.get('items', []))
                print(f"Imported package: {output_name} ({item_count} items, "
                      f"{videos_imported} videos, {roms_imported} ROMs, "
                      f"{videos_skipped} videos + {roms_skipped} ROMs skipped)",
                      file=sys.stderr)

                # A skip is not a failure but it is not a success either: the
                # file was already on the box, so the operator's newer copy was
                # silently discarded. Both counters were computed and only one
                # was reported, and the UI dropped that one too — so an import
                # of a package whose every file already existed rendered
                # identically to one that landed in full. Keep both keys
                # present unconditionally so the client can render zero.
                skipped_note = ""
                if videos_skipped or roms_skipped:
                    skipped_note = (f" ({videos_skipped} videos, {roms_skipped} "
                                    "ROMs already on the box were kept)")

                return success_response(
                    data={
                        "playlist_filename": output_name,
                        "playlist_title": playlist_data.get('title', output_name),
                        "item_count": item_count,
                        "videos_imported": videos_imported,
                        "roms_imported": roms_imported,
                        "videos_skipped": videos_skipped,
                        "roms_skipped": roms_skipped,
                    },
                    message=(f"Package imported: {item_count} playlist items, "
                             f"{videos_imported} videos, {roms_imported} ROMs"
                             f"{skipped_note}")
                )

            except zipfile.BadZipFile:
                return error_response("VALIDATION_ERROR", "Invalid ZIP file")
            finally:
                # cleanup temp file
                if temp_path and os.path.exists(temp_path):
                    os.unlink(temp_path)

        except HTTPException:
            # Werkzeug raises RequestEntityTooLarge (a 413 HTTPException)
            # when MAX_CONTENT_LENGTH is exceeded. Swallowing it here would
            # turn it into a 500 with a raw message and bypass the
            # errorhandler(413) that returns the standard JSON envelope.
            raise
        except Exception as e:
            print(f"Error importing package: {e}", file=sys.stderr)
            import traceback
            traceback.print_exc()
            return error_response("INTERNAL_ERROR", str(e), status=500)
