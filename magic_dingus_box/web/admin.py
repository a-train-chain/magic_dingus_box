from __future__ import annotations

import collections
import io
import ipaddress
import json
import posixpath
from urllib.parse import urlsplit
import socket
import os
import re
import subprocess
import sys

# admin.py is imported both as a package member and as a flat module (the test
# harness does the latter), so sibling imports need the same dual form the
# remote/* imports below use.
try:  # noqa: E402
    from storage_prepare import (
        PROTECTED_MOUNTPOINTS,
        eligible_devices,
        protected_disk_names,
    )
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from .storage_prepare import (
        PROTECTED_MOUNTPOINTS,
        eligible_devices,
        protected_disk_names,
    )
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
    from remote.text_input_writer import TextInputWriter
except ImportError:
    from .remote import auth as remote_auth
    from .remote import devices as remote_devices
    from .remote import ws_handler
    from .remote.uinput_writer import UinputWriter
    from .remote.text_input_writer import TextInputWriter


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


class _ExtractTooLarge(Exception):
    """Internal signal: a ZIP entry would push extraction past the byte cap.

    Raised from inside the streaming loop so the staging temp file is cleaned up
    by one handler rather than at each bail-out point. Never escapes the import
    endpoint.
    """


def _atomic_write_text(path: Path, content: str, encoding: str = "utf-8",
                       mode: int = 0o644) -> None:
    """Write `content` to `path` so an interrupted write cannot corrupt it.

    This matters more here than in an ordinary web app. The Magic Dingus Box is
    a plug-it-in appliance with no shutdown ritual — it is powered off by pulling
    the cord, routinely, and that can land in the middle of a save. A bare
    Path.write_text() truncates the target and *then* streams into it, so losing
    power part-way leaves a half-written playlist on the SD card.

    The failure is silent, which is what makes it nasty: playlist_loader.cpp
    parses each file in a try/catch and only logs to stderr on a parse error
    (magic_dingus_box_cpp/src/app/playlist_loader.cpp), so a truncated playlist
    does not surface an error anywhere the user can see — the playlist simply
    stops appearing on the kiosk.

    The sequence:
      1. Write to a temp file in the SAME directory. os.replace is only atomic
         within one filesystem, so /tmp is not a valid staging area.
      2. fsync the file, so the bytes are actually on the card before anything
         points at them.
      3. os.replace onto the target — atomic, so a reader sees either the whole
         old file or the whole new one, never a mixture.
      4. fsync the *directory*, so the rename itself survives power loss. This
         is the step most implementations omit; without it the rename can still
         be lost even though the data was synced.

    Args:
        path: Destination file.
        content: Text to write.
        encoding: Text encoding.
        mode: Permission bits. mkstemp creates 0600, which would make files
            unreadable to other accounts; 0644 matches what write_text produced
            under the default umask, so behaviour is unchanged for readers.
    """
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)

    fd, tmp_name = tempfile.mkstemp(
        dir=str(path.parent), prefix=f".{path.name}.", suffix=".tmp"
    )
    tmp_path = Path(tmp_name)
    try:
        with os.fdopen(fd, "w", encoding=encoding) as fh:
            fh.write(content)
            fh.flush()
            os.fsync(fh.fileno())
        os.chmod(tmp_path, mode)
        os.replace(tmp_path, path)
    except BaseException:
        # Never leave the staging file behind on failure — the directory these
        # land in is scanned for playlists, and the dot-prefix plus .tmp suffix
        # keeps a stray one from being mistaken for content even if unlink fails.
        try:
            tmp_path.unlink()
        except OSError:
            pass
        raise

    # Durability of the rename. Best-effort: some filesystems refuse to open a
    # directory for fsync, and failing to sync is not a reason to fail a save
    # that has already landed.
    try:
        dir_fd = os.open(str(path.parent), os.O_RDONLY)
        try:
            os.fsync(dir_fd)
        finally:
            os.close(dir_fd)
    except OSError:
        pass


# ===== TMDB API KEY =====
#
# The kiosk's Media Browser discovers movies through TMDB. Without a key,
# every Browse and Search screen on the TV is blank — downloads still work,
# but the entire discovery surface is dead. first_boot.sh deliberately wipes
# the developer's personal key from every cloned unit (it is one person's
# key and rate limits are per key), so a shipped box has NO key and no way
# to get one short of SSH. These helpers back the Content Manager field that
# closes that gap.
#
# The file this writes is the one the kiosk already reads — see
# magic_dingus_box_cpp/src/main.cpp, which checks $MDB_TMDB_API_KEY first
# and otherwise reads $HOME/.config/magic_dingus_box/tmdb_api_key. No C++
# change is needed to make the key land.

# TMDB v3 API Key: 32 hex characters. This is the ONLY form the kiosk can
# use. TmdbClient interpolates the key into `?api_key=` on every request
# (magic_dingus_box_cpp/src/media_browser/tmdb_client.cpp — search_movie,
# get_movie, get_popular, ... all build the URL that way) and its http_get
# sets no CURLOPT_HTTPHEADER at all. A v4 "Read Access Token" only
# authenticates via an `Authorization: Bearer` header, so passing one here
# would produce a 401 on every kiosk call and the exact blank Browse screen
# this feature exists to prevent. We therefore reject v4 tokens at entry
# with an explanation rather than accepting them and failing silently.
_TMDB_V3_KEY_RE = re.compile(r"^[0-9a-fA-F]{32}$")

# v4 Read Access Tokens are JWTs: three base64url segments, and because the
# header is always {"alg":..,"typ":"JWT"} they begin "eyJ". Matched only so
# we can give a specific error, never to accept.
_TMDB_V4_TOKEN_RE = re.compile(r"^eyJ[A-Za-z0-9_-]{4,}\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+$")

_TMDB_VERIFY_URL = "https://api.themoviedb.org/3/authentication"
_TMDB_KEY_FILE_ENV = "MDB_TMDB_KEY_FILE"

KIOSK_SERVICE = "magic-dingus-box-cpp.service"


def _protected_disks() -> set:
    """Whole disks backing the running system — never offered for formatting.

    Derived live from findmnt rather than hardcoded, because the boot device
    differs across boards (mmcblk0 on SD, nvme0n1 on an NVMe hat, sda on USB
    boot). An empty return means we could not establish what the system runs
    from, and every caller treats that as fatal rather than proceeding.
    """
    sources = []
    for mountpoint in PROTECTED_MOUNTPOINTS:
        try:
            out = subprocess.run(["findmnt", "-no", "SOURCE", mountpoint],
                                 capture_output=True, text=True, timeout=10)
            if out.returncode == 0:
                sources.append(out.stdout.strip())
        except Exception:  # noqa: BLE001 - a missing mount is not an error
            continue
    return protected_disk_names(sources)


def _mountpoints_under(disk_path: str) -> list:
    """Every mountpoint currently served by this disk or its partitions.

    Used to clear a drive before formatting it. Anything still mounted makes
    wipefs fail with "device is busy", which is an unhelpful way for the
    Prepare Drive flow to end.
    """
    out = []
    try:
        result = subprocess.run(["lsblk", "-nro", "MOUNTPOINT", disk_path],
                                capture_output=True, text=True, timeout=15)
        if result.returncode != 0:
            return []
        for line in result.stdout.splitlines():
            mountpoint = line.strip()
            # Never hand a system path to umount, whatever lsblk reports. The
            # eligibility rule should already have excluded such a disk; this
            # is the same belt-and-braces reasoning applied one layer down.
            if mountpoint and mountpoint not in PROTECTED_MOUNTPOINTS:
                out.append(mountpoint)
    except Exception:  # noqa: BLE001
        return []
    return out


def _first_partition_of(disk_path: str):
    """The first partition node of a disk, once udev has created it.

    Naming differs by device class — sda -> sda1, but mmcblk0 -> mmcblk0p1 and
    nvme0n1 -> nvme0n1p1 — so this asks lsblk rather than concatenating.
    """
    try:
        out = subprocess.run(["lsblk", "-J", "-o", "NAME,TYPE", disk_path],
                             capture_output=True, text=True, timeout=15)
        if out.returncode != 0:
            return None
        for device in (json.loads(out.stdout).get("blockdevices") or []):
            for child in (device.get("children") or []):
                if child.get("type") == "part":
                    return "/dev/" + child["name"]
    except Exception:  # noqa: BLE001
        return None
    return None

# Systems whose libraries contain multi-disc titles, so an upload should
# re-run the .m3u generator over that system's ROM directory. Cartridge
# systems are excluded — there is nothing to swap, and scanning them is a
# pointless subprocess on every upload.
MULTI_DISC_SYSTEMS = frozenset({"ps1", "dreamcast"})


def _kiosk_started_at() -> Optional[float]:
    """Wall-clock epoch when the kiosk unit last became active, or None.

    Used to answer "is the running kiosk using the key that is on disk?" —
    the kiosk reads the key only at startup, so a key file newer than the
    process means the process is stale.

    Clock-jump note: the Pi has no RTC, so at boot the clock is stale and NTP
    steps it forward afterwards. systemd records ActiveEnterTimestamp at the
    moment of start and never revises it, so after such a step the recorded
    start looks EARLIER than it really was. That biases the comparison toward
    "stale" — i.e. toward telling the operator a restart is needed when it
    might not be. That is the safe direction: a needless restart prompt is
    recoverable, a silently-empty Browse screen is the bug being fixed.
    """
    try:
        result = subprocess.run(
            ["systemctl", "show", KIOSK_SERVICE, "-p", "ActiveEnterTimestamp",
             "--value"],
            capture_output=True, text=True, timeout=5,
        )
        raw = (result.stdout or "").strip()
        if not raw:
            return None
        # systemd emits e.g. "Tue 2026-07-28 19:07:19 PDT". Drop the leading
        # weekday and let the platform parse the rest.
        parts = raw.split(" ", 1)
        stamp = parts[1] if len(parts) > 1 else raw
        for fmt in ("%Y-%m-%d %H:%M:%S %Z", "%Y-%m-%d %H:%M:%S"):
            try:
                return datetime.strptime(stamp.strip(), fmt).timestamp()
            except ValueError:
                continue
        return None
    except Exception:
        return None


def _tmdb_key_file() -> Path:
    """Path of the key file the kiosk reads.

    Defaults to the kiosk's own lookup path. $MDB_TMDB_KEY_FILE overrides it
    so tests can write somewhere disposable — the web process must never be
    made to write into a real home directory during a test run.
    """
    override = os.getenv(_TMDB_KEY_FILE_ENV)
    if override:
        return Path(override)
    return Path.home() / ".config" / "magic_dingus_box" / "tmdb_api_key"


def _tmdb_redact(text: str, secret: str) -> str:
    """Strip `secret` out of a message before it can reach a log or a client.

    Belt-and-braces. Nothing here is *supposed* to put the key in an error
    string, but a library exception that happens to carry the request URL
    would leak it into a JSON response, and API keys have leaked out of this
    project before.
    """
    if not secret:
        return text
    return text.replace(secret, "<redacted>")


def _tmdb_classify_key(raw: str) -> tuple[str, str]:
    """Classify a user-supplied key. Returns (kind, normalized).

    kind is one of:
      "v3"      usable — 32 hex chars
      "v4"      well-formed Read Access Token, but the kiosk cannot use it
      "empty"   nothing supplied
      "invalid" anything else
    """
    normalized = (raw or "").strip()
    if not normalized:
        return ("empty", "")
    if _TMDB_V3_KEY_RE.match(normalized):
        return ("v3", normalized)
    if _TMDB_V4_TOKEN_RE.match(normalized):
        return ("v4", normalized)
    return ("invalid", normalized)


def _tmdb_verify_key(api_key: str, timeout: float = 10.0) -> tuple[str, str]:
    """Ask TMDB whether this key actually works. Returns (result, detail).

    result is one of:
      "valid"        TMDB accepted it
      "invalid"      TMDB rejected it (401) — the key is wrong or revoked
      "unreachable"  we could not get an answer (no internet, DNS down,
                     TMDB outage, rate limit). NOT evidence either way.

    The distinction matters: "invalid" is a reason to refuse the save,
    "unreachable" is not — a box mid-setup may have no working DNS yet, and
    refusing outright would strand the customer. The caller decides.

    Never raises, and never lets the key into `detail`.
    """
    import urllib.error
    import urllib.request
    from urllib.parse import urlencode

    url = f"{_TMDB_VERIFY_URL}?{urlencode({'api_key': api_key})}"
    req = urllib.request.Request(url, headers={"User-Agent": "MagicDingusBox/1.0"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            if resp.status == 200:
                return ("valid", "")
            return ("unreachable", f"TMDB returned HTTP {resp.status}")
    except urllib.error.HTTPError as e:
        if e.code == 401:
            # TMDB puts a human-readable reason in the body; surface it so a
            # revoked key reads differently from a mistyped one.
            detail = "TMDB rejected this key"
            try:
                payload = json.loads(e.read().decode("utf-8", "replace"))
                if payload.get("status_message"):
                    detail = str(payload["status_message"])
            except Exception:
                pass
            return ("invalid", _tmdb_redact(detail, api_key))
        if e.code == 429:
            return ("unreachable", "TMDB is rate-limiting this box; try again shortly")
        return ("unreachable", f"TMDB returned HTTP {e.code}")
    except Exception as e:
        # URLError, socket.timeout, ssl errors, anything else. Report the
        # exception TYPE only — the message could in principle echo the
        # request URL, which contains the key.
        return ("unreachable", f"Could not reach TMDB ({type(e).__name__})")


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


def _is_within(child, parent) -> bool:
    """True iff `child` is `parent` itself or a descendant of it.

    Uses the resolved-path parent relationship, NOT a string prefix.
    `str(x).startswith(str(y))` is a classic CWE-22 containment bypass:
    "/data/media_backup/secret" startswith "/data/media" is True even
    though media_backup is a SIBLING of media, not inside it — so a
    crafted <path:...> could reach files outside the intended tree the
    moment any such sibling directory exists. Path.is_relative_to()
    (Py3.9+) compares path components, so siblings never match."""
    try:
        child = Path(child).resolve()
        parent = Path(parent).resolve()
        return child == parent or parent in child.parents
    except Exception:
        return False


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


NICKNAME_PROMPT_HTML = """
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover, maximum-scale=1, user-scalable=no">
<!-- Matches the faceplate's BOTTOM edge (#131013), not --bg. iOS paints the
     region outside the web view with theme-color, and on a standalone launch
     that region is real: the layout viewport comes up ~59px short of the
     screen. The strip cannot be drawn into (only the root BACKGROUND
     propagates past the viewport, not content or borders), so the only way to
     hide the seam is to make iOS paint it the same colour the faceplate ends
     on. The bottom radial gradient darkens #1F191F to #131013 there. -->
<meta name="theme-color" content="#131013">
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


# --- VPN provider support -------------------------------------------------
#
# Everything below was verified empirically against the exact image this box
# runs (`qmcgaw/gluetun:v3` == v3.41.1) by launching throwaway containers and
# reading gluetun's own settings-validation errors. Do not "simplify" these
# rules from memory — each one corresponds to a FATAL gluetun startup error,
# and gluetun failing to start takes the whole media stack with it (radarr /
# prowlarr / qbittorrent / byparr are all `depends_on: service_healthy`).

# Default WireGuard listen port, used when a .conf's Endpoint omits one.
_WG_DEFAULT_ENDPOINT_PORT = 51820

# Gluetun's `custom` provider runs ANY standard WireGuard config. It needs
# exactly what a .conf already contains, so it is our universal fallback.
_VPN_PROVIDER_CUSTOM = "custom"

# Providers gluetun v3.41.1 accepts for VPN_PORT_FORWARDING=on. Verbatim from
# its own error text:
#   "port forwarding cannot be enabled: value is not one of the possible
#    choices: mullvad must be one of perfect privacy, private internet
#    access, privatevpn or protonvpn"
# Setting VPN_PORT_FORWARDING=on for anything else is a HARD startup failure,
# not a warning — including for `custom`.
_VPN_PORT_FORWARDING_PROVIDERS = frozenset({
    "perfect privacy",
    "private internet access",
    "privatevpn",
    "protonvpn",
})

# Of the four above, only protonvpn actually has WireGuard servers in
# gluetun's embedded server list (checked against /gluetun/servers.json:
# perfect privacy 0, private internet access 0, privatevpn 0, protonvpn 800).
# So over WireGuard — which is all this box supports — ProtonVPN is the only
# provider that can ever forward a port.
_VPN_WIREGUARD_NATIVE_PROVIDERS = frozenset({
    "airvpn", "fastestvpn", "ivpn", "mullvad",
    "nordvpn", "protonvpn", "surfshark", "windscribe",
})

# Providers we are willing to select NATIVELY on our own (i.e. from detection
# alone, with no operator confirmation). Native mode hands server choice to
# gluetun, which is only worth the extra failure surface where it unlocks
# something we need — and the only thing it unlocks here is port forwarding.
# Everything else detects to a friendly label but still RUNS as `custom`.
_VPN_AUTO_NATIVE_PROVIDERS = frozenset({"protonvpn"})


def _parse_wireguard_endpoint(endpoint: str) -> tuple[str, int]:
    """Split a WireGuard `Endpoint` into (host, port).

    Host may be an IPv4 address, an IPv6 address or a DNS name — callers that
    hand it to gluetun must resolve names first (see
    _resolve_wireguard_endpoint_ip). Port falls back to the WireGuard default
    when the endpoint omits it.
    """
    endpoint = endpoint.strip()
    port = _WG_DEFAULT_ENDPOINT_PORT
    if endpoint.startswith("["):
        # Bracketed IPv6: "[2001:db8::1]:51820"
        host, _, rest = endpoint[1:].partition("]")
        if rest.startswith(":") and rest[1:].isdigit():
            port = int(rest[1:])
    else:
        head, sep, tail = endpoint.rpartition(":")
        # "host:port" — but an unbracketed IPv6 literal is also full of
        # colons, and splitting one on its last colon would silently invent a
        # port from the final hextet. Only treat the tail as a port when what
        # is left is a single colon-free host.
        if sep and tail.isdigit() and head and ":" not in head:
            host, port = head, int(tail)
        else:
            host = endpoint
    return host.strip(), port


def _wireguard_sections(text: str) -> tuple[dict, dict]:
    """Split a WireGuard .conf into its ([Interface], [Peer]) key/value maps."""
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
    return interface, peer


def _parse_wireguard_config(text: str) -> dict:
    """Parse a WireGuard .conf file into the vars Gluetun needs.

    Returns a dict with keys:
      WIREGUARD_PRIVATE_KEY, WIREGUARD_ADDRESSES,
      WIREGUARD_PUBLIC_KEY,  WIREGUARD_ENDPOINT_IP,
      WIREGUARD_ENDPOINT_PORT
    Raises ValueError on missing/malformed fields.

    Every key returned here is written straight to services/.env, so nothing
    that is not a real gluetun variable belongs in this dict.

    WIREGUARD_ENDPOINT_PORT is not optional trivia: gluetun's `custom`
    provider refuses to start without it ("server selection: Wireguard server
    selection settings: endpoint port is not set"). The port used to be
    discarded here, which is fine only for native providers that carry their
    own server list.
    """
    interface, peer = _wireguard_sections(text)

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

    endpoint_ip, endpoint_port = _parse_wireguard_endpoint(peer["Endpoint"])
    if not endpoint_ip:
        raise ValueError("Could not parse host from [Peer] Endpoint")

    # ProtonVPN configs list dual-stack addresses
    # ("10.2.0.2/32, 2a07:b944::2:2/128"). Gluetun's container has no
    # IPv6 and hard-fails on the IPv6 entry, crash-looping the stack —
    # hit live on the first Pi 5 provisioning (2026-07-22). Keep IPv4 only.
    addresses = [a.strip() for a in interface["Address"].split(",")]
    ipv4_addresses = [a for a in addresses if a and ":" not in a]
    if not ipv4_addresses:
        raise ValueError(
            "[Interface] Address has no IPv4 entry (IPv6-only configs are "
            "not supported by the Gluetun container)")

    return {
        "WIREGUARD_PRIVATE_KEY": interface["PrivateKey"],
        "WIREGUARD_ADDRESSES": ", ".join(ipv4_addresses),
        "WIREGUARD_PUBLIC_KEY": peer["PublicKey"],
        "WIREGUARD_ENDPOINT_IP": endpoint_ip,
        "WIREGUARD_ENDPOINT_PORT": str(endpoint_port),
    }


def _detect_vpn_brand(text: str, wg: dict | None = None) -> str:
    """Best-effort brand ID for an uploaded WireGuard config.

    Returns a gluetun provider string, or "" when nothing matches. This is a
    LABEL — it does not by itself decide how the tunnel is run; see
    _vpn_provider_env. Detection is deliberately conservative: an unknown
    config falls through to "" and therefore to gluetun's `custom` provider,
    which runs any standard WireGuard file.

    Signature sources, and how far each is actually trusted:
      * Endpoint hostname suffixes are taken from gluetun's own embedded
        server list (/gluetun/servers.json in the running image), so they are
        exact for the providers gluetun knows. They only help when the .conf
        carries a hostname — ProtonVPN and Mullvad emit a bare IP instead.
      * ProtonVPN's 10.2.0.2 interface address + 10.2.0.1 DNS gateway is
        corroborated three ways: a real Proton config; this repo's own
        operational notes (the FIREWALL_OUTBOUND_SUBNETS comment in
        docker-compose.yml documents the 10.2.0.1 gateway as the reason
        10.0.0.0/8 must not be listed); and gluetun itself, which hardcodes
        10.2.0.2 as its ProtonVPN default address.
      * The remaining subnet rules are only applied where the range is
        distinctive enough that a self-hosted tunnel is unlikely to collide
        with it. IVPN (172.16.0.0/12) is deliberately NOT matched on subnet:
        that is the most commonly self-chosen private range there is, and
        IVPN configs carry a hostname endpoint anyway.

    A wrong label here is cosmetic, never functional: every brand except
    protonvpn still RUNS as `custom` (see _VPN_AUTO_NATIVE_PROVIDERS), and
    the operator can override the choice in the setup panel.
    """
    interface, peer = _wireguard_sections(text)
    if wg and wg.get("WIREGUARD_ENDPOINT_IP"):
        host = wg["WIREGUARD_ENDPOINT_IP"].strip().lower()
    else:
        host, _ = _parse_wireguard_endpoint(peer.get("Endpoint", ""))
        host = host.lower()
    address = (interface.get("Address") or "").strip().lower()
    dns = (interface.get("DNS") or "").strip().lower()

    def _in(value: str, cidr: str) -> bool:
        """True iff `value` (an Address or DNS entry) sits inside `cidr`."""
        first = value.split(",")[0].strip().split("/")[0].strip()
        if not first:
            return False
        try:
            return ipaddress.ip_address(first) in ipaddress.ip_network(cidr)
        except ValueError:
            return False

    # 1. Endpoint hostname — the strongest signal when present.
    host_suffixes = (
        (".protonvpn.net", "protonvpn"),
        (".mullvad.net", "mullvad"),
        (".wg.ivpn.net", "ivpn"),
        (".ivpn.net", "ivpn"),
        (".vpn.airdns.org", "airvpn"),
        (".airvpn.org", "airvpn"),
        (".nordvpn.com", "nordvpn"),
        (".prod.surfshark.com", "surfshark"),
        (".surfshark.com", "surfshark"),
        (".whiskergalaxy.com", "windscribe"),
        (".windscribe.com", "windscribe"),
        (".jumptoserver.com", "fastestvpn"),
    )
    for suffix, brand in host_suffixes:
        if host.endswith(suffix):
            return brand

    # 2. ProtonVPN — two of three: the 10.2.0.x address, the 10.2.0.1 DNS
    #    gateway, or one of the "# Key for" / "# NetShield" / "# NAT-PMP" /
    #    "# VPN Accelerator" headers its dashboard writes. These co-occur;
    #    they are not alternatives, so any one of them counts once.
    proton_hits = 0
    if _in(address, "10.2.0.0/24"):
        proton_hits += 1
    if _in(dns, "10.2.0.1/32"):
        proton_hits += 1
    if re.search(r"^\s*#\s*(Key for\s+\S|NetShield|NAT-PMP|VPN Accelerator)",
                 text, re.MULTILINE | re.IGNORECASE):
        proton_hits += 1
    if proton_hits >= 2:
        return "protonvpn"

    # 3. Surfshark writes a byte-identical `Address = 10.14.0.2/16` for every
    #    user and every server — the /16 on a single-peer client config is
    #    itself unusual enough to be a signature.
    if address.startswith("10.14.0.2/16"):
        return "surfshark"

    # 4. Mullvad: 10.64.0.0/10 address AND the 10.64.0.1 DNS gateway. The
    #    address is NOT fixed at 10.64.0.x — real ones include 10.69.209.105
    #    and 10.71.237.120 — so the whole /10 has to be checked.
    if _in(address, "10.64.0.0/10") and _in(dns, "10.64.0.1/32"):
        return "mullvad"

    # 5. AirVPN: 10.128.0.0/9 address with the 10.128.0.1 DNS gateway.
    if _in(address, "10.128.0.0/9") and _in(dns, "10.128.0.1/32"):
        return "airvpn"

    # 6. Windscribe: CGNAT-range address (100.64.0.0/10) with a 10.255.255.x
    #    resolver. Its hostname rule above covers the usual case.
    if _in(address, "100.64.0.0/10") and _in(dns, "10.255.255.0/24"):
        return "windscribe"

    return ""


def _vpn_provider_env(provider: str, wg: dict, country: str = "") -> dict:
    """Return the VPN_*/WIREGUARD_ENDPOINT_* env block for a chosen provider.

    `provider` is a gluetun VPN_SERVICE_PROVIDER value; anything not natively
    supported over WireGuard is coerced to `custom`. The returned dict is
    applied AUTHORITATIVELY (it overwrites whatever the .env had), because
    these keys have to stay mutually consistent with the uploaded config —
    a stale value from a previous provider is exactly what breaks the tunnel.

    Three rules here are load-bearing, each verified against gluetun v3.41.1
    by reading its startup validation:

    1. VPN_PORT_FORWARDING=on is only legal for the four providers in
       _VPN_PORT_FORWARDING_PROVIDERS. For anything else — `custom` included
       — gluetun exits with "port forwarding cannot be enabled". Because the
       rest of the stack gates on `depends_on: service_healthy`, that is a
       whole-stack outage, not a degraded tunnel.

    2. A non-empty SERVER_COUNTRIES with provider `custom` is likewise fatal:
       "for VPN service provider custom: the country specified is not valid:
       one or more values is set but there is no possible value available".
       Custom has no server list to filter, so the country must be blank.

    3. The endpoint pin is only written for `custom`. For a native provider
       over WireGuard, gluetun filters its server list FIRST and then scans
       the filtered pool for the pinned IP (internal/provider/utils/pick.go)
       — so the pin does not skip the port-forwarding-only filter, it
       collapses the pool to exactly one server. That is why this box stayed
       on a single server across every reconnect and could not move to a
       working one; and if the pinned server ever drops out of the filtered
       set, gluetun fails outright with "target IP address not found: in %d
       filtered connections". Native providers get the pin explicitly
       BLANKED so re-provisioning clears one written by an earlier version.
       (Native WireGuard providers also reject an endpoint PORT outright —
       it must be unset for protonvpn/nordvpn/surfshark/fastestvpn, and is
       restricted to a fixed allow-list for airvpn/ivpn/windscribe — so
       blanking both keys is the only safe native shape.)
    """
    if provider not in _VPN_WIREGUARD_NATIVE_PROVIDERS:
        provider = _VPN_PROVIDER_CUSTOM

    env = {
        "VPN_SERVICE_PROVIDER": provider,
        "VPN_TYPE": "wireguard",
        "VPN_PORT_FORWARDING":
            "on" if provider in _VPN_PORT_FORWARDING_PROVIDERS else "off",
    }

    if provider == _VPN_PROVIDER_CUSTOM:
        env["VPN_COUNTRIES"] = ""            # rule 2
        env["WIREGUARD_ENDPOINT_IP"] = wg.get("WIREGUARD_ENDPOINT_IP", "")
        env["WIREGUARD_ENDPOINT_PORT"] = wg.get(
            "WIREGUARD_ENDPOINT_PORT", str(_WG_DEFAULT_ENDPOINT_PORT))
    else:
        # Country filtering is only offered for ProtonVPN. Other native
        # providers are left unfiltered on purpose: gluetun's windscribe
        # WireGuard servers carry no country field at all, so a country here
        # would trip the same "no possible value available" fatal as rule 2.
        env["VPN_COUNTRIES"] = country if provider == "protonvpn" else ""
        env["WIREGUARD_ENDPOINT_IP"] = ""    # rule 3
        env["WIREGUARD_ENDPOINT_PORT"] = ""

    return env


def _vpn_supports_port_forwarding(provider: str) -> bool:
    """True iff gluetun can lease a forwarded port for this provider."""
    return provider in _VPN_PORT_FORWARDING_PROVIDERS


# Display names for the setup panel's provider dropdown. Keys are gluetun's
# own VPN_SERVICE_PROVIDER strings and must stay exact.
_VPN_PROVIDER_LABELS = {
    _VPN_PROVIDER_CUSTOM: "Other / self-hosted (works with any WireGuard config)",
    "protonvpn": "ProtonVPN",
    "airvpn": "AirVPN",
    "fastestvpn": "FastestVPN",
    "ivpn": "IVPN",
    "mullvad": "Mullvad",
    "nordvpn": "NordVPN",
    "surfshark": "Surfshark",
    "windscribe": "Windscribe",
}


def _vpn_provider_choices() -> list[dict]:
    """Provider options for the setup panel, custom first.

    `custom` leads because it is the right answer for almost everyone: it
    runs any standard WireGuard config, so it is the option that cannot be
    wrong. The native entries below it only change anything for operators who
    want gluetun picking servers for them.
    """
    ordered = [_VPN_PROVIDER_CUSTOM] + sorted(_VPN_WIREGUARD_NATIVE_PROVIDERS)
    return [
        {
            "value": p,
            "label": _VPN_PROVIDER_LABELS.get(p, p),
            "port_forwarding": _vpn_supports_port_forwarding(p),
        }
        for p in ordered
    ]


def _resolve_wireguard_endpoint_ip(host: str) -> str:
    """Resolve a WireGuard endpoint host to a literal IPv4 address.

    Gluetun rejects a hostname outright — "environment variable
    WIREGUARD_ENDPOINT_IP: ParseAddr(...): unexpected character ... note this
    MUST be an IP address" — and several providers ship configs whose
    Endpoint is a DNS name, so resolving here is what makes those configs
    usable at all. Returns the input unchanged when it is already an IP.
    Raises ValueError when a name cannot be resolved.
    """
    host = (host or "").strip()
    if not host:
        raise ValueError("empty WireGuard endpoint host")
    try:
        ipaddress.ip_address(host)
        return host
    except ValueError:
        pass
    try:
        return socket.gethostbyname(host)
    except Exception as exc:
        raise ValueError(
            f"Could not resolve WireGuard endpoint host {host!r} to an IP "
            f"address ({exc}). Gluetun requires a literal IP.") from exc


def create_app(data_dir: Path, config=None) -> Flask:
    app = Flask(__name__)

    # flask-sock for the Phone Remote WebSocket.
    try:
        from flask_sock import Sock
    except ImportError:
        Sock = None
    # WS keepalive: ping every 25s so a phone that vanishes without a clean
    # close (WiFi drop, screen lock, walked out of range) gets its connection
    # torn down within ~one interval instead of lingering half-open. Without
    # this, the dead socket's worker thread — and any button the phone was
    # holding — stuck around until kernel TCP keepalive gave up (hours).
    # flask-sock reads ping_interval from SOCK_SERVER_OPTIONS (not a Sock()
    # constructor kwarg in 0.7.x), so set it BEFORE constructing Sock.
    app.config.setdefault("SOCK_SERVER_OPTIONS", {"ping_interval": 25})
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
        # mode=0600 from the moment it is visible. Writing then chmod-ing left a
        # window in which the HMAC signing secret existed at the default 0644 —
        # and this is the key that authenticates every paired phone. mkstemp
        # creates at 0600 and the file only appears under its real name via the
        # rename, so there is no such window here.
        _atomic_write_text(secret_path, new_secret, mode=0o600)
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

    # Eagerly construct the UinputWriter so /dev/input/eventN exists at
    # boot — the kiosk's InputManager only scans /dev/input/event* once
    # at startup, so a lazily-created virtual gamepad would never be seen
    # by the kiosk until it restarted. Failure here is non-fatal (uinput
    # may be missing in dev environments); WS connect will then fail
    # gracefully with a 'uinput_unavailable' error to the phone.
    try:
        app.config["UINPUT_WRITER"] = UinputWriter()
    except Exception as e:
        import warnings
        warnings.warn(f"Phone Remote: failed to open /dev/uinput at startup: {e}", RuntimeWarning)
        app.config["UINPUT_WRITER"] = None

    # Construct TextInputWriter for phone-remote keyboard input. The kiosk
    # drains the same queue file each frame via config::get_data_path().
    app.config["TEXT_INPUT_WRITER"] = TextInputWriter(
        queue_path=data_dir / "text_input_queue.jsonl"
    )

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

    # Sweep leftovers from previous runs. Transcode inputs, probe files,
    # import staging ZIPs and ffmpeg-stderr temps all rely on in-process
    # cleanup by daemon worker threads whose job state is in-memory only —
    # a service restart, OOM kill, or power cut mid-job orphaned them here
    # permanently (a single interrupted upload can strand up to
    # MAX_CONTENT_LENGTH bytes on the SD card with nothing in any UI
    # pointing at the cause). No upload or transcode survives a restart,
    # so at this point in startup nothing in the directory can be live.
    _swept_bytes = 0
    for _leftover in upload_temp_dir.iterdir():
        try:
            if _leftover.is_dir() and not _leftover.is_symlink():
                _swept_bytes += sum(
                    f.stat().st_size
                    for f in _leftover.rglob("*") if f.is_file())
                shutil.rmtree(_leftover, ignore_errors=True)
            else:
                _swept_bytes += _leftover.stat().st_size
                _leftover.unlink()
        except OSError:
            pass  # vanished mid-sweep or unreadable — not worth failing startup
    if _swept_bytes:
        print(f"[upload_temp] swept {_swept_bytes / (1024 * 1024):.1f} MB "
              "of orphaned upload/transcode temp files from previous runs",
              flush=True)

    
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
                            safe_name = _canonical_playlist_name(
                                _sanitize_filename(playlist_name, allowed_extensions=['.yaml', '.yml']))
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

                        kiosk_config_dir.mkdir(parents=True, exist_ok=True)
                        settings_dest = kiosk_config_dir / "settings.json"
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

                    # Security check: ensure we're deleting within allowed directories
                    for candidate in [video_path, dev_video_path]:
                        if candidate.exists() and candidate.is_file():
                            candidate_resolved = candidate.resolve()
                            # Verify path is within data directories
                            data_parent = data_dir.parent.resolve()
                            if _is_within(candidate_resolved, data_parent):
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
                safe_name = _canonical_playlist_name(
                    _sanitize_filename(output_name, allowed_extensions=['.yaml', '.yml']))
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

                    # Guard against ZIP bombs: limit total extracted size (default 10GB)
                    MAX_EXTRACT_BYTES = int(os.getenv("MAGIC_MAX_EXTRACT_MB", "10240")) * 1024 * 1024
                    total_extracted = 0

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
                            return error_response(
                                "VALIDATION_ERROR",
                                f"Package exceeds maximum extraction size ({MAX_EXTRACT_BYTES // (1024*1024)}MB)",
                                status=413
                            )
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
                            return error_response(
                                "VALIDATION_ERROR",
                                f"Package exceeds maximum extraction size ({MAX_EXTRACT_BYTES // (1024*1024)}MB)",
                                status=413
                            )
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

                item_count = len(playlist_data.get('items', []))
                print(f"Imported package: {output_name} ({item_count} items, "
                      f"{videos_imported} videos, {roms_imported} ROMs)", file=sys.stderr)

                return success_response(
                    data={
                        "playlist_filename": output_name,
                        "playlist_title": playlist_data.get('title', output_name),
                        "item_count": item_count,
                        "videos_imported": videos_imported,
                        "roms_imported": roms_imported,
                        "videos_skipped": videos_skipped,
                    },
                    message=(f"Package imported: {item_count} playlist items, "
                             f"{videos_imported} videos, {roms_imported} ROMs")
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
        if not _is_within(out_resolved, media_dir_resolved):
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

        if not any(_is_within(target_resolved, d) for d in allowed_dirs):
            return error_response("VALIDATION_ERROR", "Invalid path")

        if target.exists() and target.is_file():
            target.unlink()
            return success_response(message="File deleted")
        return error_response("NOT_FOUND", "File not found", status=404)

    # ===== VIDEO TRANSCODING =====

    # Resolution presets for transcoding.
    #
    # These are MASTERS, not display formats: the kiosk scales whatever is
    # stored to whichever display mode is active, so the right strategy is
    # to keep the best master that storage allows and let playback derive
    # the rest. Storing a display-resolution file is lossy in one direction
    # only — you can always go down, never back up.
    #
    # Aspect matters as much as resolution. The main kiosk renders playlist
    # video into a 4:3 viewport (gst_renderer letterbox: vp_w = canvas_h*4/3
    # -> 960x720 at 720p, 1440x1080 at 1080p), which is the deliberate
    # CRT look. A 16:9 master gets letterboxed INSIDE that pillarbox and
    # ends up smaller on screen, so 4:3 masters are correct for playlist
    # content even on a widescreen TV.
    #
    # 'crt_hd' is the default: 4:3 at 720p height. It is 2.25x the detail of
    # the old 640x480 default, lands 1:1 in the 4:3 area at 720p output,
    # upscales 1.5x at 1080p, and downscales cleanly for a real CRT through
    # the HDMI->composite converter (which is a downscale either way — the
    # Pi 5 has no composite out, so 640x480 was never a pixel-exact path).
    TRANSCODE_RESOLUTIONS = {
        # 4:3 masters — playlist content, both display modes
        'crt':     {'width': 640,  'height': 480},   # legacy/smallest
        'crt_hd':  {'width': 960,  'height': 720},   # DEFAULT
        'crt_fhd': {'width': 1440, 'height': 1080},  # max detail, ~2.25x the files
        # 16:9 master — only for genuinely widescreen source material
        'modern':  {'width': 1280, 'height': 720},
    }

    # Default master. Changing this affects NEW uploads only; existing
    # files are untouched and cannot regain detail they never had.
    DEFAULT_TRANSCODE = 'crt_hd'

    # Store for tracking transcoding jobs (in-memory, cleared on restart)
    transcode_jobs: dict = {}

    # Cap concurrent ffmpeg encodes. The Pi 4 has 4 cores / limited RAM
    # shared with the kiosk and (optionally) the Media Browser Docker stack;
    # one libx264 encode already saturates it. 2 lets a second upload start
    # transcoding while a first finishes without thrashing. Excess jobs block
    # in run_transcode_job() and report 'queued' to the UI.
    _TRANSCODE_SEMAPHORE = threading.Semaphore(
        int(os.getenv("MAGIC_MAX_TRANSCODES", "2")))

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
        res = TRANSCODE_RESOLUTIONS.get(resolution, TRANSCODE_RESOLUTIONS[DEFAULT_TRANSCODE])
        width, height = res['width'], res['height']

        # Bound concurrent encodes. Each transcode is a full libx264 encode;
        # on a Pi 4 sharing RAM/CPU with the kiosk (and possibly the Media
        # Browser Docker stack), several phones uploading at once would spawn
        # unbounded parallel ffmpeg processes and thrash/OOM. Block here until
        # a slot frees — the job sits in 'queued' rather than competing.
        job['status'] = 'queued'
        job['message'] = 'Queued — waiting for an encoder slot...'
        _TRANSCODE_SEMAPHORE.acquire()
        stderr_file = None

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

            # Run FFmpeg. Progress goes to stdout (`-progress pipe:1`), which
            # we parse below; the ffmpeg banner + decoder warnings/errors go
            # to stderr. We redirect stderr to a TEMP FILE rather than a pipe:
            # with a stderr PIPE that we only read after stdout drains, a
            # chatty stderr (iPhone HEVC/.MOV decode warnings routinely exceed
            # the ~64KB pipe buffer) blocks ffmpeg's write(), stdout stops
            # advancing, our readline() blocks, and the job hangs forever with
            # ffmpeg orphaned. A file has no such buffer limit, so it can't
            # deadlock; we read it back only if ffmpeg fails.
            stderr_file = tempfile.NamedTemporaryFile(
                mode='w+', suffix='.ffmpeg-stderr', delete=False)
            process = subprocess.Popen(
                ffmpeg_cmd,
                stdout=subprocess.PIPE,
                stderr=stderr_file,
                text=True
            )

            # Watchdog: kill a hung ffmpeg so it can't hold its encoder slot
            # (the _TRANSCODE_SEMAPHORE below) forever. A stalled decode on a
            # corrupt/partial upload would otherwise block the readline() loop
            # indefinitely — two such stalls exhaust the default 2-slot pool
            # and every later upload sits 'queued' until a service restart.
            # SIGKILL after the cap unblocks readline() (stdout closes) → the
            # job falls through to the non-zero-returncode error path.
            transcode_timeout_s = int(os.getenv("MAGIC_TRANSCODE_TIMEOUT_S", "3600"))
            watchdog = threading.Timer(transcode_timeout_s, process.kill)
            watchdog.daemon = True
            watchdog.start()

            try:
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

                process.wait()  # ensure returncode is set
            finally:
                watchdog.cancel()

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
                # Read ffmpeg's error output back from the stderr temp file.
                stderr_output = ''
                try:
                    stderr_file.seek(0)
                    stderr_output = stderr_file.read()
                except Exception:
                    pass
                job['status'] = 'error'
                job['message'] = f'FFmpeg failed: {stderr_output[-200:]}'

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
        finally:
            # Always remove the stderr temp file.
            if stderr_file is not None:
                try:
                    stderr_file.close()
                    os.unlink(stderr_file.name)
                except Exception:
                    pass
            _TRANSCODE_SEMAPHORE.release()

    @app.post("/admin/upload-and-transcode")
    @require_csrf
    def upload_and_transcode():  # type: ignore[no-redef]
        """Upload video file and transcode it on the Pi."""
        if "file" not in request.files:
            return error_response("VALIDATION_ERROR", "File field required")

        f = request.files["file"]
        resolution = request.form.get("resolution", DEFAULT_TRANSCODE)
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
        target = TRANSCODE_RESOLUTIONS.get(target_resolution, TRANSCODE_RESOLUTIONS[DEFAULT_TRANSCODE])
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
        resolution = request.form.get("resolution", DEFAULT_TRANSCODE)
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
        if not _is_within(out_resolved, roms_dir_resolved):
            return error_response("VALIDATION_ERROR", "Invalid path")

        out.parent.mkdir(parents=True, exist_ok=True)
        f.save(str(out))
        
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
                        script_path = data_dir.parent / "magic_dingus_box_cpp" / "scripts" / "generate_m3u_playlists.sh"
                        if script_path.exists():
                            subprocess.run(
                                [str(script_path), str(system_rom_dir)],
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

        recent_lines: "collections.deque[str]" = collections.deque(maxlen=20)
        try:
            # Run update script with install command.
            # stderr is MERGED into stdout (not a separate pipe): the progress
            # parser below already ignores any non-JSON line, so mixing the
            # script's stderr in is harmless — and it removes the classic
            # dual-pipe deadlock where a chatty stderr fills its 64KB buffer,
            # blocks the script's write(), and hangs our stdout read forever.
            # Use start_new_session=True so the process survives web restart.
            process = subprocess.Popen(
                [str(UPDATE_SCRIPT), "install", version, download_url],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,  # Line buffered
                start_new_session=True  # Detach from parent process group
            )

            # Read progress output line by line
            for line in process.stdout:
                line = line.strip()
                if line:
                    recent_lines.append(line)
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
                tail = "\n".join(recent_lines)
                job['status'] = 'error'
                job['message'] = tail[-500:] if tail else 'Update failed'

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

        # Validate the download URL. Pin it to THIS PROJECT's own repo, not
        # just "any github.com URL" — the old prefix check let any device on
        # the LAN POST a download_url pointing at an attacker-owned GitHub
        # repo and have the Pi fetch + install that tarball over itself
        # (update.sh runs with no signature check). Override via
        # MAGIC_GITHUB_REPO for forks.
        #
        # A plain string startswith() is NOT enough: curl (which update.sh
        # uses with -L) normalizes RFC-3986 dot-segments before the request,
        # so ".../a-train-chain/magic_dingus_box/../../attacker/repo/x.tar.gz"
        # would pass a prefix check yet fetch attacker/repo. Parse the URL and
        # compare the NORMALIZED path segments (same defense the _is_within
        # filesystem check uses), and match the host exactly.
        gh_repo = os.getenv("MAGIC_GITHUB_REPO", "a-train-chain/magic_dingus_box")
        # host → required leading path (normalized, no trailing slash yet)
        host_prefix = {
            "github.com":         f"/{gh_repo}",
            "codeload.github.com": f"/{gh_repo}",
            "api.github.com":     f"/repos/{gh_repo}",
        }
        parts = urlsplit(download_url)
        norm_path = posixpath.normpath(parts.path) if parts.path else ""
        expected = host_prefix.get(parts.netloc)
        url_ok = (
            parts.scheme == "https"
            and expected is not None
            # exact repo dir or a descendant path, post-normalization
            and (norm_path == expected or norm_path.startswith(expected + "/"))
        )
        if not url_ok:
            return error_response(
                "VALIDATION_ERROR",
                f"Invalid download URL (must be from the {gh_repo} GitHub repo)")

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
        lines = [f"{k}={v}" for k, v in env.items()]
        # Was already tmp+rename, but with no fsync: the rename could survive a
        # power cut while the contents behind it did not. This file holds the
        # WireGuard private key and the qBittorrent password.
        _atomic_write_text(path, "\n".join(lines) + "\n", mode=0o600)

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

    # ===== PREPARE DRIVE =====
    #
    # A customer's new drive is exFAT or NTFS with some arbitrary label, so it
    # does not match the `LABEL=MOVIES ... ext4` line in /etc/fstab and never
    # mounts. Worse, STORAGE_ROOT=/mnt/ssd is a plain directory on the SD card
    # when nothing is mounted there, so the stack comes up bound to the SD and
    # downloads land on the OS partition.
    #
    # ext4 is not something a customer can produce from Windows or macOS, so
    # the preparation has to happen on the box.
    #
    # Gated behind the SAME Layer 1 unlock as the rest of the Media Browser —
    # when the secret sequence has not been entered, these 403 exactly like
    # every other /admin/media-browser/* route and the UI renders nothing.
    # require_vpn=False because preparing storage is setup work that precedes
    # dropping in a WireGuard config, same as the other setup endpoints.

    @app.get("/admin/media-browser/storage/devices")
    def storage_devices():  # type: ignore[no-redef]
        gate = _check_media_browser_gates(require_vpn=False)
        if gate is not None:
            return gate
        try:
            listing = subprocess.run(
                ["lsblk", "-J", "-o", "NAME,TYPE,SIZE,RM,MOUNTPOINT,LABEL,FSTYPE"],
                capture_output=True, text=True, timeout=15, check=True).stdout
            protected = _protected_disks()
        except Exception as e:  # noqa: BLE001 - surfaced to the operator
            return error_response("STORAGE_ERROR", f"Could not enumerate disks: {e}")
        if not protected:
            # We could not establish what the system is running from. Refusing
            # is the only safe answer — an empty protected set would make the
            # boot disk eligible.
            return error_response(
                "STORAGE_ERROR",
                "Could not determine the system disk; refusing to list targets")
        return success_response(data={
            "devices": eligible_devices(json.loads(listing), protected),
            "mountpoint": "/mnt/ssd",
            "label": "MOVIES",
        })

    @app.post("/admin/media-browser/storage/prepare")
    @require_csrf
    def storage_prepare():  # type: ignore[no-redef]
        """ERASE a drive and set it up to hold the movie library.

        Destructive and irreversible. Three independent checks stand between a
        request and mkfs: the device must be in the eligible list computed
        fresh here (never trusted from the client), the caller must echo the
        device name back in `confirm`, and the eligibility rule itself refuses
        anything serving the running system.
        """
        gate = _check_media_browser_gates(require_vpn=False)
        if gate is not None:
            return gate

        body = request.get_json(silent=True) or {}
        device = str(body.get("device") or "").strip()
        confirm = str(body.get("confirm") or "").strip()
        if not device:
            return error_response("VALIDATION_ERROR", "device required")

        try:
            listing = subprocess.run(
                ["lsblk", "-J", "-o", "NAME,TYPE,SIZE,RM,MOUNTPOINT,LABEL,FSTYPE"],
                capture_output=True, text=True, timeout=15, check=True).stdout
            protected = _protected_disks()
        except Exception as e:  # noqa: BLE001
            return error_response("STORAGE_ERROR", f"Could not enumerate disks: {e}")
        if not protected:
            return error_response(
                "STORAGE_ERROR",
                "Could not determine the system disk; refusing to format anything")

        # Re-derive eligibility server-side. The client's idea of what is safe
        # is never trusted.
        allowed = {d["name"]: d for d in eligible_devices(json.loads(listing), protected)}
        target = allowed.get(device)
        if target is None:
            return error_response(
                "VALIDATION_ERROR",
                f"{device} is not a device this box may format", status=400)

        # Explicit, typed confirmation naming the exact device. Guards against
        # a mis-click reaching a destructive endpoint.
        if confirm != device:
            return error_response(
                "VALIDATION_ERROR",
                "confirm must exactly match the device name", status=400)

        path = target["path"]
        try:
            # Unmount everything on the target before touching it, or wipefs
            # fails with "device is busy" — a confusing way for this to end.
            #
            # Mountpoints are queried live rather than read from the listing
            # above: the eligible-device records deliberately carry no
            # partition detail, and state can change between listing and
            # formatting anyway.
            subprocess.run(["sudo", "umount", "-l", "/mnt/ssd"],
                           capture_output=True, timeout=30)
            for mp in _mountpoints_under(path):
                subprocess.run(["sudo", "umount", "-l", mp],
                               capture_output=True, timeout=30)

            steps = [
                # Clear any existing signatures so a stale label cannot linger.
                ["sudo", "wipefs", "-a", path],
                ["sudo", "parted", "-s", path, "mklabel", "gpt"],
                ["sudo", "parted", "-s", path, "mkpart", "primary", "ext4",
                 "0%", "100%"],
            ]
            for step in steps:
                subprocess.run(step, capture_output=True, text=True,
                               timeout=120, check=True)
            subprocess.run(["sudo", "partprobe", path], capture_output=True,
                           timeout=60)
            time.sleep(2)  # let udev create the partition node

            partition = _first_partition_of(path)
            if not partition:
                return error_response(
                    "STORAGE_ERROR",
                    "Partition was created but did not appear; try again")

            # The label is the whole contract with /etc/fstab.
            subprocess.run(["sudo", "mkfs.ext4", "-F", "-L", "MOVIES", partition],
                           capture_output=True, text=True, timeout=600, check=True)
            subprocess.run(["sudo", "systemctl", "daemon-reload"],
                           capture_output=True, timeout=30)
            subprocess.run(["sudo", "mount", "/mnt/ssd"], capture_output=True,
                           text=True, timeout=60, check=True)

            # Radarr binds ${STORAGE_ROOT}/library and qBit ${STORAGE_ROOT}/
            # downloads. Docker resolves a bind source once, at container
            # start, so these must exist before the stack is re-linked.
            for sub in ("library", "downloads"):
                subprocess.run(["sudo", "mkdir", "-p", f"/mnt/ssd/{sub}"],
                               capture_output=True, timeout=30, check=True)
            subprocess.run(["sudo", "chown", "-R", "magic:magic", "/mnt/ssd"],
                           capture_output=True, timeout=120)
        except subprocess.CalledProcessError as e:
            detail = (e.stderr or e.stdout or "").strip()[:300]
            return error_response("STORAGE_ERROR",
                                  f"Preparation failed: {detail or e}")
        except Exception as e:  # noqa: BLE001
            return error_response("STORAGE_ERROR", f"Preparation failed: {e}")

        # Re-link the containers onto the freshly mounted drive. Without this
        # they keep the bind they resolved at start — the empty placeholder
        # dirs on the SD card — and Radarr reports an empty library with
        # nothing in any log to explain it.
        try:
            subprocess.run(
                ["sudo", "systemctl", "start", "magic-dingus-storage-attach.service"],
                capture_output=True, timeout=300)
        except Exception:  # noqa: BLE001 - drive is prepared either way
            pass

        return success_response(
            data={"device": device, "mountpoint": "/mnt/ssd", "label": "MOVIES"},
            message=f"{device} is ready for movies")

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

        # Which gluetun provider runs this tunnel.
        #
        # The operator's dropdown wins when they set it; otherwise we detect.
        # Detection only promotes to NATIVE mode for the providers in
        # _VPN_AUTO_NATIVE_PROVIDERS (ProtonVPN alone, because it is the only
        # WireGuard provider gluetun can port-forward for). Every other
        # recognised brand is still a label — it runs as `custom`, which
        # works for any standard WireGuard config. That keeps a wrong guess
        # cosmetic instead of turning it into a failed tunnel.
        detected = _detect_vpn_brand(config_text, wg)
        requested = (request.form.get("provider") or "").strip().lower()
        if requested and requested != "auto":
            provider = requested
        elif detected in _VPN_AUTO_NATIVE_PROVIDERS:
            provider = detected
        else:
            provider = _VPN_PROVIDER_CUSTOM

        # `custom` has no server list, so gluetun connects to the endpoint in
        # the .conf verbatim — and it will only accept a literal IP there.
        # Several providers ship a hostname, so resolve it now and fail with
        # something the operator can act on rather than letting gluetun exit
        # at startup with a ParseAddr error nobody will ever read.
        if provider == _VPN_PROVIDER_CUSTOM:
            try:
                wg["WIREGUARD_ENDPOINT_IP"] = _resolve_wireguard_endpoint_ip(
                    wg["WIREGUARD_ENDPOINT_IP"])
            except ValueError as e:
                return error_response("invalid_wireguard_config", str(e),
                                      status=400)

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

        # Host-level defaults: fill only when absent, so a box that has been
        # tuned by hand keeps its settings.
        for key, value in {
            "STORAGE_ROOT": "/mnt/ssd",
            "PUID": "1000",
            "PGID": "1000",
            "TZ": _detect_timezone(),
        }.items():
            if not env.get(key):
                env[key] = value

        # VPN keys, by contrast, are applied AUTHORITATIVELY. They have to
        # stay mutually consistent with the config that was just uploaded:
        # a VPN_PORT_FORWARDING=on or a WIREGUARD_ENDPOINT_IP left over from
        # a previous provider is precisely what stops gluetun from starting.
        country = (request.form.get("country") or "").strip() or "Netherlands"
        env.update(_vpn_provider_env(provider, wg, country=country))

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
            data={
                "job_id": job_id,
                "detected_provider": detected,
                "provider": provider,
                "port_forwarding": _vpn_supports_port_forwarding(provider),
            },
            message="Media Browser setup started",
        )

    @app.post("/admin/media-browser/detect-provider")
    @require_csrf
    def media_browser_detect_provider():  # type: ignore[no-redef]
        """Preview which VPN provider a config looks like, without applying it.

        Lets the setup panel default its provider dropdown to the detected
        value the moment a .conf is dropped, so the operator sees — and can
        correct — the choice BEFORE anything is written to services/.env.

        Parses only; writes nothing and starts no job.
        """
        if (resp := _check_media_browser_gates(require_vpn=False)):
            return resp

        config_text = ""
        if "file" in request.files and request.files["file"].filename:
            try:
                config_text = request.files["file"].read().decode(
                    "utf-8", errors="replace")
            except Exception as e:
                return error_response("invalid_wireguard_config",
                                      f"Could not read uploaded file: {e}",
                                      status=400)
        else:
            config_text = (request.form.get("config_text") or "").strip()

        if not config_text:
            return error_response(
                "invalid_wireguard_config", "No WireGuard config provided",
                status=400)

        try:
            wg = _parse_wireguard_config(config_text)
        except ValueError as e:
            return error_response(
                "invalid_wireguard_config",
                f"Could not parse WireGuard config: {e}", status=400)

        detected = _detect_vpn_brand(config_text, wg)
        provider = (detected if detected in _VPN_AUTO_NATIVE_PROVIDERS
                    else _VPN_PROVIDER_CUSTOM)
        return success_response(data={
            "detected_provider": detected,
            "provider": provider,
            "port_forwarding": _vpn_supports_port_forwarding(provider),
            "choices": _vpn_provider_choices(),
        })

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

        # DELIBERATELY does not return the key/password VALUES.
        #
        # The Content Manager has no authentication — it is reachable by any
        # device on the customer's LAN, and GET /admin/csrf-token hands a token
        # to anyone who asks, so the CSRF token authenticates nobody. This
        # endpoint was therefore disclosing the qBittorrent admin password and
        # both API keys to any LAN client: a guest phone, a compromised IoT
        # device, or a malicious page via DNS rebinding.
        #
        # Nothing is lost by withholding them. The docstring's own use case is
        # "operators use these to SSH-tunnel into the Pi" — and anyone with the
        # shell access that presupposes can simply read services/.env directly.
        # So the values only ever helped someone who already had them, while
        # exposing them to everyone who did not.
        #
        # NOTE this is disclosure control, not authentication. The admin
        # surface still needs a real credential; that is a separate designed
        # change (a setup PIN shown on the kiosk screen, exchanged for a
        # session cookie). It deliberately must NOT reuse the phone-remote
        # pairing cookie: the Content Manager is opened from a laptop, which
        # never pairs, and first_boot.sh wipes paired_remotes.json — so a
        # pairing gate would 401 every fresh unit and break the documented
        # no-SSH setup workflow.
        return success_response(data={
            "radarr_url": "http://localhost:7878",
            "prowlarr_url": "http://localhost:9696",
            "qbittorrent_url": "http://localhost:8080",
            "qbittorrent_admin_username": env.get("QBITTORRENT_ADMIN_USERNAME", "admin"),
            "credentials_hint": (
                "Read secrets on the box: "
                "sudo cat /opt/magic_dingus_box/services/.env"
            ),
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

        # Does this box's VPN forward a port AT ALL?
        #
        # Most providers do not, and gluetun only implements it for four of
        # them (of which only ProtonVPN has WireGuard servers). A customer on
        # Mullvad or a self-hosted tunnel has no forwarded port by design —
        # that is a normal, healthy configuration with somewhat slower peer
        # discovery, NOT a fault. Reporting it in red as "unavailable"
        # alongside genuine outages sent people hunting for a broken thing
        # that was never there. Distinguish the two: `unsupported` means
        # there is nothing to report, `unavailable` means a port was
        # expected and is missing.
        provider = (env.get("VPN_SERVICE_PROVIDER") or "").strip().lower()
        pf_requested = (env.get("VPN_PORT_FORWARDING") or "").strip().lower() \
            in ("on", "true", "1", "yes")
        pf_expected = pf_requested and _vpn_supports_port_forwarding(provider)

        if not pf_expected:
            # Skip the docker exec entirely — there is no lease to ask about,
            # and the call would just burn its timeout on every refresh.
            forwarded_port = 0
            qbit_listen = _qbit_listen_port(env)
            port_status = "unsupported"
        else:
            forwarded_port = _gluetun_forwarded_port()
            qbit_listen = _qbit_listen_port(env)
            if forwarded_port <= 0 or qbit_listen == -1:
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
            "vpn_provider": provider,
            "vpn_port_forwarding_supported": _vpn_supports_port_forwarding(provider),
        })

    # ----- TMDB API key -----
    #
    # THE RESTART PROBLEM. The kiosk loads the TMDB key exactly once, during
    # startup: main.cpp reads $MDB_TMDB_API_KEY or the key file and passes the
    # string to `TmdbClient(tmdb_key)`. TmdbClient's only constructor takes the
    # key by value (tmdb_client.h) and exposes no setter, and the kiosk has no
    # SIGHUP handler or config-reload path — so writing the file does NOT reach
    # a process that is already running. It keeps whatever it had at boot,
    # which on a shipped box is nothing.
    #
    # We do NOT restart the kiosk automatically. This endpoint is reachable at
    # any time, not just during first-run setup, so an implicit restart could
    # kill a movie mid-playback with no warning. Instead:
    #   * the save response reports kiosk_restart_required
    #   * GET reports kiosk_has_current_key, derived from the kiosk service's
    #     start time vs. the key file's mtime — a fact, not a guess
    #   * POST /admin/media-browser/restart-kiosk performs the restart, but
    #     only when the operator explicitly asks for it
    def _tmdb_env_override_present() -> bool:
        """True if MDB_TMDB_API_KEY is set somewhere the kiosk will see it.

        main.cpp checks the environment variable BEFORE the file, and the
        kiosk unit has `EnvironmentFile=-/opt/magic_dingus_box/services/.env`.
        If that file ever defines MDB_TMDB_API_KEY, anything written here is
        dead on arrival — so the UI needs to be able to say so instead of
        reporting a successful save that changes nothing.
        """
        if os.getenv("MDB_TMDB_API_KEY", "").strip():
            return True
        try:
            return bool(_read_env_file(SERVICES_ENV).get("MDB_TMDB_API_KEY", "").strip())
        except Exception:
            return False

    def _tmdb_key_state() -> dict:
        """Everything the UI needs about the key — never the key itself."""
        path = _tmdb_key_file()
        configured = False
        key_length = 0
        updated_at = None
        try:
            if path.is_file():
                content = path.read_text(encoding="utf-8", errors="replace").strip()
                if content:
                    configured = True
                    key_length = len(content)
                    updated_at = path.stat().st_mtime
        except OSError:
            pass

        kiosk_active = check_service_status("magic-dingus-box-cpp") == "active"
        started_at = _kiosk_started_at()
        # Only meaningful when a key exists AND we could read both timestamps.
        if not configured or updated_at is None or started_at is None:
            kiosk_has_current_key = None
        else:
            kiosk_has_current_key = started_at >= updated_at

        return {
            "configured": configured,
            # Width for the masked readout. A v3 key is always 32 hex chars,
            # so this discloses nothing the format doesn't already imply, and
            # the real value never leaves the box.
            "key_length": key_length,
            "updated_at": updated_at,
            "kiosk_service_active": kiosk_active,
            "kiosk_has_current_key": kiosk_has_current_key,
            "env_override": _tmdb_env_override_present(),
            "signup_url": "https://www.themoviedb.org/settings/api",
        }

    @app.get("/admin/media-browser/tmdb")
    def media_browser_tmdb_status():  # type: ignore[no-redef]
        """Report whether a TMDB key is configured. Never returns the key.

        Layer 1 only (require_vpn=False): TMDB metadata calls exit via the
        host network, not through Gluetun, so the key is useful — and
        settable — regardless of VPN state.
        """
        if (resp := _check_media_browser_gates(require_vpn=False)):
            return resp
        return success_response(data=_tmdb_key_state())

    @app.post("/admin/media-browser/tmdb")
    @require_csrf
    def media_browser_tmdb_save():  # type: ignore[no-redef]
        """Validate a TMDB v3 API key and write it where the kiosk reads it.

        Body: {"api_key": "<32 hex>", "allow_unverified": false}

        Order of operations matters: the key is checked against TMDB BEFORE
        anything is written, so a bad key cannot clobber a working one.
        """
        if (resp := _check_media_browser_gates(require_vpn=False)):
            return resp

        body = request.get_json(silent=True) or {}
        kind, key = _tmdb_classify_key(body.get("api_key", ""))

        if kind == "empty":
            return error_response(
                "invalid_key_format", "Enter your TMDB API key.", status=400)
        if kind == "v4":
            return error_response(
                "unsupported_key_type",
                "That looks like a TMDB Read Access Token (v4). This box needs "
                "the API Key (v3) — the 32-character value on the same TMDB "
                "settings page, listed as \"API Key\".",
                status=400,
            )
        if kind != "v3":
            return error_response(
                "invalid_key_format",
                "A TMDB API key is exactly 32 characters, digits and letters "
                "a-f only. Check for a stray space or a truncated paste.",
                status=400,
            )

        result, detail = _tmdb_verify_key(key)
        if result == "invalid":
            return error_response(
                "key_rejected",
                detail or "TMDB rejected this key.",
                status=400,
            )
        if result == "unreachable" and not body.get("allow_unverified"):
            # Deliberately not saved. The format is right but we have no
            # evidence the key works, and saving an unverified key reproduces
            # the silent-empty-Browse failure. The client can retry with
            # allow_unverified once the operator accepts that trade.
            return error_response(
                "tmdb_unreachable",
                detail or "Could not reach TMDB to check this key.",
                status=503,
            )

        verified = (result == "valid")
        path = _tmdb_key_file()
        try:
            # Trailing newline: main.cpp strips \n, \r and spaces off the end,
            # so this is safe and keeps the file a well-formed text file.
            # 0600 because the kiosk and the web app both run as `magic` and
            # nobody else has any business reading it.
            _atomic_write_text(path, key + "\n", mode=0o600)
        except OSError as e:
            return error_response(
                "write_failed",
                _tmdb_redact(f"Could not write the key file: {e}", key),
                status=500,
            )

        state = _tmdb_key_state()
        state["verified"] = verified
        # After a fresh write the file is newer than any running kiosk, so
        # _tmdb_key_state already reports kiosk_has_current_key=False. Restate
        # it as an explicit instruction for the UI.
        state["kiosk_restart_required"] = (
            state["kiosk_service_active"] and not state["kiosk_has_current_key"]
        )
        if not verified:
            state["verify_warning"] = detail or "Key saved without verification."
        return success_response(
            data=state,
            message=("TMDB key saved and verified." if verified
                     else "TMDB key saved, but it could not be verified."),
        )

    @app.post("/admin/media-browser/restart-kiosk")
    @require_csrf
    def media_browser_restart_kiosk():  # type: ignore[no-redef]
        """Restart the kiosk app so it re-reads the TMDB key.

        Explicitly operator-triggered — see the restart-problem note above.
        This interrupts whatever is on the TV, which is why nothing calls it
        implicitly.
        """
        if (resp := _check_media_browser_gates(require_vpn=False)):
            return resp
        try:
            result = subprocess.run(
                ["sudo", "-n", "systemctl", "restart", KIOSK_SERVICE],
                capture_output=True, text=True, timeout=120,
            )
            if result.returncode != 0:
                return error_response(
                    "restart_failed",
                    (result.stderr or result.stdout or "systemctl restart failed").strip(),
                    status=500,
                )
            return success_response(message="Kiosk restarted")
        except subprocess.TimeoutExpired:
            return error_response("timeout", "Kiosk restart timed out", status=504)
        except Exception as e:
            return error_response("restart_failed", str(e), status=500)

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
                text_input_writer=app.config["TEXT_INPUT_WRITER"],
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

    @app.get("/api/host-info")
    def host_info():  # type: ignore[no-redef]
        """The box's own addresses, for the install-coaching toast.

        An installed home-screen app is pinned to the origin it was
        installed from, permanently. If the phone arrived via the pairing
        QR it is standing on a raw DHCP IP, and an icon made there breaks
        at the next lease change. The toast uses this to offer the stable
        <hostname>.local address instead.
        """
        import socket
        try:
            host = socket.gethostname()
        except Exception:
            host = ""
        mdns = (host + ".local") if host and not host.endswith(".local") else host
        return jsonify({"hostname": host, "mdns": mdns})

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
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover, maximum-scale=1, user-scalable=no">
<!-- Faceplate BOTTOM-edge colour (#131013), not --bg. iOS paints the region
     outside the web view with theme-color, and on a standalone launch that
     region is real: the layout viewport comes up ~59px short of the screen.
     It cannot be drawn into (only the root BACKGROUND propagates past the
     viewport; content and borders are clipped), so matching the colour the
     faceplate ends on is the only way to hide the seam. -->
<meta name="theme-color" content="#131013">
<!-- Same installable-app tags as the other pages. Without these, adding
     to the home screen from THIS page (a real possibility, since it is
     where an expired pairing lands you) produces a generic Safari
     bookmark with a screenshot icon instead of the app. -->
<link rel="manifest" href="/static/manifest.webmanifest">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="Magic Dingus Box">
<link rel="apple-touch-icon" href="/static/icons/icon-180.png">
<link rel="icon" type="image/png" sizes="32x32" href="/static/icons/icon-32.png">
<title>Remote not paired</title>
<style>
  html, body { margin: 0; padding: 0; background: #1F191F; color: #F2E4D9;
               font-family: -apple-system, system-ui, sans-serif;
               min-height: 100vh; display: flex; align-items: center; justify-content: center; }
  .card { width: min(360px, 90%); padding: 32px 24px; background: #2A232A;
          border-radius: 16px; text-align: center; }
  h1 { margin: 0 0 12px; font-size: 22px; }
  p { color: #968B85; line-height: 1.5; }
  form { margin: 22px 0 6px; }
  input[name=pair] {
    width: 100%; box-sizing: border-box; padding: 16px; font-size: 30px;
    letter-spacing: 10px; text-align: center; border-radius: 12px;
    border: 2px solid #4A414A; background: #1F191F; color: #F2E4D9;
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }
  input[name=pair]:focus { outline: none; border-color: #EA3A27; }
  button {
    width: 100%; margin-top: 14px; padding: 16px; font-size: 17px;
    font-weight: 600; border: 0; border-radius: 12px;
    background: #EA3A27; color: #FFF; }
  .home { display: inline-block; margin-top: 20px; color: #968B85;
          font-size: 15px; text-decoration: none; }
  .hint { font-size: 13px; margin-top: 4px; }
</style>
</head>
<body>
<div class="card">
  <h1>Pair this remote</h1>
  <p>On the kiosk, open <strong>Settings &rarr; Phone Remote</strong> and enter the 6-digit code shown there.</p>
  <!-- A form, not just instructions. Scanning the QR opens Safari, which
       on iOS has a DIFFERENT cookie jar from an installed home-screen app
       — so a QR scan can never authenticate this app, and telling the
       user to scan it left them permanently stuck with no way out (there
       is no address bar in standalone mode). Submitting here issues the
       request from THIS jar, so the cookie lands where it is needed.
       GET to "/" because that is where handle_pair_param lives; there is
       no /pair route. -->
  <form action="/" method="get" autocomplete="off">
    <input name="pair" inputmode="numeric" pattern="[0-9]{6}" maxlength="6"
           placeholder="000000" aria-label="6-digit pairing code" autofocus>
    <input type="hidden" name="tab" value="remote">
    <button type="submit">Pair</button>
  </form>
  <p class="hint">The code changes every couple of minutes.</p>
  <a class="home" href="/">&larr; Content Manager</a>
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
            # Update the entry in paired_remotes.json — atomic temp+rename so
            # concurrent reads from the StatusBroadcaster's reap_revocations
            # tick can never see a torn write.
            try:
                data = json.loads(paired_path.read_text())
            except (FileNotFoundError, json.JSONDecodeError):
                data = {"schema": 1, "devices": []}
            for d in data["devices"]:
                if d["id"] == device_id:
                    d["nickname"] = nickname
                    break
            _atomic_write_text(paired_path, json.dumps(data, indent=2))
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

