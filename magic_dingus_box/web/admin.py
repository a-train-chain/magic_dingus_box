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
import shutil
import tempfile
from datetime import datetime
from functools import wraps
from pathlib import Path
from typing import Any, Optional

import yaml
from flask import Flask, jsonify, redirect, render_template_string, request, send_file, send_from_directory

try:
    from remote import auth as remote_auth
    from remote import devices as remote_devices
    from remote import ws_handler
    from remote.uinput_writer import UinputWriter
except ImportError:
    from .remote import auth as remote_auth
    from .remote import devices as remote_devices
    from .remote import ws_handler
    from .remote.uinput_writer import UinputWriter


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


# ===== MEDIA BROWSER VISIBILITY GATE =====
#
# The kiosk has a "secret sequence" (BTN1+BTN3 chord → BTN2 × 3 → rotary click)
# that flips media_browser_unlocked = true in settings.json. The Content
# Manager's Media Browser tab — and every /admin/media-browser/* endpoint —
# stays hidden / 403 until that flag is true. Default state on a fresh Pi is
# locked, so ordinary users never see the feature exists.

MEDIA_BROWSER_SETTINGS_PATH = "/opt/magic_dingus_box/config/settings.json"


def _media_browser_unlocked() -> bool:
    """Return True iff the kiosk's persisted media_browser_unlocked flag is set.

    The flag lives at playback.media_browser_unlocked in
    /opt/magic_dingus_box/config/settings.json. Any error (file missing,
    malformed JSON, key missing) falls through to False — failure-closed by
    design so a corrupted settings file can't accidentally expose the feature.
    """
    try:
        with open(MEDIA_BROWSER_SETTINGS_PATH) as f:
            settings = json.load(f)
        return bool(settings.get("playback", {}).get("media_browser_unlocked", False))
    except Exception:
        return False


def _media_browser_locked_response():
    """Standard 403 response used by every guarded /admin/media-browser/* route."""
    return error_response(
        "media_browser_locked",
        "Media Browser is currently locked",
        status=403,
    )


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
    """Validate a CSRF token.

    Tokens expire after 1 hour but are NOT single-use — the frontend
    fetches one token at app load and reuses it across all state-changing
    requests for the session. Per-request rotation would require frontend
    work to refetch before each request; for the LAN-only single-operator
    kiosk threat model the expiry-based scheme is adequate.
    """
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
            # Format tags as YAML list - filter out invalid tags
            valid_tags = [t for t in item['tags'] if isinstance(t, str) and t.strip()]
            if valid_tags:
                lines.append("    tags:")
                for tag in valid_tags:
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


NICKNAME_PROMPT_HTML = """
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#1F191F">
<title>Name your remote</title>
<style>
  * { box-sizing: border-box; }
  html, body {
    margin: 0; padding: 0; min-height: 100vh;
    background: #1F191F; color: #F2E4D9;
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif;
    display: flex; align-items: center; justify-content: center;
  }
  .card {
    width: min(360px, 90%); padding: 32px 24px;
    background: #2A232A; border-radius: 16px;
    text-align: center;
  }
  h1 { margin: 0 0 8px; font-size: 22px; font-weight: 600; }
  p.sub { margin: 0 0 24px; font-size: 13px; color: #968B85; }
  input {
    width: 100%; padding: 14px 12px;
    background: #1F191F; color: #F2E4D9;
    border: 1px solid #968B85; border-radius: 10px;
    font-size: 16px; text-align: center; margin-bottom: 16px;
  }
  button {
    width: 100%; padding: 14px;
    background: #F5BF42; color: #1F191F;
    border: none; border-radius: 10px;
    font-size: 16px; font-weight: 700;
    cursor: pointer;
  }
  button:active { filter: brightness(0.9); }
</style>
</head>
<body>
<form class="card" method="post">
  <h1>&#10003; Paired</h1>
  <p class="sub">What should we call this remote?</p>
  <input name="nickname" placeholder="{{ placeholder }}" autofocus
         autocomplete="off" autocapitalize="words" maxlength="40">
  <button type="submit">Continue</button>
</form>
</body></html>
"""


def create_app(data_dir: Path, config=None) -> Flask:
    app = Flask(__name__)

    # flask-sock for the Phone Remote WebSocket.
    try:
        from flask_sock import Sock
    except ImportError:
        Sock = None
    sock = Sock(app) if Sock is not None else None

    # Expose data_dir to blueprints and request handlers (e.g. remote auth).
    app.config["DATA_DIR"] = str(data_dir)

    # Phone Remote — persistent HMAC secret for cookie signing.
    # Stored in the data dir so it survives restarts (otherwise paired phones
    # would be invalidated on every service restart). Falls back to env var
    # FLASK_SECRET_KEY if the operator wants to manage it externally.
    secret_path = Path(app.config["DATA_DIR"]) / "flask_secret.key"
    if os.environ.get("FLASK_SECRET_KEY"):
        app.config["SECRET_KEY"] = os.environ["FLASK_SECRET_KEY"]
    elif secret_path.exists():
        app.config["SECRET_KEY"] = secret_path.read_text().strip()
    else:
        new_secret = secrets.token_hex(32)
        secret_path.write_text(new_secret)
        secret_path.chmod(0o600)  # readable only by the magic user
        app.config["SECRET_KEY"] = new_secret

    # Phone Remote — status broadcaster (kiosk_status.json → WS push).
    try:
        from remote.status_broadcaster import StatusBroadcaster
    except ImportError:
        from .remote.status_broadcaster import StatusBroadcaster
    status_path = Path(app.config["DATA_DIR"]) / "kiosk_status.json"
    app.config["STATUS_BROADCASTER"] = StatusBroadcaster(
        status_path, queues=[], interval_s=0.2)
    app.config["STATUS_BROADCASTER"].start()

    # Limit upload sizes; default 8GB (can override via MAGIC_MAX_UPLOAD_MB)
    max_mb = int(os.getenv("MAGIC_MAX_UPLOAD_MB", "8192"))
    app.config["MAX_CONTENT_LENGTH"] = max_mb * 1024 * 1024
    
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
        try:
            safe_name = _sanitize_filename(name, allowed_extensions=['.yaml', '.yml'])
        except ValueError as e:
            return error_response("VALIDATION_ERROR", str(e))

        p = playlists_dir / safe_name
        # Containment check matching the DELETE handler — defends against any
        # path-traversal that survived _sanitize_filename (symlinks, etc.).
        p_resolved = p.resolve()
        playlists_dir_resolved = playlists_dir.resolve()
        if not str(p_resolved).startswith(str(playlists_dir_resolved)):
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
        if not str(p_resolved).startswith(str(playlists_dir_resolved)):
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

                    # Security check: ensure we're deleting within allowed directories
                    for candidate in [video_path, dev_video_path]:
                        if candidate.exists() and candidate.is_file():
                            candidate_resolved = candidate.resolve()
                            # Verify path is within data directories
                            data_parent = data_dir.parent.resolve()
                            if str(candidate_resolved).startswith(str(data_parent)):
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

        if videos_deleted > 0:
            return success_response(
                data={"videos_deleted": videos_deleted},
                message=f"Playlist deleted along with {videos_deleted} video(s)"
            )
        return success_response(message="Playlist deleted")

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
            
            # Determine output filename
            # Prefer 'title' from YAML, fall back to uploaded filename
            title = data.get('title', '').strip()
            if title:
                # Sanitize title for use as filename
                safe_title = re.sub(r'[^\w\s-]', '', title).strip()
                safe_title = re.sub(r'[-\s]+', '_', safe_title)
                output_name = f"{safe_title}.yaml"
            else:
                output_name = original_filename
            
            try:
                safe_name = _sanitize_filename(output_name, allowed_extensions=['.yaml', '.yml'])
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
            playlists_dir.mkdir(parents=True, exist_ok=True)
            output_path.write_text(formatted_yaml)
            
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
            
        except Exception as e:
            print(f"Error importing playlist: {e}", file=sys.stderr)
            import traceback
            traceback.print_exc()
            return error_response("INTERNAL_ERROR", str(e), status=500)

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

            # Hybrid Upload Handling
            # < 100MB: RAM for speed
            # >= 100MB or Unknown: Disk for safety
            
            RAM_LIMIT = 100 * 1024 * 1024  # 100MB
            content_length = request.content_length or 0
            temp_path = None
            
            try:
                # Use RAM only if we know the size is safe
                if content_length > 0 and content_length < RAM_LIMIT:
                    # Small file: Load into RAM
                    print(f"Upload size {content_length}: Processing in RAM", file=sys.stderr)
                    source = io.BytesIO(file.read())
                    zf = zipfile.ZipFile(source, 'r')
                else:
                    # Large file: Stream to Disk
                    print(f"Upload size {content_length}: Processing on Disk", file=sys.stderr)
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
                        with zf.open(playlist_file) as pf:
                            yaml_content = pf.read().decode('utf-8')
                            playlist_data = yaml.safe_load(yaml_content)
                    except UnicodeDecodeError:
                        return error_response("VALIDATION_ERROR", "playlist.yaml must be valid UTF-8")
                    except yaml.YAMLError as e:
                        return error_response("VALIDATION_ERROR", f"Invalid YAML: {e}")

                    if not playlist_data or not isinstance(playlist_data, dict):
                        return error_response("VALIDATION_ERROR", "Invalid playlist structure")

                    if 'items' not in playlist_data or not isinstance(playlist_data.get('items'), list):
                        return error_response("VALIDATION_ERROR", "Playlist must have an 'items' list")

                    # Determine output filename for playlist BEFORE extracting videos
                    title = playlist_data.get('title', '').strip()
                    if title:
                        safe_title = re.sub(r'[^\w\s-]', '', title).strip()
                        safe_title = re.sub(r'[-\s]+', '_', safe_title)
                        output_name = f"{safe_title}.yaml"
                    else:
                        output_name = "imported_playlist.yaml"

                    # Check if playlist exists BEFORE extracting any files
                    playlist_path = playlists_dir / output_name
                    if playlist_path.exists() and not overwrite:
                        return error_response(
                            "ALREADY_EXISTS",
                            f"Playlist '{output_name}' already exists. Set overwrite=true to replace.",
                            status=409
                        )

                    # Find media files in the ZIP
                    media_files = []
                    for name in namelist:
                        # Accept files in media/ folder or root level video files
                        lower_name = name.lower()
                        if lower_name.endswith(('.mp4', '.mkv', '.avi', '.mov', '.webm')):
                            media_files.append(name)

                    # Guard against ZIP bombs: limit total extracted size (default 10GB)
                    MAX_EXTRACT_BYTES = int(os.getenv("MAGIC_MAX_EXTRACT_MB", "10240")) * 1024 * 1024
                    total_extracted = 0

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
                            return error_response(
                                "VALIDATION_ERROR",
                                f"Package exceeds maximum extraction size ({MAX_EXTRACT_BYTES // (1024*1024)}MB)",
                                status=413
                            )

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
                        actual_written = 0
                        remaining = MAX_EXTRACT_BYTES - total_extracted
                        try:
                            with zf.open(media_file) as src, open(output_path, 'wb') as dst:
                                while True:
                                    chunk = src.read(64 * 1024)
                                    if not chunk:
                                        break
                                    actual_written += len(chunk)
                                    if actual_written > remaining:
                                        dst.close()
                                        try:
                                            output_path.unlink()
                                        except OSError:
                                            pass
                                        return error_response(
                                            "VALIDATION_ERROR",
                                            f"Package exceeds maximum extraction size ({MAX_EXTRACT_BYTES // (1024*1024)}MB)",
                                            status=413
                                        )
                                    dst.write(chunk)
                        except Exception:
                            try:
                                output_path.unlink()
                            except OSError:
                                pass
                            raise

                        total_extracted += actual_written
                        videos_imported += 1

                # Update playlist paths to use sanitized filenames (matching what we saved)
                for item in playlist_data.get('items', []):
                    if 'path' in item:
                        path = item['path']
                        if item.get('source_type') == 'local':
                            basename = os.path.basename(path)
                            # Sanitize the basename the same way we sanitized the files
                            safe_basename = re.sub(r'[^\w\s\-\.\[\]]', '', basename)
                            safe_basename = safe_basename.strip()
                            if safe_basename:
                                item['path'] = f"media/{safe_basename}"

                # Format and save playlist
                formatted_yaml = format_playlist_yaml(playlist_data)
                playlists_dir.mkdir(parents=True, exist_ok=True)
                playlist_path.write_text(formatted_yaml)

                item_count = len(playlist_data.get('items', []))
                print(f"Imported package: {output_name} ({item_count} items, {videos_imported} videos)", file=sys.stderr)

                return success_response(
                    data={
                        "playlist_filename": output_name,
                        "playlist_title": playlist_data.get('title', output_name),
                        "item_count": item_count,
                        "videos_imported": videos_imported,
                        "videos_skipped": videos_skipped,
                    },
                    message=f"Package imported: {item_count} playlist items, {videos_imported} videos uploaded"
                )

            except zipfile.BadZipFile:
                return error_response("VALIDATION_ERROR", "Invalid ZIP file")
            finally:
                # cleanup temp file
                if temp_path and os.path.exists(temp_path):
                    os.unlink(temp_path)

        except Exception as e:
            print(f"Error importing package: {e}", file=sys.stderr)
            import traceback
            traceback.print_exc()
            return error_response("INTERNAL_ERROR", str(e), status=500)

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

        def _get_clean_title(filename: str) -> str:
            """Extract clean title from filename (remove Youtube ID suffix)."""
            # Match "Title [video_id].ext"
            # Allow flexible ID format (anything in brackets at end of name)
            match = re.search(r"^(.*?) \[([^\]]+)\]\.[a-zA-Z0-9]+$", filename)
            if match:
                return match.group(1).strip()
            return filename

        media_list = [{
            'filename': f.name,
            'title': _get_clean_title(f.name),
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

        # Security check: only allow deletion within media subdirectories
        target_resolved = target.resolve()
        allowed_dirs = [
            (data_dir / "media").resolve(),
            (data_dir.parent / "dev_data" / "media").resolve(),
        ]

        if not any(str(target_resolved).startswith(str(d)) for d in allowed_dirs):
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

    # TTL for completed/errored jobs in any of the three job dicts. Without
    # eviction, a long-running kiosk that sees repeated upload/update/MB
    # operations accumulates entries indefinitely. Pruned opportunistically
    # at the start of each job-creating route — no background thread needed.
    _JOB_RETENTION_SECONDS = 3600  # 1 hour after terminal state

    def _prune_terminal_jobs(jobs_dict: dict) -> None:
        """Remove jobs in a terminal state older than _JOB_RETENTION_SECONDS.

        Uses the `_pruner_ts` field (numeric epoch, set at each job-creation
        site for prune-tracking purposes; intentionally separate from the
        existing user-facing `started_at` strings so we don't break their
        ISO-8601 contract). Jobs without `_pruner_ts` are kept forever — by
        design, since adding the field is opt-in at each call site."""
        cutoff = time.time() - _JOB_RETENTION_SECONDS
        terminal_states = {"complete", "completed", "error", "failed", "cancelled", "canceled"}
        stale = []
        for jid, job in jobs_dict.items():
            if not isinstance(job, dict):
                continue
            status = str(job.get("status", "")).lower()
            if status not in terminal_states:
                continue
            ts = job.get("_pruner_ts")
            if isinstance(ts, (int, float)) and ts < cutoff:
                stale.append(jid)
        for jid in stale:
            jobs_dict.pop(jid, None)

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
        
        # Audio normalization via FFmpeg's loudnorm filter (EBU R128).
        # Matches the Retro Ripper companion tool's settings exactly so a
        # video uploaded directly via the Content Manager sounds the same
        # as one ripped externally and dropped in:
        #   I=-16  → integrated loudness target -16 LUFS (YouTube-like;
        #            warmer/louder than the -23 LUFS broadcast standard
        #            we used to default to here)
        #   TP=-1  → max true peak -1 dBTP (small headroom for resampler
        #            ringing; matches Retro Ripper)
        #   LRA=11 → loudness range 11 LU (allows reasonable dynamics
        #            instead of squashing to 7 LU)
        # Reference: retro_ripper/config/config.py:AUDIO_TARGET_LUFS,
        # AUDIO_TRUE_PEAK, AUDIO_LOUDNESS_RANGE.
        if normalize_audio:
            ffmpeg_cmd.extend(['-af', 'loudnorm=I=-16:TP=-1:LRA=11'])
        
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
        # `normalize_audio` arrives as the string "true"/"false" (FormData
        # POST). Coerce to bool. Default ON because phone-uploaded clips
        # almost always have inconsistent levels — we'd rather opt-out
        # than opt-in for the typical user flow.
        normalize_audio = (request.form.get("normalize_audio", "true").lower()
                           == "true")

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

        # Initialize job (prune stale terminal-state entries first)
        _prune_terminal_jobs(transcode_jobs)
        transcode_jobs[job_id] = {
            'status': 'pending',
            'progress': 0,
            'message': 'Upload complete, starting transcode...',
            'output_path': None,
            'output_filename': output_name,
            '_pruner_ts': time.time(),
        }

        # Start transcoding in background thread. Pass normalize_audio
        # through — without this, the upload UI's "Normalize audio
        # volume" checkbox was being silently ignored because the
        # request handler dropped the form field on the floor.
        thread = threading.Thread(
            target=run_transcode_job,
            args=(job_id, temp_input, output_path, resolution, normalize_audio)
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
        # See upload_and_transcode for normalize_audio default rationale.
        # Default ON here too so a phone upload of an already-720p clip
        # still gets its audio levels fixed even when the video itself
        # doesn't need re-encoding.
        normalize_audio = (request.form.get("normalize_audio", "true").lower()
                           == "true")

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

        # If the video is already at target resolution+codec but the
        # user wants audio normalized, we can't take the "direct copy"
        # shortcut — flip needs_transcode on so the loudnorm pass
        # runs. The transcode is fast in this case (still has to
        # re-encode video but the loudnorm filter can be heavy on
        # long clips, so this is the right tradeoff).
        if normalize_audio and not probe_result['needs_transcode']:
            probe_result['needs_transcode'] = True
            probe_result['reason'] = (
                'Audio normalization requested — re-encoding to apply '
                'loudnorm filter even though video already matches target.'
            )

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

            # Initialize job (prune stale terminal-state entries first)
            _prune_terminal_jobs(transcode_jobs)
            transcode_jobs[job_id] = {
                'status': 'pending',
                'progress': 0,
                'message': 'Starting transcode...',
                'output_path': None,
                'output_filename': output_name,
                '_pruner_ts': time.time(),
            }

            # Start transcoding in background thread. normalize_audio
            # propagates from the form here as well — without this the
            # loudnorm filter never fires regardless of UI checkbox.
            thread = threading.Thread(
                target=run_transcode_job,
                args=(job_id, temp_input, output_path, resolution, normalize_audio)
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

        # Create job (prune stale terminal-state entries first)
        _prune_terminal_jobs(update_jobs)
        job_id = str(uuid.uuid4())
        update_jobs[job_id] = {
            'status': 'running',
            'stage': 'preparing',
            'progress': 0,
            'message': 'Starting update...',
            'version': version,
            'new_version': None,
            '_pruner_ts': time.time(),
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

    # ===== MEDIA BROWSER (RADARR/PROWLARR/QBIT/GLUETUN) SETUP =====
    #
    # Provisions the Media Browser docker stack on a fresh Pi. Operator drops a
    # WireGuard .conf from the ProtonVPN dashboard into the Content Manager UI;
    # we parse out the 4 vars Gluetun needs, write them into
    # /opt/magic_dingus_box/services/.env (preserving any non-WG vars), then
    # invoke setup_services.sh as a background job and stream its stdout for
    # the frontend to tail.

    SERVICES_DIR = data_dir.parent.parent / "services"
    SERVICES_ENV = SERVICES_DIR / ".env"
    SETUP_SERVICES_SCRIPT = data_dir.parent / "scripts" / "setup_services.sh"

    EXPECTED_CONTAINERS = [
        "mdb_gluetun",
        "mdb_radarr",
        "mdb_prowlarr",
        "mdb_qbittorrent",
        "mdb_byparr",
    ]

    # Track media-browser setup jobs (in-memory, cleared on restart)
    media_browser_jobs: dict = {}
    _MB_LOG_BUFFER_LIMIT = 500  # keep at most this many lines per job
    _MB_LOG_TAIL_LINES = 30     # return this many lines on each status poll

    def _parse_wireguard_config(text: str) -> dict:
        """Parse a WireGuard .conf file into the 4 vars Gluetun needs.

        Returns a dict with keys:
          WIREGUARD_PRIVATE_KEY, WIREGUARD_ADDRESSES,
          WIREGUARD_PUBLIC_KEY,  WIREGUARD_ENDPOINT_IP
        Raises ValueError on missing/malformed fields.
        """
        section = None
        interface: dict = {}
        peer: dict = {}
        for raw in text.splitlines():
            line = raw.strip()
            if not line or line.startswith("#") or line.startswith(";"):
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1].strip().lower()
                continue
            if "=" not in line:
                continue
            key, _, value = line.partition("=")
            key = key.strip()
            value = value.strip()
            if section == "interface":
                interface[key] = value
            elif section == "peer":
                peer[key] = value

        missing = []
        if "PrivateKey" not in interface:
            missing.append("[Interface] PrivateKey")
        if "Address" not in interface:
            missing.append("[Interface] Address")
        if "PublicKey" not in peer:
            missing.append("[Peer] PublicKey")
        if "Endpoint" not in peer:
            missing.append("[Peer] Endpoint")
        if missing:
            raise ValueError(f"Missing required fields: {', '.join(missing)}")

        endpoint = peer["Endpoint"]
        # Endpoint format: "host:port" — split off the port. Handle bracketed
        # IPv6 like "[2001:db8::1]:51820" defensively.
        if endpoint.startswith("["):
            endpoint_ip = endpoint[1:].split("]", 1)[0]
        else:
            endpoint_ip = endpoint.rsplit(":", 1)[0]
        if not endpoint_ip:
            raise ValueError("Could not parse host from [Peer] Endpoint")

        return {
            "WIREGUARD_PRIVATE_KEY": interface["PrivateKey"],
            "WIREGUARD_ADDRESSES": interface["Address"],
            "WIREGUARD_PUBLIC_KEY": peer["PublicKey"],
            "WIREGUARD_ENDPOINT_IP": endpoint_ip,
        }

    def _read_env_file(path: Path) -> dict:
        """Parse a KEY=VALUE .env file into a dict. Returns {} if missing."""
        if not path.exists():
            return {}
        result = {}
        try:
            for raw in path.read_text().splitlines():
                line = raw.strip()
                if not line or line.startswith("#"):
                    continue
                if "=" not in line:
                    continue
                key, _, value = line.partition("=")
                result[key.strip()] = value.strip()
        except Exception:
            return {}
        return result

    def _write_env_file(path: Path, env: dict) -> None:
        """Write a dict back to a .env file with chmod 600. Creates parent dir."""
        path.parent.mkdir(parents=True, exist_ok=True)
        # Write to a tempfile in the same dir, then atomic rename, so a crash
        # mid-write can't leave a partial .env.
        tmp = path.with_suffix(path.suffix + ".tmp")
        lines = [f"{k}={v}" for k, v in env.items()]
        tmp.write_text("\n".join(lines) + "\n")
        try:
            os.chmod(tmp, 0o600)
        except Exception:
            pass
        tmp.replace(path)

    def _detect_timezone() -> str:
        """Best-effort host timezone detection; falls back to UTC."""
        try:
            result = subprocess.run(
                ["timedatectl", "show", "-p", "Timezone", "--value"],
                capture_output=True, text=True, timeout=5,
            )
            tz = result.stdout.strip()
            if tz:
                return tz
        except Exception:
            pass
        return "UTC"

    def _docker_ps_table() -> list[dict]:
        """Return [{name, status}] for the expected media-browser containers."""
        try:
            result = subprocess.run(
                ["docker", "ps", "-a", "--format", "{{.Names}}\t{{.Status}}"],
                capture_output=True, text=True, timeout=5,
            )
            if result.returncode != 0:
                return [{"name": n, "status": "unknown"} for n in EXPECTED_CONTAINERS]
            running = {}
            for line in result.stdout.splitlines():
                parts = line.split("\t", 1)
                if len(parts) == 2:
                    running[parts[0].strip()] = parts[1].strip()
            return [
                {"name": n, "status": running.get(n, "not found")}
                for n in EXPECTED_CONTAINERS
            ]
        except Exception:
            return [{"name": n, "status": "unknown"} for n in EXPECTED_CONTAINERS]

    def _vpn_exit_info() -> dict:
        """Hit gluetun's local control server for current exit IP + country.

        Gluetun's control server listens on port 8000 INSIDE the container and
        is not (by default) exposed on the host, so we have to shell into the
        container with `docker exec`. Returns empty strings when gluetun isn't
        running or anything goes wrong — never raises.
        """
        try:
            result = subprocess.run(
                ["docker", "exec", "mdb_gluetun", "wget", "-qO-",
                 "http://localhost:8000/v1/publicip/ip"],
                capture_output=True, text=True, timeout=5,
            )
            if result.returncode != 0 or not result.stdout.strip():
                return {"vpn_exit_ip": "", "vpn_country": ""}
            payload = json.loads(result.stdout)
            return {
                "vpn_exit_ip": payload.get("public_ip", "") or "",
                "vpn_country": payload.get("country", "") or "",
            }
        except Exception:
            return {"vpn_exit_ip": "", "vpn_country": ""}

    def _env_has_wireguard_key(path: Path) -> bool:
        """True iff .env exists AND has a non-empty WIREGUARD_PRIVATE_KEY=."""
        env = _read_env_file(path)
        return bool(env.get("WIREGUARD_PRIVATE_KEY", "").strip())

    def _vpn_configured() -> bool:
        """True iff services/.env exists AND has a non-empty WIREGUARD_PRIVATE_KEY.

        Layer 2 of the three-layer Media Browser gate. Failure-closed:
        any error reading the .env returns False so a malformed file
        can't accidentally allow access.

        Delegates to _env_has_wireguard_key with the canonical SERVICES_ENV
        path. Use _env_has_wireguard_key directly if you need to check a
        non-canonical path (e.g., during setup-job preview).
        """
        return _env_has_wireguard_key(SERVICES_ENV)

    def _vpn_required_response():
        """Standard 403 used when Layer 2 (VPN configured) fails."""
        return error_response(
            "vpn_not_configured",
            "VPN must be configured in the Media Browser tab before using this feature",
            status=403,
        )

    def _check_media_browser_gates(*, require_vpn: bool = True):
        """Run the Layer 1 + (optionally) Layer 2 gates.

        Returns None on pass, or a 403 Response on fail. Endpoints that
        are part of the VPN-setup flow itself (status, setup,
        setup-status, reset) pass require_vpn=False so the operator can
        reach them before configuring VPN.
        """
        if not _media_browser_unlocked():
            return _media_browser_locked_response()
        if require_vpn and not _vpn_configured():
            return _vpn_required_response()
        return None

    @app.get("/admin/media-browser/visibility")
    def media_browser_visibility():  # type: ignore[no-redef]
        """Public — return whether the Media Browser tab should be rendered.

        Always 200, never errors. Returns two flags:
          - visible: Layer 1 (unlock). Whether to render the tab DOM
            at all.
          - vpn_configured: Layer 2 (WireGuard config dropped). When
            visible=true and vpn_configured=false, the frontend shows
            a "Configure VPN" form instead of the dashboard.

        Other /admin/media-browser/* routes enforce the same gates
        server-side via _check_media_browser_gates and return 403
        (`media_browser_locked` or `vpn_not_configured`) on failure.
        """
        return success_response(data={
            "visible": _media_browser_unlocked(),
            "vpn_configured": _vpn_configured(),
        })

    @app.get("/admin/media-browser/status")
    def media_browser_status():  # type: ignore[no-redef]
        """Return current Media Browser configuration + service health.

        Drives the 3-state UI: Not configured / Configuring / Configured.
        """
        if (resp := _check_media_browser_gates(require_vpn=False)):
            return resp
        env_present = _env_has_wireguard_key(SERVICES_ENV)
        containers = _docker_ps_table()
        services_running = any(
            c["status"].lower().startswith("up")
            for c in containers
        )
        vpn = _vpn_exit_info() if services_running else {"vpn_exit_ip": "", "vpn_country": ""}

        return success_response(data={
            "configured": env_present,
            "env_present": env_present,
            "services_running": services_running,
            "containers": containers,
            "vpn_exit_ip": vpn["vpn_exit_ip"],
            "vpn_country": vpn["vpn_country"],
        })

    def _run_media_browser_setup_job(job_id: str):
        """Background thread: stream setup_services.sh output into the job buffer."""
        job = media_browser_jobs[job_id]
        try:
            process = subprocess.Popen(
                ["sudo", "-n", str(SETUP_SERVICES_SCRIPT)],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                start_new_session=True,
            )
            job["process"] = process

            for line in process.stdout:
                line = line.rstrip("\n")
                buf = job["log"]
                buf.append(line)
                if len(buf) > _MB_LOG_BUFFER_LIMIT:
                    del buf[: len(buf) - _MB_LOG_BUFFER_LIMIT]

            process.wait()
            job["exit_code"] = process.returncode
            job["status"] = "success" if process.returncode == 0 else "failed"
        except Exception as e:
            job["log"].append(f"[admin.py] setup job crashed: {e}")
            job["status"] = "failed"
            job["exit_code"] = -1

    @app.post("/admin/media-browser/setup")
    @require_csrf
    def media_browser_setup():  # type: ignore[no-redef]
        """Configure VPN credentials + run setup_services.sh.

        Accepts the WireGuard config either as a multipart .conf file upload
        ('file' field) or as pasted text in the 'config_text' form field.
        Returns a job_id; clients poll /admin/media-browser/setup-status/<id>.
        """
        if (resp := _check_media_browser_gates(require_vpn=False)):
            return resp
        if not SETUP_SERVICES_SCRIPT.exists():
            return error_response(
                "setup_script_missing",
                f"setup_services.sh not found at {SETUP_SERVICES_SCRIPT}",
                status=500,
            )

        # Pull config text from upload or form field
        config_text = ""
        if "file" in request.files and request.files["file"].filename:
            try:
                config_text = request.files["file"].read().decode("utf-8", errors="replace")
            except Exception as e:
                return error_response("invalid_wireguard_config",
                                      f"Could not read uploaded file: {e}", status=400)
        else:
            config_text = (request.form.get("config_text") or "").strip()

        if not config_text:
            return error_response(
                "invalid_wireguard_config",
                "No WireGuard config provided (expected .conf file upload or 'config_text' form field)",
                status=400,
            )

        try:
            wg = _parse_wireguard_config(config_text)
        except ValueError as e:
            return error_response("invalid_wireguard_config",
                                  f"Could not parse WireGuard config: {e}", status=400)

        # Quick NOPASSWD sudo precheck so we fail fast with a clear error
        # rather than silently spawning a process that hangs on a password prompt.
        try:
            sudo_check = subprocess.run(
                ["sudo", "-n", "true"], capture_output=True, text=True, timeout=5
            )
            if sudo_check.returncode != 0:
                return error_response(
                    "sudo_required",
                    "magic user must have NOPASSWD sudo configured",
                    status=500,
                )
        except Exception:
            return error_response(
                "sudo_required",
                "magic user must have NOPASSWD sudo configured",
                status=500,
            )

        # Merge WG vars + sensible defaults into existing .env
        env = _read_env_file(SERVICES_ENV)
        env.update(wg)

        country = (request.form.get("country") or "").strip() or "Netherlands"
        defaults = {
            "VPN_SERVICE_PROVIDER": "protonvpn",
            "VPN_TYPE": "wireguard",
            "VPN_PORT_FORWARDING": "on",
            "VPN_COUNTRIES": country,
            "STORAGE_ROOT": "/mnt/ssd",
            "PUID": "1000",
            "PGID": "1000",
            "TZ": _detect_timezone(),
        }
        for key, value in defaults.items():
            if not env.get(key):
                env[key] = value

        try:
            _write_env_file(SERVICES_ENV, env)
        except Exception as e:
            return error_response(
                "env_write_failed",
                f"Could not write {SERVICES_ENV}: {e}",
                status=500,
            )

        # Start the long-running setup script in a background thread
        # (prune stale terminal-state entries first)
        _prune_terminal_jobs(media_browser_jobs)
        job_id = str(uuid.uuid4())
        media_browser_jobs[job_id] = {
            "status": "running",
            "exit_code": None,
            "log": [],
            "started_at": datetime.now().isoformat(),
            "started_ts": time.time(),
            "process": None,
            "_pruner_ts": time.time(),
        }
        thread = threading.Thread(
            target=_run_media_browser_setup_job, args=(job_id,), daemon=True
        )
        thread.start()

        return success_response(
            data={"job_id": job_id},
            message="Media Browser setup started",
        )

    @app.get("/admin/media-browser/setup-status/<job_id>")
    def media_browser_setup_status(job_id):  # type: ignore[no-redef]
        """Return last N log lines + status for a setup job.

        On unknown job_id (e.g. server restart cleared in-memory state), returns
        success with status='unknown' rather than 404 so the frontend can fall
        back to the generic /admin/media-browser/status endpoint.
        """
        if (resp := _check_media_browser_gates(require_vpn=False)):
            return resp
        job = media_browser_jobs.get(job_id)
        if not job:
            return success_response(data={
                "status": "unknown",
                "log_lines": [],
                "exit_code": None,
                "started_at": None,
                "elapsed_sec": 0,
            })

        # If the background thread hasn't yet observed a finished process,
        # poll the Popen handle defensively to keep status fresh.
        process = job.get("process")
        if job["status"] == "running" and process is not None:
            rc = process.poll()
            if rc is not None:
                job["exit_code"] = rc
                job["status"] = "success" if rc == 0 else "failed"

        log = job["log"]
        tail = log[-_MB_LOG_TAIL_LINES:] if len(log) > _MB_LOG_TAIL_LINES else list(log)

        return success_response(data={
            "status": job["status"],
            "log_lines": tail,
            "exit_code": job["exit_code"],
            "started_at": job["started_at"],
            "elapsed_sec": int(time.time() - job["started_ts"]),
        })

    @app.get("/admin/media-browser/credentials")
    def media_browser_credentials():  # type: ignore[no-redef]
        """Return Radarr / Prowlarr API keys + qBit admin password.

        Operators use these to SSH-tunnel into the Pi and admin the services
        directly (Radarr at :7878, Prowlarr at :9696, qBit at :8080).

        Lazy-loaded by the frontend — only fetched when the operator opens
        the "Show credentials" expander, never on routine status polls, to
        avoid leaking secrets into background traffic.
        """
        if (resp := _check_media_browser_gates()):
            return resp

        env = _read_env_file(SERVICES_ENV)
        radarr_key = env.get("RADARR_API_KEY", "").strip()
        prowlarr_key = env.get("PROWLARR_API_KEY", "").strip()
        qbit_password = env.get("QBITTORRENT_ADMIN_PASSWORD", "").strip()

        # Treat unset / placeholder values as "not ready". setup_services.sh
        # writes __WILL_BE_SET_AFTER_FIRST_START__ initially and then patches
        # in the real keys once Radarr+Prowlarr have generated them.
        def _is_placeholder(value: str) -> bool:
            return (
                not value
                or value.startswith("__WILL_BE_SET_AFTER_FIRST_START__")
                or value.startswith("__")
            )

        if _is_placeholder(radarr_key) or _is_placeholder(prowlarr_key) or _is_placeholder(qbit_password):
            return error_response(
                "credentials_not_ready",
                "Setup not yet complete",
                status=400,
            )

        return success_response(data={
            "radarr_api_key": radarr_key,
            "prowlarr_api_key": prowlarr_key,
            "qbittorrent_admin_username": env.get("QBITTORRENT_ADMIN_USERNAME", "admin"),
            "qbittorrent_admin_password": qbit_password,
        })

    def _radarr_library_count(env: dict) -> int:
        """Count movies in Radarr's library. Returns -1 on failure."""
        api_key = env.get("RADARR_API_KEY", "").strip()
        if not api_key or api_key.startswith("__"):
            return -1
        try:
            result = subprocess.run(
                ["curl", "-sS", "--max-time", "5",
                 "-H", f"X-Api-Key: {api_key}",
                 "http://localhost:7878/api/v3/movie"],
                capture_output=True, text=True, timeout=6,
            )
            if result.returncode != 0 or not result.stdout.strip():
                return -1
            payload = json.loads(result.stdout)
            return len(payload) if isinstance(payload, list) else -1
        except Exception:
            return -1

    def _radarr_queue_summary(env: dict) -> dict:
        """Return {count, active_dl_mbps}. Returns {-1, 0.0} on failure."""
        api_key = env.get("RADARR_API_KEY", "").strip()
        if not api_key or api_key.startswith("__"):
            return {"count": -1, "active_dl_mbps": 0.0}
        try:
            result = subprocess.run(
                ["curl", "-sS", "--max-time", "5",
                 "-H", f"X-Api-Key: {api_key}",
                 "http://localhost:7878/api/v3/queue?pageSize=50"],
                capture_output=True, text=True, timeout=6,
            )
            if result.returncode != 0 or not result.stdout.strip():
                return {"count": -1, "active_dl_mbps": 0.0}
            payload = json.loads(result.stdout)
            records = payload.get("records", []) if isinstance(payload, dict) else []
            # Radarr reports speed in bytes/sec under varying keys depending on
            # version — try the common ones, default to 0.
            total_bps = 0.0
            for r in records:
                for k in ("downloadRate", "downloadSpeed", "speed"):
                    if k in r and isinstance(r[k], (int, float)):
                        total_bps += float(r[k])
                        break
            mbps = round((total_bps * 8) / 1_000_000, 2)
            return {"count": len(records), "active_dl_mbps": mbps}
        except Exception:
            return {"count": -1, "active_dl_mbps": 0.0}

    def _qbit_torrent_summary(env: dict) -> dict:
        """Return {active, seeding} torrent counts. Returns {-1, -1} on failure."""
        username = env.get("QBITTORRENT_ADMIN_USERNAME", "admin")
        password = env.get("QBITTORRENT_ADMIN_PASSWORD", "").strip()
        if not password or password.startswith("__"):
            return {"active": -1, "seeding": -1}
        cookie_jar = tempfile.NamedTemporaryFile(suffix=".cookies", delete=False)
        cookie_jar.close()
        try:
            # Pass credentials via stdin (-d @-) instead of argv to avoid
            # leaking the password into /proc/<pid>/cmdline, where any local
            # process can read it.
            login = subprocess.run(
                ["curl", "-sS", "--max-time", "5",
                 "-c", cookie_jar.name,
                 "-d", "@-",
                 "http://localhost:8080/api/v2/auth/login"],
                input=f"username={username}&password={password}",
                capture_output=True, text=True, timeout=6,
            )
            if login.returncode != 0 or "Ok." not in (login.stdout or ""):
                return {"active": -1, "seeding": -1}
            info = subprocess.run(
                ["curl", "-sS", "--max-time", "5",
                 "-b", cookie_jar.name,
                 "http://localhost:8080/api/v2/torrents/info"],
                capture_output=True, text=True, timeout=6,
            )
            if info.returncode != 0 or not info.stdout.strip():
                return {"active": -1, "seeding": -1}
            torrents = json.loads(info.stdout)
            if not isinstance(torrents, list):
                return {"active": -1, "seeding": -1}
            seeding_states = {"uploading", "stalledUP", "queuedUP", "forcedUP", "checkingUP"}
            seeding = sum(1 for t in torrents if t.get("state") in seeding_states)
            return {"active": len(torrents), "seeding": seeding}
        except Exception:
            return {"active": -1, "seeding": -1}
        finally:
            try:
                os.unlink(cookie_jar.name)
            except Exception:
                pass

    def _qbit_listen_port(env: dict) -> int:
        """Return qBit's currently-configured listen_port, or -1 on failure."""
        username = env.get("QBITTORRENT_ADMIN_USERNAME", "admin")
        password = env.get("QBITTORRENT_ADMIN_PASSWORD", "").strip()
        if not password or password.startswith("__"):
            return -1
        cookie_jar = tempfile.NamedTemporaryFile(suffix=".cookies", delete=False)
        cookie_jar.close()
        try:
            # Credentials via stdin — see _qbit_torrent_summary for rationale.
            login = subprocess.run(
                ["curl", "-sS", "--max-time", "5",
                 "-c", cookie_jar.name,
                 "-d", "@-",
                 "http://localhost:8080/api/v2/auth/login"],
                input=f"username={username}&password={password}",
                capture_output=True, text=True, timeout=6,
            )
            if login.returncode != 0 or "Ok." not in (login.stdout or ""):
                return -1
            prefs = subprocess.run(
                ["curl", "-sS", "--max-time", "5",
                 "-b", cookie_jar.name,
                 "http://localhost:8080/api/v2/app/preferences"],
                capture_output=True, text=True, timeout=6,
            )
            if prefs.returncode != 0 or not prefs.stdout.strip():
                return -1
            payload = json.loads(prefs.stdout)
            return int(payload.get("listen_port", -1))
        except Exception:
            return -1
        finally:
            try:
                os.unlink(cookie_jar.name)
            except Exception:
                pass

    def _gluetun_forwarded_port() -> int:
        """Return Gluetun's NAT-PMP forwarded port, 0 if unavailable, -1 on failure."""
        try:
            result = subprocess.run(
                ["docker", "exec", "mdb_gluetun", "wget", "-qO-",
                 "http://localhost:8000/v1/openvpn/portforwarded"],
                capture_output=True, text=True, timeout=5,
            )
            if result.returncode != 0 or not result.stdout.strip():
                # /v1/openvpn/portforwarded is the canonical endpoint; older
                # gluetun builds expose /v1/portforward instead. Try that as
                # a fallback before giving up.
                fallback = subprocess.run(
                    ["docker", "exec", "mdb_gluetun", "wget", "-qO-",
                     "http://localhost:8000/v1/portforward"],
                    capture_output=True, text=True, timeout=5,
                )
                if fallback.returncode != 0 or not fallback.stdout.strip():
                    return -1
                payload = json.loads(fallback.stdout)
            else:
                payload = json.loads(result.stdout)
            return int(payload.get("port", 0))
        except Exception:
            return -1

    @app.get("/admin/media-browser/health-summary")
    def media_browser_health_summary():  # type: ignore[no-redef]
        """Aggregate one-shot health snapshot for the State C dashboard.

        Manual-refresh only — no auto-poll on the frontend. Each external
        call has a 5-sec timeout and is wrapped in try/except; any failing
        field returns -1 / "unavailable" without breaking the others.
        """
        if (resp := _check_media_browser_gates()):
            return resp

        env = _read_env_file(SERVICES_ENV)
        library_count = _radarr_library_count(env)
        queue = _radarr_queue_summary(env)
        qbit = _qbit_torrent_summary(env)
        forwarded_port = _gluetun_forwarded_port()
        qbit_listen = _qbit_listen_port(env)

        if forwarded_port == -1:
            port_status = "unavailable"
        elif forwarded_port == 0:
            port_status = "unavailable"
        elif qbit_listen == -1:
            port_status = "unavailable"
        elif forwarded_port == qbit_listen:
            port_status = "synced"
        else:
            port_status = "drift"

        return success_response(data={
            "library_count": library_count,
            "queue_count": queue["count"],
            "queue_active_dl_mbps": queue["active_dl_mbps"],
            "qbit_active_torrents": qbit["active"],
            "qbit_seeding_torrents": qbit["seeding"],
            "vpn_forwarded_port": forwarded_port if forwarded_port > 0 else 0,
            "vpn_port_status": port_status,
        })

    @app.post("/admin/media-browser/restart")
    @require_csrf
    def media_browser_restart():  # type: ignore[no-redef]
        """Restart the magic-dingus-services systemd unit (~30 sec).

        Runs `sudo -n systemctl restart magic-dingus-services.service` —
        same path setup_services.sh uses, so it relies on the same NOPASSWD
        sudoers rule.
        """
        if (resp := _check_media_browser_gates()):
            return resp

        try:
            sudo_check = subprocess.run(
                ["sudo", "-n", "true"], capture_output=True, text=True, timeout=5
            )
            if sudo_check.returncode != 0:
                return error_response(
                    "sudo_required",
                    "magic user must have NOPASSWD sudo configured",
                    status=500,
                )
        except Exception:
            return error_response(
                "sudo_required",
                "magic user must have NOPASSWD sudo configured",
                status=500,
            )

        try:
            result = subprocess.run(
                ["sudo", "-n", "systemctl", "restart", "magic-dingus-services.service"],
                capture_output=True, text=True, timeout=120,
            )
            if result.returncode != 0:
                return error_response(
                    "restart_failed",
                    (result.stderr or result.stdout or "systemctl restart failed").strip(),
                    status=500,
                )
            return success_response(message="Services restarted")
        except subprocess.TimeoutExpired:
            return error_response("timeout", "Restart timed out after 120 seconds", status=504)
        except Exception as e:
            return error_response("restart_failed", str(e), status=500)

    @app.post("/admin/media-browser/reset")
    @require_csrf
    def media_browser_reset():  # type: ignore[no-redef]
        """Tear down the docker stack + wipe configuration. Movies on the SSD
        library are NOT touched; only Radarr's metadata + service config is.

        Requires confirmation token in JSON body to prevent accidental clicks
        in dev tools / curl. After this completes, /status returns
        configured=false → frontend transitions back to State A.
        """
        if (resp := _check_media_browser_gates(require_vpn=False)):
            return resp

        body = request.get_json(silent=True) or {}
        if body.get("confirm") != "RESET":
            return error_response(
                "confirmation_required",
                'Reset requires {"confirm": "RESET"} in request body',
                status=400,
            )

        try:
            sudo_check = subprocess.run(
                ["sudo", "-n", "true"], capture_output=True, text=True, timeout=5
            )
            if sudo_check.returncode != 0:
                return error_response(
                    "sudo_required",
                    "magic user must have NOPASSWD sudo configured",
                    status=500,
                )
        except Exception:
            return error_response(
                "sudo_required",
                "magic user must have NOPASSWD sudo configured",
                status=500,
            )

        steps_completed = []

        # 1. docker compose down — stop + remove containers + networks
        try:
            result = subprocess.run(
                ["sudo", "-n", "docker", "compose", "down", "--remove-orphans"],
                cwd=str(SERVICES_DIR),
                capture_output=True, text=True, timeout=60,
            )
            if result.returncode != 0:
                # Non-fatal — proceed with file cleanup anyway so a stuck
                # docker daemon can't strand a half-reset state.
                steps_completed.append(
                    f"compose_down_failed:{(result.stderr or result.stdout or '').strip()[:200]}"
                )
            else:
                steps_completed.append("compose_down")
        except Exception as e:
            steps_completed.append(f"compose_down_exception:{e}")

        # 2. Remove .env (drops VPN credentials + API keys)
        try:
            if SERVICES_ENV.exists():
                SERVICES_ENV.unlink()
            steps_completed.append("env_removed")
        except Exception as e:
            return error_response(
                "reset_failed",
                f"Could not remove {SERVICES_ENV}: {e}",
                status=500,
                details={"steps": steps_completed},
            )

        # 3. Wipe service config dirs (radarr/prowlarr/qbit/gluetun/byparr)
        config_dirs_root = SERVICES_DIR / "config"
        targets = ["radarr", "prowlarr", "qbittorrent", "gluetun", "byparr"]
        for name in targets:
            target = config_dirs_root / name
            if not target.exists():
                continue
            try:
                # Use sudo for cleanup since service containers run as a
                # different uid and may have written root-owned state.
                rm_result = subprocess.run(
                    ["sudo", "-n", "find", str(target), "-mindepth", "1", "-delete"],
                    capture_output=True, text=True, timeout=30,
                )
                if rm_result.returncode != 0:
                    steps_completed.append(
                        f"wipe_{name}_failed:{(rm_result.stderr or '').strip()[:120]}"
                    )
                else:
                    steps_completed.append(f"wipe_{name}")
            except Exception as e:
                steps_completed.append(f"wipe_{name}_exception:{e}")

        return success_response(
            data={"steps": steps_completed},
            message="Media Browser reset",
        )

    # ============= Phone Remote — debug endpoint =============
    # Curl-driven smoke test: POST /admin/remote/_debug/press?btn=OK&phase=tap
    # Auth is intentionally absent here — Phase C adds the real /admin/remote/ws
    # which is HMAC-cookie gated. This endpoint stays available for diagnostics.
    @app.route("/admin/remote/_debug/press", methods=["POST"])
    def remote_debug_press():
        from flask import abort
        cookie = request.cookies.get(remote_auth.COOKIE_NAME, "")
        if remote_auth.verify_cookie(cookie) is None:
            abort(401)
        btn = request.args.get("btn", "")
        phase = request.args.get("phase", "tap")
        writer = app.config.get("UINPUT_WRITER")
        if writer is None:
            try:
                writer = UinputWriter()  # opens real /dev/uinput
                app.config["UINPUT_WRITER"] = writer
            except Exception as e:
                return error_response("uinput_unavailable", str(e), status=503)
        try:
            writer.press(btn, phase=phase)
        except ValueError as e:
            return error_response("bad_button", str(e))
        return success_response({"sent": btn})

    # Phone Remote — WebSocket endpoint (auth via mdb_remote cookie).
    if sock is not None:
        @sock.route("/admin/remote/ws")
        def remote_ws(ws):
            writer = app.config.get("UINPUT_WRITER")
            if writer is None:
                try:
                    writer = UinputWriter()
                    app.config["UINPUT_WRITER"] = writer
                except Exception:
                    # Best-effort: send an error and close. The phone will retry.
                    try:
                        ws.send(json.dumps({"t": "error",
                                            "code": "uinput_unavailable"}))
                        ws.close()
                    except Exception:
                        pass
                    return
            ws_handler.handle_connection(
                ws,
                uinput_writer=writer,
                data_dir=Path(app.config["DATA_DIR"]),
                verify_cookie=remote_auth.verify_cookie,
            )
    else:
        import warnings
        warnings.warn(
            "flask-sock not installed; /admin/remote/ws WebSocket endpoint is unavailable.",
            RuntimeWarning,
            stacklevel=2,
        )

    # ===== SERVE WEB INTERFACE =====

    @app.get("/")
    @app.get("/admin")
    def admin_interface():  # type: ignore[no-redef]
        """Serve the web interface.

        If ?pair=<code> is present, delegate to the phone-remote pairing flow
        before serving the static SPA so that the kiosk QR-code link is handled
        transparently.
        """
        pair_code = request.args.get("pair")
        if pair_code:
            return remote_auth.handle_pair_param(pair_code)
        static_dir = Path(__file__).parent / "static"
        return send_file(static_dir / "index.html")

    @app.route("/admin/remote", methods=["GET"])
    def remote_page():  # type: ignore[no-redef]
        cookie = request.cookies.get(remote_auth.COOKIE_NAME, "")
        device_id = remote_auth.verify_cookie(cookie)
        if device_id is None:
            return render_template_string("""
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#1F191F">
<title>Remote not paired</title>
<style>
  html, body { margin: 0; padding: 0; background: #1F191F; color: #F2E4D9;
               font-family: -apple-system, system-ui, sans-serif;
               min-height: 100vh; display: flex; align-items: center; justify-content: center; }
  .card { width: min(360px, 90%); padding: 32px 24px; background: #2A232A;
          border-radius: 16px; text-align: center; }
  h1 { margin: 0 0 12px; font-size: 22px; }
  p { color: #968B85; line-height: 1.5; }
</style>
</head>
<body>
<div class="card">
  <h1>Remote not paired</h1>
  <p>On the kiosk, open <strong>Settings &rarr; Phone Remote</strong> and scan the QR code with your phone&#39;s camera.</p>
</div>
</body></html>
""")
        # Cookie OK — serve the static remote shell
        return send_from_directory("static/remote", "remote.html")

    @app.route("/admin/remote/name", methods=["GET", "POST"])
    def remote_name():  # type: ignore[no-redef]
        """Nickname-prompt page shown immediately after a successful pair."""
        cookie = request.cookies.get(remote_auth.COOKIE_NAME, "")
        device_id = remote_auth.verify_cookie(cookie)
        if device_id is None:
            return redirect("/", code=303)

        paired_path = Path(app.config["DATA_DIR"]) / "paired_remotes.json"

        if request.method == "POST":
            nickname = (request.form.get("nickname") or "").strip()[:40] or "Phone"
            # Update the entry in paired_remotes.json
            try:
                data = json.loads(paired_path.read_text())
            except (FileNotFoundError, json.JSONDecodeError):
                data = {"schema": 1, "devices": []}
            for d in data["devices"]:
                if d["id"] == device_id:
                    d["nickname"] = nickname
                    break
            paired_path.write_text(json.dumps(data, indent=2))
            target = request.args.get("tab", "remote")
            return redirect(f"/?tab={target}", code=303)

        # GET — render the form. User-Agent hint becomes the placeholder.
        ua = request.headers.get("User-Agent", "")
        placeholder = "iPad" if "iPad" in ua else "iPhone" if "iPhone" in ua else "Phone"

        return render_template_string(NICKNAME_PROMPT_HTML, placeholder=placeholder)

    @app.route("/admin/remote/protected_check")
    def remote_protected_check():  # type: ignore[no-redef]
        """Debug endpoint: verify the mdb_remote cookie and return device_id."""
        cookie = request.cookies.get(remote_auth.COOKIE_NAME, "")
        device_id = remote_auth.verify_cookie(cookie)
        if device_id is None:
            return error_response("unpaired", "Not paired", status=401)
        return success_response({"device_id": device_id})

    @app.route("/static/<path:filename>")
    def serve_static(filename):  # type: ignore[no-redef]
        """Serve static assets.

        Use send_from_directory (not send_file with `static_dir / filename`)
        so Flask's safe_join enforces containment — without it, a request
        like /static/../../config/settings.json escapes the static dir and
        discloses arbitrary process-readable files.
        """
        static_dir = Path(__file__).parent / "static"
        return send_from_directory(static_dir, filename)

    return app

