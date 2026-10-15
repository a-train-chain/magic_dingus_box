"""Game ROM routes: list, upload (with the multi-disc .m3u regeneration)
and delete.
"""
from __future__ import annotations

import subprocess
import sys
import threading
from pathlib import Path

from flask import request

# Same dual import form as admin.py: flat when tests put web/ on
# sys.path, package-relative in production.
try:
    from admin_common import (
        _sanitize_filename, _staged_save_upload, error_response,
        success_response,
    )
    from admin_security import _is_within
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from .admin_common import (
        _sanitize_filename, _staged_save_upload, error_response,
        success_response,
    )
    from .admin_security import _is_within


# Systems whose libraries contain multi-disc titles, so an upload should
# re-run the .m3u generator over that system's ROM directory. Cartridge
# systems are excluded — there is nothing to swap, and scanning them is a
# pointless subprocess on every upload.
MULTI_DISC_SYSTEMS = frozenset({"ps1", "dreamcast"})


def register(app, ctx) -> None:
    """Register this module's routes on `app`. Called exactly once, from
    admin.create_app(); `ctx` carries the create_app() state these routes
    share with the rest of the Content Manager (see admin.py)."""
    # create_app() state shared across route modules
    IMPORT_PEAK_MULTIPLIER = ctx.IMPORT_PEAK_MULTIPLIER
    _storage_precondition = ctx._storage_precondition
    data_dir = ctx.data_dir
    require_csrf = ctx.require_csrf
    roms_dir = ctx.roms_dir

    # ===== ROM MANAGEMENT =====

    @app.get("/admin/roms")
    def list_roms():  # type: ignore[no-redef]
        """List ROMs by system."""
        roms = {}

        # Helper to scan a directory and add to roms dict
        def scan_dir(base_dir: Path):
            if not base_dir.exists():
                return
            for system_dir in base_dir.iterdir():
                if system_dir.is_dir() and not system_dir.name.startswith('.'):
                    if system_dir.name not in roms:
                        roms[system_dir.name] = []

                    files = [
                        {
                            'filename': f.name,
                            'path': str(f.relative_to(data_dir.parent)),  # Always relative to app root
                            'size': f.stat().st_size
                        }
                        for f in sorted(system_dir.rglob("*"))
                        if f.is_file() and not f.name.startswith('.')
                    ]
                    roms[system_dir.name].extend(files)

        # 1. Scan uploaded ROMs
        scan_dir(roms_dir)

        # 2. Scan dev/pre-loaded ROMs
        dev_roms_dir = data_dir.parent / "dev_data" / "roms"
        scan_dir(dev_roms_dir)

        return success_response(data=roms)

    # Lock for serializing M3U generation
    m3u_lock = threading.Lock()

    @app.post("/admin/upload/rom/<system>")
    @require_csrf
    def upload_rom(system):  # type: ignore[no-redef]
        """Upload ROM for specific system."""
        # Spool + staging copy: two copies, exactly as for /admin/upload.
        _no_space = _storage_precondition(
            (request.content_length or 0) * IMPORT_PEAK_MULTIPLIER)
        if _no_space:
            return _no_space

        if "file" not in request.files:
            return error_response("VALIDATION_ERROR", "File field required")
        f = request.files["file"]

        # Sanitize system name and filename to prevent path traversal
        try:
            safe_system = _sanitize_filename(system)
            safe_filename = _sanitize_filename(f.filename)
        except ValueError as e:
            return error_response("VALIDATION_ERROR", str(e))

        out = roms_dir / safe_system / safe_filename
        # Ensure path stays within roms_dir
        out_resolved = out.resolve()
        roms_dir_resolved = roms_dir.resolve()
        if not _is_within(out_resolved, roms_dir_resolved):
            return error_response("VALIDATION_ERROR", "Invalid path")

        _staged_save_upload(f, out)

        # Auto-generate M3U playlists for multi-disc games.
        #
        # This was gated on 'ps1' and invoked the generator with no argument,
        # so it always scanned the PS1 directory no matter what was uploaded.
        # Dreamcast ships two-disc titles of its own (Resident Evil - Code -
        # Veronica, Skies of Arcadia) and flycast reads .m3u exactly like
        # pcsx_rearmed does — without this, uploading disc 2 of a Dreamcast
        # game through the Content Manager left the two discs as separate,
        # unswappable playlist entries.
        if safe_system.lower() in MULTI_DISC_SYSTEMS:
            system_rom_dir = out.parent

            def run_m3u_generator():
                # Acquire lock to ensure only one script instance runs at a time
                with m3u_lock:
                    try:
                        # Same base as UPDATE_SCRIPT: data_dir.parent IS
                        # .../magic_dingus_box_cpp, so the script lives at
                        # data_dir.parent/scripts. This used to insert a second
                        # "magic_dingus_box_cpp" component — a path that exists
                        # on no box, so exists() was always False and multi-disc
                        # .m3u generation silently never ran after an upload.
                        script_path = data_dir.parent / "scripts" / "generate_m3u_playlists.sh"
                        if script_path.exists():
                            # Via bash, not exec: immune to a tarball that
                            # drops the mode bit (see network_doctor).
                            subprocess.run(
                                ["/bin/bash", str(script_path), str(system_rom_dir)],
                                capture_output=True,
                                timeout=30
                            )
                    except Exception as e:
                        print(f"M3U generator error: {e}", file=sys.stderr)
            
            # Run in background thread to not block response
            thread = threading.Thread(target=run_m3u_generator)
            thread.daemon = True
            thread.start()
        
        return success_response(data={"path": str(out.relative_to(data_dir.parent))}, message="ROM uploaded")

    @app.delete("/admin/roms/<path:filepath>")
    @require_csrf
    def delete_rom(filepath):  # type: ignore[no-redef]
        """Delete a ROM file."""
        # filepath is relative to /opt/magic_dingus_box (parent of data_dir)
        target = data_dir.parent / filepath

        # Security check: must live under data/roms or dev_data/roms. Use the
        # resolved-path containment helper, NOT str.startswith — the latter
        # matches sibling dirs (e.g. "roms_backup" as inside "roms"), which
        # for a DELETE endpoint taking a <path:...> is arbitrary-file-delete.
        dev_roms_dir = data_dir.parent / "dev_data" / "roms"
        if not (_is_within(target, roms_dir) or _is_within(target, dev_roms_dir)):
            return error_response("VALIDATION_ERROR", "File is not a ROM")

        if target.exists() and target.is_file():
            target.unlink()
            return success_response(message="ROM deleted")
        return error_response("NOT_FOUND", "ROM not found", status=404)
