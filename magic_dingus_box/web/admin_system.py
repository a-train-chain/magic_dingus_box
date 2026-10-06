"""Device identity, health and backup/restore routes.

/admin/device/info, /admin/device/name, /admin/health,
/admin/health/detailed, /admin/backup and /admin/restore, plus the
device-name validators and get_device_info()/get_app_version(), which the
support and update routes reuse through ctx.
"""
from __future__ import annotations

import io
import json
import os
import re
import socket
import time
import uuid
import yaml
import zipfile
from datetime import datetime
from typing import Optional

from flask import request, send_file

# Same dual import form as admin.py: flat when tests put web/ on
# sys.path, package-relative in production.
try:
    from admin_common import (
        _read_zip_entry_capped, _sanitize_filename, error_response,
        get_cpu_temperature, get_cpu_usage, get_disk_info, get_memory_info,
        get_uptime, success_response,
    )
    from admin_playlists import (
        _canonical_playlist_name, _invalid_emulator_core,
    )
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from .admin_common import (
        _read_zip_entry_capped, _sanitize_filename, error_response,
        get_cpu_temperature, get_cpu_usage, get_disk_info, get_memory_info,
        get_uptime, success_response,
    )
    from .admin_playlists import (
        _canonical_playlist_name, _invalid_emulator_core,
    )


DEVICE_NAME_MAX = 64
_CONTROL_CHARS_RE = re.compile(r"[\x00-\x1f\x7f]")


def _clean_device_name(value) -> Optional[str]:
    """The trimmed name if `value` is an acceptable device name, else None.

    A display label shown on every Content Manager screen and in the mDNS
    device list: a string, 1..DEVICE_NAME_MAX chars after trimming, no
    control characters (a newline or NUL would break the one-line labels
    and the JSON-lines tooling that greps this file)."""
    if not isinstance(value, str):
        return None
    name = value.strip()
    if not name or len(name) > DEVICE_NAME_MAX or _CONTROL_CHARS_RE.search(name):
        return None
    return name


def _device_info_problem(doc) -> Optional[str]:
    """Why `doc` cannot be a device_info.json, or None if it can. It must be
    a JSON object; device_name / device_id, when present, plain strings."""
    if not isinstance(doc, dict):
        return "device_info.json must be a JSON object"
    if "device_name" in doc and _clean_device_name(doc["device_name"]) is None:
        return f"device_name must be text of 1-{DEVICE_NAME_MAX} characters"
    if "device_id" in doc and not (isinstance(doc["device_id"], str)
                                   and 0 < len(doc["device_id"]) <= 128):
        return "device_id must be a short string"
    return None


def get_local_ip() -> str:
    """Get local IP address of this device."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "unknown"


def register(app, ctx) -> None:
    """Register this module's routes on `app`. Called exactly once, from
    admin.create_app(); `ctx` carries the create_app() state these routes
    share with the rest of the Content Manager (see admin.py)."""
    # create_app() state shared across route modules
    _poke_kiosk_reload = ctx._poke_kiosk_reload
    data_dir = ctx.data_dir
    device_info_file = ctx.device_info_file
    media_dir = ctx.media_dir
    playlists_dir = ctx.playlists_dir
    require_csrf = ctx.require_csrf
    roms_dir = ctx.roms_dir
    # Defined/re-exported in admin.py; tests monkeypatch admin.<name>, so
    # these are shims that look the name up in admin at call time.
    _atomic_write_text = ctx._atomic_write_text
    check_service_status = ctx.check_service_status

    def get_device_info() -> dict:
        """Get device identity and stats."""
        try:
            info = None
            if device_info_file.exists():
                info = json.loads(device_info_file.read_text())
                # A wrong-shaped file (e.g. restored before restore checked
                # the shape) must not 500 this endpoint forever — it is the
                # first call the Content Manager makes to find the box. Fall
                # back to defaults; a rename rewrites the file properly.
                if not isinstance(info, dict):
                    info = None
                elif _clean_device_name(info.get('device_name')) is None:
                    info['device_name'] = 'Magic Dingus Box'
            if info is None:
                info = {
                    'device_id': 'unknown',
                    'device_name': 'Magic Dingus Box'
                }

            # Add runtime info
            info['hostname'] = socket.gethostname()
            info['local_ip'] = get_local_ip()
            
            # Add content stats
            info['stats'] = {
                'playlists': len(list(playlists_dir.glob("*.y*ml"))) if playlists_dir.exists() else 0,
                'videos': len(list(media_dir.rglob("*.mp4"))) if media_dir.exists() else 0,
                'roms': sum(1 for _ in roms_dir.rglob("*") if _.is_file()) if roms_dir.exists() else 0,
            }
            
            return info
        except Exception as e:
            return {'error': str(e), 'device_name': 'Unknown Device'}

    # ===== DEVICE MANAGEMENT =====

    @app.get("/admin/device/info")
    def device_info():  # type: ignore[no-redef]
        """Get this device's identity and stats."""
        info = get_device_info()
        if 'error' in info:
            return error_response("DEVICE_ERROR", info['error'], status=500)
        return success_response(data=info)

    @app.post("/admin/device/name")
    @require_csrf
    def set_device_name():  # type: ignore[no-redef]
        """Set/update device name."""
        data = request.get_json(silent=True)
        if not data or not isinstance(data, dict):
            return error_response("VALIDATION_ERROR", "JSON object body required")
        new_name = _clean_device_name(data.get('name', 'Magic Dingus Box'))
        if new_name is None:
            return error_response(
                "VALIDATION_ERROR",
                f"Name must be text of 1-{DEVICE_NAME_MAX} characters")

        try:
            info = None
            if device_info_file.exists():
                try:
                    info = json.loads(device_info_file.read_text())
                except json.JSONDecodeError:
                    info = None
            if not isinstance(info, dict):
                # Missing, corrupt, or wrong-shaped: start a fresh record
                # (this is also how a bad restored file gets healed).
                info = {'device_id': str(uuid.uuid4())}

            info['device_name'] = new_name
            _atomic_write_text(device_info_file, json.dumps(info, indent=2))
            return success_response(data={"device_name": new_name}, message="Device name updated")
        except Exception as e:
            return error_response("INTERNAL_ERROR", str(e), status=500)

    # ===== HEALTH CHECK & MONITORING =====

    @app.get("/admin/health")
    def health():  # type: ignore[no-redef]
        """Basic health check endpoint."""
        return success_response(message="Service is healthy")

    @app.get("/admin/health/detailed")
    def health_detailed():  # type: ignore[no-redef]
        """Detailed health and system monitoring endpoint."""
        # Gather system stats
        stats = {
            "status": "healthy",
            "timestamp": time.time(),
        }

        # CPU temperature
        cpu_temp = get_cpu_temperature()
        if cpu_temp is not None:
            stats["cpu_temperature_c"] = cpu_temp
            # Warn if temperature is high (Pi throttles at 80C)
            if cpu_temp > 75:
                stats["status"] = "warning"
                stats["warnings"] = stats.get("warnings", []) + ["CPU temperature high"]

        # CPU usage
        cpu_usage = get_cpu_usage()
        if cpu_usage is not None:
            stats["cpu_percent"] = cpu_usage

        # Memory usage
        memory = get_memory_info()
        if memory:
            stats["memory"] = memory
            if memory.get("percent", 0) > 90:
                stats["status"] = "warning"
                stats["warnings"] = stats.get("warnings", []) + ["Memory usage high"]

        # Disk usage
        disk = get_disk_info("/")
        if disk:
            stats["disk"] = disk
            if disk.get("percent", 0) > 90:
                stats["status"] = "warning"
                stats["warnings"] = stats.get("warnings", []) + ["Disk usage high"]

        # System uptime
        uptime = get_uptime()
        if uptime is not None:
            stats["uptime_seconds"] = uptime
            # Format as human-readable
            days = uptime // 86400
            hours = (uptime % 86400) // 3600
            minutes = (uptime % 3600) // 60
            if days > 0:
                stats["uptime_human"] = f"{days}d {hours}h {minutes}m"
            elif hours > 0:
                stats["uptime_human"] = f"{hours}h {minutes}m"
            else:
                stats["uptime_human"] = f"{minutes}m"

        # Service status (check main app service)
        app_status = check_service_status("magic-dingus-box-cpp")
        stats["app_service"] = app_status
        if app_status != "active":
            stats["status"] = "degraded"
            stats["warnings"] = stats.get("warnings", []) + [f"App service is {app_status}"]

        # Content stats
        stats["content"] = {
            "playlists": len(list(playlists_dir.glob("*.y*ml"))) if playlists_dir.exists() else 0,
            "videos": len(list(media_dir.rglob("*.mp4"))) if media_dir.exists() else 0,
            "roms": sum(1 for _ in roms_dir.rglob("*") if _.is_file()) if roms_dir.exists() else 0,
        }

        return success_response(data=stats)

    # ===== BACKUP & RESTORE =====

    def get_app_version() -> str:
        """Get the current installed app version from VERSION file."""
        # VERSION file is at /opt/magic_dingus_box/VERSION (two levels up from data dir)
        version_file = data_dir.parent.parent / "VERSION"
        if version_file.exists():
            return version_file.read_text().strip()
        # Fallback: check one level up (old location)
        alt_version_file = data_dir.parent / "VERSION"
        if alt_version_file.exists():
            return alt_version_file.read_text().strip()
        return "0.0.0"  # Fallback for pre-versioning installations

    # The kiosk reads settings.json from <base>/config where <base> is
    # /opt/magic_dingus_box in production (the path MEDIA_BROWSER_SETTINGS_PATH
    # hardcodes) — TWO levels up from data_dir, same as the VERSION file
    # above. Backup and restore used to derive this as data_dir.parent /
    # "config" (= magic_dingus_box_cpp/config), a directory the kiosk never
    # touches: settings were silently omitted from every backup ZIP, and a
    # restore wrote settings.json somewhere it would never be read while
    # reporting success.
    kiosk_config_dir = data_dir.parent.parent / "config"

    @app.get("/admin/backup")
    def create_backup():  # type: ignore[no-redef]
        """Create a backup of all playlists, settings, and device info.

        Returns a ZIP file containing:
        - playlists/*.yaml - All playlist files
        - config/settings.json - Device settings (if exists)
        - data/device_info.json - Device identity info (if exists)
        - manifest.json - Backup metadata
        """
        buffer = io.BytesIO()

        with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED) as zf:
            # Track what we're backing up
            manifest = {
                "version": get_app_version(),
                "created_at": datetime.now().isoformat(),
                "device_name": get_device_info().get("device_name", "Unknown"),
                "contents": {
                    "playlists": [],
                    "settings": False,
                    "device_info": False
                }
            }

            # Add playlists
            if playlists_dir.exists():
                for playlist_file in sorted(playlists_dir.glob("*.y*ml")):
                    arcname = f"playlists/{playlist_file.name}"
                    zf.write(playlist_file, arcname)
                    manifest["contents"]["playlists"].append(playlist_file.name)

            # Add settings file (from the kiosk's real config directory)
            settings_file = kiosk_config_dir / "settings.json"
            if settings_file.exists():
                zf.write(settings_file, "config/settings.json")
                manifest["contents"]["settings"] = True

            # Add device info
            if device_info_file.exists():
                zf.write(device_info_file, "data/device_info.json")
                manifest["contents"]["device_info"] = True

            # Add manifest
            zf.writestr("manifest.json", json.dumps(manifest, indent=2))

        buffer.seek(0)

        # Generate filename with timestamp
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        device_name = get_device_info().get("device_name", "magic_dingus_box")
        # Sanitize device name for filename
        safe_name = "".join(c if c.isalnum() or c in "-_" else "_" for c in device_name)
        filename = f"{safe_name}_backup_{timestamp}.zip"

        return send_file(
            buffer,
            mimetype='application/zip',
            as_attachment=True,
            download_name=filename
        )

    @app.post("/admin/restore")
    @require_csrf
    def restore_backup():  # type: ignore[no-redef]
        """Restore from a backup ZIP file.

        Accepts a ZIP file created by the backup endpoint.
        Restores playlists, settings, and device info.
        """
        if "file" not in request.files:
            return error_response("NO_FILE", "No backup file provided")

        file = request.files["file"]
        if not file.filename:
            return error_response("NO_FILE", "No backup file selected")

        # Check file extension
        if not file.filename.lower().endswith('.zip'):
            return error_response("INVALID_FORMAT", "Backup must be a ZIP file")

        restored = {
            "playlists": [],
            "settings": False,
            "device_info": False
        }
        errors = []

        try:
            with zipfile.ZipFile(file, 'r') as zf:
                # Validate ZIP contents
                names = zf.namelist()

                # Check for manifest (optional but helpful)
                manifest = None
                if "manifest.json" in names:
                    try:
                        manifest_data = _read_zip_entry_capped(zf, "manifest.json")
                        manifest = json.loads(manifest_data.decode('utf-8'))
                    except Exception as e:
                        errors.append(f"Could not read manifest: {e}")

                # Restore playlists
                for name in names:
                    if name.startswith("playlists/") and (name.endswith('.yaml') or name.endswith('.yml')):
                        playlist_name = os.path.basename(name)
                        if not playlist_name:
                            continue

                        # Sanitize filename
                        try:
                            safe_name = _canonical_playlist_name(
                                _sanitize_filename(playlist_name, allowed_extensions=['.yaml', '.yml']))
                        except ValueError:
                            errors.append(f"Invalid playlist filename: {playlist_name}")
                            continue

                        try:
                            content = _read_zip_entry_capped(zf, name)
                            # Validate it's valid YAML
                            restored_doc = yaml.safe_load(content.decode('utf-8'))
                            bad = _invalid_emulator_core(restored_doc)
                            if bad is not None:
                                errors.append(
                                    f"Skipped {playlist_name}: invalid emulator_core {bad}")
                                continue

                            dest = playlists_dir / safe_name
                            # Atomic + fsync'd: the kiosk reads these files
                            # and a torn playlist silently vanishes from the
                            # menu (loader treats parse failure as empty).
                            _atomic_write_text(dest, content.decode("utf-8"))
                            restored["playlists"].append(safe_name)
                        except Exception as e:
                            errors.append(f"Failed to restore {playlist_name}: {e}")

                # The restore path already poked for settings 20 lines below
                # but never for playlists, so a restored backup put the
                # playlists on disk and left the TV showing the old set.
                if restored["playlists"]:
                    _poke_kiosk_reload("playlists")

                # Restore settings
                if "config/settings.json" in names:
                    try:
                        content = _read_zip_entry_capped(zf, "config/settings.json")
                        # Valid JSON AND an object: the kiosk reads this file
                        # as a JSON object; a list or scalar is not settings.
                        if not isinstance(json.loads(content.decode('utf-8')), dict):
                            raise ValueError("settings.json must be a JSON object")

                        settings_dest = kiosk_config_dir / "settings.json"
                        # Atomic + fsync'd — a torn settings.json reads as
                        # CRT_NATIVE on the kiosk's peek and flips the
                        # display mode silently.
                        _atomic_write_text(settings_dest, content.decode("utf-8"))
                        restored["settings"] = True
                        # Poke the running kiosk to reload: it polls for
                        # this marker (~1s cadence) and re-reads
                        # settings.json into memory. Without the poke, the
                        # kiosk's next operator-action save would clobber
                        # the restore with its stale in-memory settings.
                        _poke_kiosk_reload("settings")
                    except Exception as e:
                        errors.append(f"Failed to restore settings: {e}")

                # Restore device info
                if "data/device_info.json" in names:
                    try:
                        content = _read_zip_entry_capped(zf, "data/device_info.json")
                        # Valid JSON is not enough: a list or a string here
                        # used to 500 /admin/device/info forever after.
                        problem = _device_info_problem(
                            json.loads(content.decode('utf-8')))
                        if problem:
                            raise ValueError(problem)

                        _atomic_write_text(device_info_file, content.decode("utf-8"))
                        restored["device_info"] = True
                    except Exception as e:
                        errors.append(f"Failed to restore device info: {e}")

        except zipfile.BadZipFile:
            return error_response("INVALID_FORMAT", "File is not a valid ZIP archive")
        except Exception as e:
            return error_response("INTERNAL_ERROR", f"Failed to process backup: {e}", status=500)

        # Build response message
        message_parts = []
        if restored["playlists"]:
            message_parts.append(f"{len(restored['playlists'])} playlist(s)")
        if restored["settings"]:
            message_parts.append("settings")
        if restored["device_info"]:
            message_parts.append("device info")

        message = "Restored: " + ", ".join(message_parts) if message_parts else "No items restored"

        result = {
            "restored": restored,
            "errors": errors if errors else None
        }

        if errors:
            result["warnings"] = errors
            return success_response(data=result, message=f"{message} (with {len(errors)} error(s))")

        return success_response(data=result, message=message)

    # Published for the route modules registered after this one
    ctx.get_app_version = get_app_version
    ctx.get_device_info = get_device_info
