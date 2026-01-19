from __future__ import annotations

import io
import json
import socket
import os
import re
import subprocess
import sys
import secrets
import threading
import time
import uuid
import zipfile
from datetime import datetime
from functools import wraps
from pathlib import Path
from typing import Any, Optional

import yaml
from flask import Flask, jsonify, request, send_file


# ===== SYSTEM MONITORING HELPERS =====

def get_cpu_temperature() -> Optional[float]:
    """Get CPU temperature (Raspberry Pi specific)."""
    try:
        # Try thermal zone (works on most Linux including Pi)
        temp_file = Path("/sys/class/thermal/thermal_zone0/temp")
        if temp_file.exists():
            return float(temp_file.read_text().strip()) / 1000.0
    except Exception:
        pass

    try:
        # Fallback: vcgencmd (Raspberry Pi specific)
        result = subprocess.run(
            ["vcgencmd", "measure_temp"],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0:
            # Output format: temp=45.0'C
            temp_str = result.stdout.strip()
            if "temp=" in temp_str:
                return float(temp_str.split("=")[1].replace("'C", ""))
    except Exception:
        pass

    return None


def get_memory_info() -> dict:
    """Get memory usage info."""
    try:
        with open("/proc/meminfo") as f:
            meminfo = {}
            for line in f:
                parts = line.split()
                if len(parts) >= 2:
                    key = parts[0].rstrip(":")
                    value = int(parts[1])  # in kB
                    meminfo[key] = value

            total = meminfo.get("MemTotal", 0)
            available = meminfo.get("MemAvailable", meminfo.get("MemFree", 0))
            used = total - available

            return {
                "total_mb": round(total / 1024, 1),
                "used_mb": round(used / 1024, 1),
                "available_mb": round(available / 1024, 1),
                "percent": round((used / total) * 100, 1) if total > 0 else 0
            }
    except Exception:
        return {}


def get_disk_info(path: str = "/") -> dict:
    """Get disk usage info for a path."""
    try:
        stat = os.statvfs(path)
        total = stat.f_blocks * stat.f_frsize
        free = stat.f_bavail * stat.f_frsize
        used = total - free

        return {
            "total_gb": round(total / (1024**3), 2),
            "used_gb": round(used / (1024**3), 2),
            "free_gb": round(free / (1024**3), 2),
            "percent": round((used / total) * 100, 1) if total > 0 else 0
        }
    except Exception:
        return {}


def get_cpu_usage() -> Optional[float]:
    """Get CPU usage percentage."""
    try:
        # Read /proc/stat twice with a small delay
        def read_cpu_stats():
            with open("/proc/stat") as f:
                line = f.readline()
                parts = line.split()
                # cpu user nice system idle iowait irq softirq
                if parts[0] == "cpu":
                    return [int(x) for x in parts[1:8]]
            return None

        stats1 = read_cpu_stats()
        if not stats1:
            return None

        time.sleep(0.1)  # Small delay
        stats2 = read_cpu_stats()
        if not stats2:
            return None

        # Calculate difference
        diff = [s2 - s1 for s1, s2 in zip(stats1, stats2)]
        total = sum(diff)
        idle = diff[3]  # idle is 4th value

        if total > 0:
            return round(((total - idle) / total) * 100, 1)
    except Exception:
        pass

    return None


def get_uptime() -> Optional[int]:
    """Get system uptime in seconds."""
    try:
        with open("/proc/uptime") as f:
            uptime_seconds = float(f.read().split()[0])
            return int(uptime_seconds)
    except Exception:
        return None


def check_service_status(service_name: str) -> str:
    """Check if a systemd service is running."""
    try:
        result = subprocess.run(
            ["systemctl", "is-active", service_name],
            capture_output=True, text=True, timeout=5
        )
        return result.stdout.strip()
    except Exception:
        return "unknown"


# ===== STANDARDIZED API RESPONSE HELPERS =====

def success_response(data: Any = None, message: str = None) -> tuple:
    """Create a standardized success response.

    Args:
        data: Optional data payload
        message: Optional success message

    Returns:
        Tuple of (response_dict, status_code)
    """
    response = {"ok": True}
    if data is not None:
        response["data"] = data
    if message:
        response["message"] = message
    return jsonify(response), 200


def error_response(code: str, message: str, status: int = 400, details: Any = None) -> tuple:
    """Create a standardized error response.

    Args:
        code: Error code (e.g., "NOT_FOUND", "VALIDATION_ERROR")
        message: Human-readable error message
        status: HTTP status code (default 400)
        details: Optional additional error details

    Returns:
        Tuple of (response_dict, status_code)
    """
    response = {
        "ok": False,
        "error": {
            "code": code,
            "message": message
        }
    }
    if details is not None:
        response["error"]["details"] = details
    return jsonify(response), status


# CSRF Token Storage (in-memory with expiration)
# In production, consider using Redis or session storage
_csrf_tokens: dict[str, float] = {}
_CSRF_TOKEN_EXPIRY = 3600  # 1 hour


def _cleanup_expired_tokens():
    """Remove expired CSRF tokens."""
    current_time = time.time()
    expired = [token for token, expiry in _csrf_tokens.items() if current_time > expiry]
    for token in expired:
        del _csrf_tokens[token]


def _generate_csrf_token() -> str:
    """Generate a new CSRF token."""
    _cleanup_expired_tokens()
    token = secrets.token_urlsafe(32)
    _csrf_tokens[token] = time.time() + _CSRF_TOKEN_EXPIRY
    return token


def _validate_csrf_token(token: str | None) -> bool:
    """Validate a CSRF token."""
    if not token:
        return False
    _cleanup_expired_tokens()
    return token in _csrf_tokens


def _sanitize_filename(name: str, allowed_extensions: Optional[list[str]] = None) -> str:
    """Sanitize filename to prevent path traversal attacks.
    
    Args:
        name: Original filename
        allowed_extensions: Optional list of allowed extensions (e.g., ['.yaml', '.yml'])
        
    Returns:
        Sanitized filename (basename only, no path separators)
        
    Raises:
        ValueError: If filename contains path separators or invalid characters
    """
    # Get basename to remove any path components
    basename = os.path.basename(name)
    
    # Reject if still contains path separators (shouldn't happen after basename, but be safe)
    if '/' in basename or '\\' in basename or '..' in basename:
        raise ValueError("Filename contains invalid path characters")
    
    # Validate extension if required
    if allowed_extensions:
        ext = os.path.splitext(basename)[1].lower()
        if ext not in allowed_extensions:
            raise ValueError(f"Filename must have one of these extensions: {', '.join(allowed_extensions)}")
    
    return basename


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


def format_playlist_yaml(data: dict) -> str:
    """Format playlist data as clean YAML matching the expected format.
    
    This ensures the YAML output matches the format that PlaylistLibrary expects,
    with consistent structure and blank fields where no data exists.
    """
    
    def yaml_quote(value: str) -> str:
        """Quote a YAML value if it contains special characters that would break parsing."""
        if not value:
            return "''"
        # Characters that need quoting: # (comment), : (key separator), leading/trailing spaces
        # Also quote if contains newlines, tabs, or other control characters
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
    lines.append(f"curator: {yaml_quote(data.get('curator', 'Unknown'))}")
    
    # Always include description field (blank if empty, for consistency)
    description = data.get('description', '')
    lines.append(f"description: {yaml_quote(description)}")
    
    # Playlist type (video or game)
    lines.append(f"playlist_type: {data.get('playlist_type', 'video')}")
    
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
        
        lines.append(f"    source_type: {item.get('source_type', 'local')}")
        
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
            # Format tags as YAML list
            lines.append("    tags:")
            for tag in item['tags']:
                lines.append(f"      - {yaml_quote(tag)}")
        
        # Emulator fields for games
        if item.get('emulator_core'):
            lines.append(f"    emulator_core: {item['emulator_core']}")
        
        if item.get('emulator_system'):
            lines.append(f"    emulator_system: {item['emulator_system']}")
        
        # Add blank line between items for readability
        if item != items[-1]:  # Not the last item
            lines.append("")
    
    return '\n'.join(lines) + '\n'


def create_app(data_dir: Path, config=None) -> Flask:
    app = Flask(__name__)
    
    # Limit upload sizes; default 2GB (can override via MAGIC_MAX_UPLOAD_MB)
    try:
        max_mb = int(os.getenv("MAGIC_MAX_UPLOAD_MB", "2048"))
        app.config["MAX_CONTENT_LENGTH"] = max_mb * 1024 * 1024
    except Exception:
        app.config["MAX_CONTENT_LENGTH"] = 2 * 1024 * 1024 * 1024
    
    # Use a temp directory on the SD card instead of /tmp (which is limited tmpfs)
    # This prevents "No space left on device" errors for large file uploads
    upload_temp_dir = data_dir / "upload_temp"
    upload_temp_dir.mkdir(parents=True, exist_ok=True)
    os.environ["TMPDIR"] = str(upload_temp_dir)
    import tempfile
    tempfile.tempdir = str(upload_temp_dir)

    
    # Optional simple token auth for admin APIs (disabled by default)
    _admin_token = os.getenv("MAGIC_ADMIN_TOKEN")
    if _admin_token:
        @app.before_request
        def _require_token():  # type: ignore[no-redef]
            # Allow static assets without token
            if request.path.startswith("/static/"):
                return None
            if request.headers.get("X-Magic-Token") != _admin_token:
                return {"error": "unauthorized"}, 401

    # CSRF protection decorator for state-changing operations
    def require_csrf(f):
        """Decorator to require valid CSRF token for state-changing requests."""
        @wraps(f)
        def decorated_function(*args, **kwargs):
            # Skip CSRF check if CSRF is disabled (for development/testing)
            if os.getenv("MAGIC_DISABLE_CSRF"):
                return f(*args, **kwargs)

            token = request.headers.get("X-CSRF-Token")
            if not _validate_csrf_token(token):
                return error_response("CSRF_ERROR", "Invalid or missing CSRF token", status=403)
            return f(*args, **kwargs)
        return decorated_function

    # ===== CSRF TOKEN ENDPOINT =====

    @app.get("/admin/csrf-token")
    def get_csrf_token():  # type: ignore[no-redef]
        """Get a new CSRF token for state-changing requests."""
        token = _generate_csrf_token()
        return success_response(data={"token": token})

    playlists_dir = data_dir / "playlists"
    media_dir = data_dir / "media"
    roms_dir = data_dir / "roms"
    device_info_file = data_dir / "device_info.json"

    def get_device_info() -> dict:
        """Get device identity and stats."""
        try:
            if device_info_file.exists():
                info = json.loads(device_info_file.read_text())
            else:
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
        data = request.get_json()
        if not data:
            return error_response("VALIDATION_ERROR", "JSON body required")
        new_name = data.get('name', 'Magic Dingus Box')

        try:
            if device_info_file.exists():
                info = json.loads(device_info_file.read_text())
            else:
                import uuid
                info = {'device_id': str(uuid.uuid4())}

            info['device_name'] = new_name
            device_info_file.write_text(json.dumps(info, indent=2))
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

            # Add settings file (from config directory)
            config_dir = data_dir.parent / "config"
            settings_file = config_dir / "settings.json"
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
                        manifest_data = zf.read("manifest.json")
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
                            safe_name = _sanitize_filename(playlist_name, allowed_extensions=['.yaml', '.yml'])
                        except ValueError:
                            errors.append(f"Invalid playlist filename: {playlist_name}")
                            continue

                        try:
                            content = zf.read(name)
                            # Validate it's valid YAML
                            yaml.safe_load(content.decode('utf-8'))

                            dest = playlists_dir / safe_name
                            dest.parent.mkdir(parents=True, exist_ok=True)
                            dest.write_bytes(content)
                            restored["playlists"].append(safe_name)
                        except Exception as e:
                            errors.append(f"Failed to restore {playlist_name}: {e}")

                # Restore settings
                if "config/settings.json" in names:
                    try:
                        content = zf.read("config/settings.json")
                        # Validate it's valid JSON
                        json.loads(content.decode('utf-8'))

                        config_dir = data_dir.parent / "config"
                        config_dir.mkdir(parents=True, exist_ok=True)
                        settings_dest = config_dir / "settings.json"
                        settings_dest.write_bytes(content)
                        restored["settings"] = True
                    except Exception as e:
                        errors.append(f"Failed to restore settings: {e}")

                # Restore device info
                if "data/device_info.json" in names:
                    try:
                        content = zf.read("data/device_info.json")
                        # Validate it's valid JSON
                        json.loads(content.decode('utf-8'))

                        data_dir.mkdir(parents=True, exist_ok=True)
                        device_info_file.write_bytes(content)
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
                    'playlist_type': data.get('playlist_type', 'video'),
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
        p = playlists_dir / name
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
            safe_name = _sanitize_filename(name, allowed_extensions=['.yaml', '.yml'])

            # Accept JSON or YAML
            if request.is_json:
                data = request.get_json()
                if not data:
                    return error_response("VALIDATION_ERROR", "Invalid JSON body")
                # Convert to clean YAML matching the expected format
                yaml_content = format_playlist_yaml(data)
            else:
                yaml_content = request.get_data(as_text=True)
                # Validate it's valid YAML
                if not yaml_content.strip():
                    return error_response("VALIDATION_ERROR", "Empty content")
                yaml.safe_load(yaml_content)

            if not yaml_content.strip():
                return error_response("VALIDATION_ERROR", "Generated YAML is empty")

            p = playlists_dir / safe_name
            # Safer check for path traversal:
            if '..' in safe_name or '/' in safe_name or '\\' in safe_name:
                return error_response("VALIDATION_ERROR", "Invalid filename")

            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(yaml_content)
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

    @app.delete("/admin/playlists/<name>")
    @require_csrf
    def delete_playlist(name):  # type: ignore[no-redef]
        """Delete a playlist."""
        try:
            safe_name = _sanitize_filename(name, allowed_extensions=['.yaml', '.yml'])
        except ValueError as e:
            return error_response("VALIDATION_ERROR", str(e))

        p = playlists_dir / safe_name
        # Ensure path stays within playlists_dir
        p_resolved = p.resolve()
        playlists_dir_resolved = playlists_dir.resolve()
        if not str(p_resolved).startswith(str(playlists_dir_resolved)):
            return error_response("VALIDATION_ERROR", "Invalid path")

        if p.exists():
            p.unlink()
            return success_response(message="Playlist deleted")
        return error_response("NOT_FOUND", f"Playlist '{name}' not found", status=404)

    # ===== MEDIA MANAGEMENT =====

    @app.get("/admin/media")
    def list_media():  # type: ignore[no-redef]
        """List all media files from both uploaded and dev directories."""
        files = []

        # Scan the main media directory (uploaded videos)
        if media_dir.exists():
            for ext in ['*.mp4', '*.mkv', '*.avi', '*.mov', '*.webm']:
                files.extend(media_dir.glob(f"**/{ext}"))

        # Also scan dev_data/media (existing videos)
        dev_media_dir = data_dir.parent / "dev_data" / "media"
        if dev_media_dir.exists():
            for ext in ['*.mp4', '*.mkv', '*.avi', '*.mov', '*.webm']:
                files.extend(dev_media_dir.glob(f"**/{ext}"))

        media_list = [{
            'filename': f.name,
            'path': str(f.relative_to(data_dir.parent)),  # Relative to parent of data dir
            'size': f.stat().st_size,
            'modified': f.stat().st_mtime
        } for f in sorted(files)]

        return success_response(data=media_list)

    @app.post("/admin/upload")
    @require_csrf
    def upload_media():  # type: ignore[no-redef]
        """Upload video file."""
        if "file" not in request.files:
            return error_response("VALIDATION_ERROR", "File field required")
        f = request.files["file"]

        # Sanitize filename to prevent path traversal
        try:
            safe_filename = _sanitize_filename(f.filename)
        except ValueError as e:
            return error_response("VALIDATION_ERROR", str(e))

        out = media_dir / safe_filename
        # Ensure path stays within media_dir
        out_resolved = out.resolve()
        media_dir_resolved = media_dir.resolve()
        if not str(out_resolved).startswith(str(media_dir_resolved)):
            return error_response("VALIDATION_ERROR", "Invalid path")

        out.parent.mkdir(parents=True, exist_ok=True)
        f.save(str(out))
        return success_response(data={"path": str(out.relative_to(data_dir.parent))}, message="File uploaded")

    @app.delete("/admin/media/<path:filepath>")
    @require_csrf
    def delete_media(filepath):  # type: ignore[no-redef]
        """Delete a media file from either uploaded or dev directories."""
        # Try to find the file in either location
        # filepath is relative to /opt/magic_dingus_box (parent of data_dir)
        target = data_dir.parent / filepath

        # Security check: ensure path is within allowed directories
        target_resolved = target.resolve()
        data_dir_resolved = data_dir.parent.resolve()

        # Check if path is within the parent directory
        if not str(target_resolved).startswith(str(data_dir_resolved)):
            return error_response("VALIDATION_ERROR", "Invalid path")

        if target.exists() and target.is_file():
            target.unlink()
            return success_response(message="File deleted")
        return error_response("NOT_FOUND", "File not found", status=404)

    # ===== VIDEO TRANSCODING =====

    # Resolution presets for transcoding
    TRANSCODE_RESOLUTIONS = {
        'crt': {'width': 640, 'height': 480},
        'modern': {'width': 1280, 'height': 720},
    }

    # Store for tracking transcoding jobs (in-memory, cleared on restart)
    transcode_jobs: dict = {}

    def run_transcode_job(job_id: str, input_path: Path, output_path: Path, resolution: str, normalize_audio: bool = False):
        """Background thread function to run FFmpeg transcoding."""
        job = transcode_jobs[job_id]
        res = TRANSCODE_RESOLUTIONS.get(resolution, TRANSCODE_RESOLUTIONS['crt'])
        width, height = res['width'], res['height']

        # Build FFmpeg command with center crop (no black bars, no distortion)
        ffmpeg_cmd = [
            'ffmpeg', '-y',
            '-i', str(input_path),
            '-vf', f'scale={width}:{height}:force_original_aspect_ratio=increase,crop={width}:{height}',
        ]
        
        # Only apply audio normalization if requested (default: OFF)
        # This allows pre-normalized videos to skip the loudnorm filter
        if normalize_audio:
            ffmpeg_cmd.extend(['-af', 'loudnorm=I=-23:LRA=7:tp=-2'])  # EBU R128 normalization
        
        ffmpeg_cmd.extend([
            '-c:v', 'libx264',
            '-preset', 'ultrafast',
            '-crf', '28',
            '-c:a', 'aac',
            '-b:a', '128k',
            '-ar', '48000',
            '-movflags', '+faststart',
            '-progress', 'pipe:1',
            '-nostats',
            str(output_path)
        ])

        try:
            job['status'] = 'transcoding'
            job['message'] = 'Starting FFmpeg...'

            # Get video duration for progress calculation
            probe_cmd = ['ffprobe', '-v', 'error', '-show_entries', 'format=duration',
                        '-of', 'default=noprint_wrappers=1:nokey=1', str(input_path)]
            try:
                duration_result = subprocess.run(probe_cmd, capture_output=True, text=True, timeout=30)
                duration = float(duration_result.stdout.strip())
            except Exception:
                duration = 0

            # Run FFmpeg
            process = subprocess.Popen(
                ffmpeg_cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )

            # Parse progress from stdout
            current_time = 0
            while True:
                line = process.stdout.readline()
                if not line and process.poll() is not None:
                    break

                # Parse out_time_ms from progress output
                if line.startswith('out_time_ms='):
                    try:
                        time_ms = int(line.split('=')[1].strip())
                        current_time = time_ms / 1000000  # Convert to seconds
                        if duration > 0:
                            job['progress'] = min(99, int((current_time / duration) * 100))
                            job['message'] = f'Transcoding: {job["progress"]}%'
                    except (ValueError, IndexError):
                        pass

            # Check result
            if process.returncode == 0:
                job['status'] = 'complete'
                job['progress'] = 100
                job['message'] = 'Transcoding complete!'
                job['output_path'] = str(output_path.relative_to(data_dir.parent))

                # Clean up input temp file
                try:
                    input_path.unlink()
                except Exception:
                    pass
            else:
                stderr_output = process.stderr.read()
                job['status'] = 'error'
                job['message'] = f'FFmpeg failed: {stderr_output[:200]}'

                # Clean up on error
                try:
                    input_path.unlink()
                except Exception:
                    pass
                try:
                    output_path.unlink()
                except Exception:
                    pass

        except Exception as e:
            job['status'] = 'error'
            job['message'] = str(e)

    @app.post("/admin/upload-and-transcode")
    @require_csrf
    def upload_and_transcode():  # type: ignore[no-redef]
        """Upload video file and transcode it on the Pi."""
        if "file" not in request.files:
            return error_response("VALIDATION_ERROR", "File field required")

        f = request.files["file"]
        resolution = request.form.get("resolution", "crt")

        if resolution not in TRANSCODE_RESOLUTIONS:
            return error_response("VALIDATION_ERROR", f"Invalid resolution: {resolution}")

        # Create unique job ID
        job_id = str(uuid.uuid4())

        # Sanitize filename
        try:
            original_name = _sanitize_filename(f.filename)
        except ValueError as e:
            return error_response("VALIDATION_ERROR", str(e))

        # Save to temp location
        temp_input = upload_temp_dir / f"transcode_input_{job_id}_{original_name}"
        output_name = Path(original_name).stem + ".mp4"
        output_path = media_dir / output_name

        # Ensure unique output filename
        counter = 1
        while output_path.exists():
            output_name = f"{Path(original_name).stem}_{counter}.mp4"
            output_path = media_dir / output_name
            counter += 1

        # Save uploaded file
        f.save(str(temp_input))

        # Initialize job
        transcode_jobs[job_id] = {
            'status': 'pending',
            'progress': 0,
            'message': 'Upload complete, starting transcode...',
            'output_path': None,
            'output_filename': output_name,
        }

        # Start transcoding in background thread
        thread = threading.Thread(
            target=run_transcode_job,
            args=(job_id, temp_input, output_path, resolution)
        )
        thread.daemon = True
        thread.start()

        return success_response(data={'job_id': job_id}, message="Transcoding started")

    @app.get("/admin/transcode-status/<job_id>")
    def transcode_status(job_id):  # type: ignore[no-redef]
        """Get status of a transcoding job."""
        if job_id not in transcode_jobs:
            return error_response("NOT_FOUND", "Job not found", status=404)

        job = transcode_jobs[job_id]
        return success_response(data={
            'status': job['status'],
            'progress': job['progress'],
            'message': job['message'],
            'output_path': job.get('output_path'),
            'output_filename': job.get('output_filename'),
        })

    def probe_video(file_path: Path, target_resolution: str) -> dict:
        """Probe video file to check if it needs transcoding."""
        target = TRANSCODE_RESOLUTIONS.get(target_resolution, TRANSCODE_RESOLUTIONS['crt'])
        target_w, target_h = target['width'], target['height']

        try:
            # Run ffprobe to get video info
            probe_cmd = [
                'ffprobe', '-v', 'error',
                '-select_streams', 'v:0',
                '-show_entries', 'stream=width,height,codec_name',
                '-show_entries', 'format=format_name',
                '-of', 'json',
                str(file_path)
            ]
            result = subprocess.run(probe_cmd, capture_output=True, text=True, timeout=30)

            if result.returncode != 0:
                return {'needs_transcode': True, 'reason': 'Could not probe video'}

            import json as json_module
            data = json_module.loads(result.stdout)

            # Extract video stream info
            streams = data.get('streams', [])
            if not streams:
                return {'needs_transcode': True, 'reason': 'No video stream found'}

            stream = streams[0]
            width = stream.get('width', 0)
            height = stream.get('height', 0)
            codec = stream.get('codec_name', '')

            # Extract container format
            format_name = data.get('format', {}).get('format_name', '')
            is_mp4 = 'mp4' in format_name or 'mov' in format_name

            # Check if video is already compatible
            is_correct_resolution = (width == target_w and height == target_h)
            is_h264 = codec in ('h264', 'libx264')

            if is_correct_resolution and is_h264 and is_mp4:
                return {
                    'needs_transcode': False,
                    'width': width,
                    'height': height,
                    'codec': codec,
                    'reason': 'Already compatible'
                }
            else:
                reasons = []
                if not is_correct_resolution:
                    reasons.append(f'Resolution {width}x{height} != {target_w}x{target_h}')
                if not is_h264:
                    reasons.append(f'Codec {codec} is not H.264')
                if not is_mp4:
                    reasons.append('Not MP4 container')

                return {
                    'needs_transcode': True,
                    'width': width,
                    'height': height,
                    'codec': codec,
                    'reason': '; '.join(reasons)
                }

        except Exception as e:
            return {'needs_transcode': True, 'reason': f'Probe error: {str(e)}'}

    @app.post("/admin/smart-upload")
    @require_csrf
    def smart_upload():  # type: ignore[no-redef]
        """Smart upload: probe video and decide whether to transcode or direct upload."""
        if "file" not in request.files:
            return error_response("VALIDATION_ERROR", "File field required")

        f = request.files["file"]
        resolution = request.form.get("resolution", "crt")

        if resolution not in TRANSCODE_RESOLUTIONS:
            return error_response("VALIDATION_ERROR", f"Invalid resolution: {resolution}")

        # Sanitize filename
        try:
            original_name = _sanitize_filename(f.filename)
        except ValueError as e:
            return error_response("VALIDATION_ERROR", str(e))

        # Save to temp location for probing
        job_id = str(uuid.uuid4())
        temp_input = upload_temp_dir / f"probe_{job_id}_{original_name}"
        f.save(str(temp_input))

        # Probe the video
        probe_result = probe_video(temp_input, resolution)

        if not probe_result['needs_transcode']:
            # Already compatible - move directly to media folder
            output_name = Path(original_name).stem + ".mp4"
            # If original is already .mp4, keep it
            if original_name.lower().endswith('.mp4'):
                output_name = original_name
            output_path = media_dir / output_name

            # Ensure unique filename
            counter = 1
            base_name = Path(output_name).stem
            while output_path.exists():
                output_name = f"{base_name}_{counter}.mp4"
                output_path = media_dir / output_name
                counter += 1

            # Move file to media folder
            import shutil
            shutil.move(str(temp_input), str(output_path))

            return success_response(data={
                'action': 'direct',
                'needs_transcode': False,
                'output_path': str(output_path.relative_to(data_dir.parent)),
                'output_filename': output_name,
                'probe': probe_result
            }, message="File uploaded directly (already compatible)")

        else:
            # Needs transcoding - start transcode job
            output_name = Path(original_name).stem + ".mp4"
            output_path = media_dir / output_name

            # Ensure unique output filename
            counter = 1
            while output_path.exists():
                output_name = f"{Path(original_name).stem}_{counter}.mp4"
                output_path = media_dir / output_name
                counter += 1

            # Initialize job
            transcode_jobs[job_id] = {
                'status': 'pending',
                'progress': 0,
                'message': 'Starting transcode...',
                'output_path': None,
                'output_filename': output_name,
            }

            # Start transcoding in background thread
            thread = threading.Thread(
                target=run_transcode_job,
                args=(job_id, temp_input, output_path, resolution)
            )
            thread.daemon = True
            thread.start()

            return success_response(data={
                'action': 'transcode',
                'needs_transcode': True,
                'job_id': job_id,
                'probe': probe_result
            }, message="Transcoding started")

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
        if not str(out_resolved).startswith(str(roms_dir_resolved)):
            return error_response("VALIDATION_ERROR", "Invalid path")

        out.parent.mkdir(parents=True, exist_ok=True)
        f.save(str(out))
        
        # Auto-generate M3U playlists for PS1 multi-disc games
        if safe_system.lower() == 'ps1':
            def run_m3u_generator():
                # Acquire lock to ensure only one script instance runs at a time
                with m3u_lock:
                    try:
                        script_path = data_dir.parent / "magic_dingus_box_cpp" / "scripts" / "generate_m3u_playlists.sh"
                        if script_path.exists():
                            subprocess.run(
                                [str(script_path)],
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

        # Security check: ensure path is within allowed directories
        target_resolved = target.resolve()
        data_dir_resolved = data_dir.parent.resolve()

        # Check if path is within the parent directory
        if not str(target_resolved).startswith(str(data_dir_resolved)):
            return error_response("VALIDATION_ERROR", "Invalid path")

        # Extra check: must be in roms dir (either data/roms or dev_data/roms)
        is_data_rom = str(target_resolved).startswith(str(roms_dir.resolve()))
        dev_roms_dir = data_dir.parent / "dev_data" / "roms"
        is_dev_rom = dev_roms_dir.exists() and str(target_resolved).startswith(str(dev_roms_dir.resolve()))

        if not (is_data_rom or is_dev_rom):
            return error_response("VALIDATION_ERROR", "File is not a ROM")

        if target.exists() and target.is_file():
            target.unlink()
            return success_response(message="ROM deleted")
        return error_response("NOT_FOUND", "ROM not found", status=404)

    # ===== OTA UPDATE MANAGEMENT =====

    # Path to update script
    # data_dir is /opt/magic_dingus_box/magic_dingus_box_cpp/data
    # update.sh is at /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/update.sh
    UPDATE_SCRIPT = data_dir.parent / "scripts" / "update.sh"

    # Store for tracking update jobs (in-memory, cleared on restart)
    update_jobs: dict = {}

    @app.get("/admin/update/version")
    def get_version():  # type: ignore[no-redef]
        """Get current installed version."""
        return success_response(data={
            "version": get_app_version(),
            "device_name": get_device_info().get("device_name", "Unknown")
        })

    @app.get("/admin/update/check")
    def check_for_update():  # type: ignore[no-redef]
        """Check if an update is available from GitHub."""
        if not UPDATE_SCRIPT.exists():
            return error_response(
                "UPDATE_NOT_AVAILABLE",
                "Update script not found. Run deploy with --build first.",
                status=500
            )

        try:
            result = subprocess.run(
                [str(UPDATE_SCRIPT), "check"],
                capture_output=True,
                text=True,
                timeout=60
            )

            if result.returncode == 0:
                # Parse JSON output from update script
                response_data = json.loads(result.stdout)
                return jsonify(response_data), 200
            else:
                error_msg = result.stderr.strip() or result.stdout.strip() or "Unknown error"
                return error_response("UPDATE_CHECK_FAILED", error_msg, status=500)

        except subprocess.TimeoutExpired:
            return error_response("TIMEOUT", "Update check timed out", status=504)
        except json.JSONDecodeError as e:
            return error_response("PARSE_ERROR", f"Failed to parse update info: {e}", status=500)
        except Exception as e:
            return error_response("INTERNAL_ERROR", str(e), status=500)

    def run_update_job(job_id: str, version: str, download_url: str):
        """Background thread function to run the update installation."""
        job = update_jobs[job_id]

        try:
            # Run update script with install command
            # Use start_new_session=True so the process survives when web service stops
            process = subprocess.Popen(
                [str(UPDATE_SCRIPT), "install", version, download_url],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                bufsize=1,  # Line buffered
                start_new_session=True  # Detach from parent process group
            )

            # Read progress output line by line
            for line in process.stdout:
                line = line.strip()
                if line:
                    try:
                        progress_data = json.loads(line)
                        job['stage'] = progress_data.get('stage', job['stage'])
                        job['progress'] = progress_data.get('progress', job['progress'])
                        job['message'] = progress_data.get('message', job['message'])

                        if progress_data.get('stage') == 'complete':
                            job['status'] = 'complete'
                            job['new_version'] = progress_data.get('new_version', version)
                        elif not progress_data.get('ok', True):
                            job['status'] = 'error'
                            job['message'] = progress_data.get('error', {}).get('message', 'Unknown error')
                    except json.JSONDecodeError:
                        # Non-JSON output, ignore
                        pass

            process.wait()

            if process.returncode != 0 and job['status'] != 'complete':
                stderr = process.stderr.read()
                job['status'] = 'error'
                job['message'] = stderr[:500] if stderr else 'Update failed'

        except Exception as e:
            job['status'] = 'error'
            job['message'] = str(e)

    @app.post("/admin/update/install")
    @require_csrf
    def install_update():  # type: ignore[no-redef]
        """Start an update installation."""
        if not UPDATE_SCRIPT.exists():
            return error_response(
                "UPDATE_NOT_AVAILABLE",
                "Update script not found",
                status=500
            )

        data = request.get_json()
        if not data:
            return error_response("VALIDATION_ERROR", "JSON body required")

        version = data.get('version')
        download_url = data.get('download_url')

        if not version or not download_url:
            return error_response("VALIDATION_ERROR", "version and download_url required")

        # Validate download URL (must be from GitHub)
        if not download_url.startswith("https://github.com/") and not download_url.startswith("https://api.github.com/"):
            return error_response("VALIDATION_ERROR", "Invalid download URL (must be from GitHub)")

        # Create job
        job_id = str(uuid.uuid4())
        update_jobs[job_id] = {
            'status': 'running',
            'stage': 'preparing',
            'progress': 0,
            'message': 'Starting update...',
            'version': version,
            'new_version': None
        }

        # Start update in background thread
        thread = threading.Thread(
            target=run_update_job,
            args=(job_id, version, download_url)
        )
        thread.daemon = True
        thread.start()

        return success_response(data={'job_id': job_id}, message="Update started")

    @app.get("/admin/update/status/<job_id>")
    def update_status(job_id):  # type: ignore[no-redef]
        """Get status of an update job."""
        if job_id not in update_jobs:
            return error_response("NOT_FOUND", "Job not found", status=404)

        job = update_jobs[job_id]
        return success_response(data={
            'status': job['status'],
            'stage': job['stage'],
            'progress': job['progress'],
            'message': job['message'],
            'new_version': job.get('new_version')
        })

    @app.post("/admin/update/rollback")
    @require_csrf
    def rollback_update():  # type: ignore[no-redef]
        """Rollback to the previous version."""
        if not UPDATE_SCRIPT.exists():
            return error_response(
                "UPDATE_NOT_AVAILABLE",
                "Update script not found",
                status=500
            )

        try:
            result = subprocess.run(
                [str(UPDATE_SCRIPT), "rollback"],
                capture_output=True,
                text=True,
                timeout=180  # 3 minute timeout for rollback
            )

            if result.returncode == 0:
                # Parse the last JSON line of output (completion message)
                lines = [l for l in result.stdout.strip().split('\n') if l.strip()]
                if lines:
                    try:
                        response_data = json.loads(lines[-1])
                        return jsonify(response_data), 200
                    except json.JSONDecodeError:
                        pass
                return success_response(message="Rollback completed")
            else:
                error_msg = result.stderr.strip() or "Rollback failed"
                return error_response("ROLLBACK_FAILED", error_msg, status=500)

        except subprocess.TimeoutExpired:
            return error_response("TIMEOUT", "Rollback timed out", status=504)
        except Exception as e:
            return error_response("INTERNAL_ERROR", str(e), status=500)

    # ===== SERVE WEB INTERFACE =====

    @app.get("/")
    @app.get("/admin")
    def admin_interface():  # type: ignore[no-redef]
        """Serve the web interface."""
        static_dir = Path(__file__).parent / "static"
        return send_file(static_dir / "index.html")

    @app.route("/static/<path:filename>")
    def serve_static(filename):  # type: ignore[no-redef]
        """Serve static assets."""
        static_dir = Path(__file__).parent / "static"
        return send_file(static_dir / filename)

    return app

