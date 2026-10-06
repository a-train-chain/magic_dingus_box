from __future__ import annotations

import collections
import io
import ipaddress
import json
import posixpath
from urllib.parse import urlencode, urlsplit
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
        movies_drive_devices,
        protected_disk_names,
    )
    from detached_jobs import DetachedJobs, default_state_dir as _default_job_state_dir
    import box_health
    import diagnostics
    import vpn_settings
    from redact import Redactor
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from .detached_jobs import DetachedJobs, default_state_dir as _default_job_state_dir
    from . import box_health
    from . import diagnostics
    from . import vpn_settings
    from .redact import Redactor
    from .storage_prepare import (
        PROTECTED_MOUNTPOINTS,
        eligible_devices,
        movies_drive_devices,
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
from werkzeug.exceptions import HTTPException

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


# One gibibyte, spelled out once so every size the API reports uses the same
# unit the disk figures in /admin/health/detailed already use.
BYTES_PER_GB = 1024 ** 3


def get_free_bytes(path) -> Optional[int]:
    """Free bytes available to an unprivileged writer at `path`.

    get_disk_info() rounds to two decimal places of a gibibyte, which is fine
    for a dashboard and useless for a precondition — 0.00 GB free and 5 MB free
    round to the same number. Preconditions need the raw count.

    Returns None when the filesystem cannot be interrogated at all (statvfs is
    Linux/macOS only, and the path may not exist yet). Callers MUST treat None
    as "unknown", not as "full": refusing every upload on a machine where the
    check itself is unavailable would be worse than the ENOSPC it prevents.
    """
    try:
        stat = os.statvfs(str(path))
        return stat.f_bavail * stat.f_frsize
    except Exception:
        return None


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


_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def strip_ansi(text: str) -> str:
    """Remove terminal colour codes before showing script output in the UI.

    update.sh colours its progress lines for a human at a terminal. Passing
    that straight into a JSON error made the Content Manager render
    literal escape gibberish -- observed on a fresh unit with no network:
        \u001b[0;32m[UPDATE]\u001b[0m Checking GitHub for updates...
    which tells a customer nothing about what actually went wrong.
    """
    return _ANSI_RE.sub("", text or "").strip()


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


# OTA install inputs. See install_update() for why `version` is load-bearing.
# re.ASCII + explicit [0-9]: the pattern must not admit non-ASCII digits.
# X.Y.Z, or a beta-channel prerelease X.Y.Z-beta.N — the same grammar as
# update.sh's VERSION_RE and release.yml's tag check. Nothing here compares
# versions; update.sh's version_cmp is the only comparator.
_OTA_VERSION_RE = re.compile(
    r"[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}(?:-beta\.[0-9]{1,6})?", re.ASCII)
# The OTA update channels `update.sh channel` understands (stable = default).
_OTA_CHANNELS = ("stable", "beta")
# A release asset name: a plain filename. No "/", no "%" (so no encoded
# separators or dot-segments), and it may not be "." or "..".
_OTA_ASSET_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,199}", re.ASCII)


def _ota_download_url_ok(url: str, repo: str, version: str) -> bool:
    """True iff `url` is exactly one of the shapes a TAGGED v<version>
    release of `repo` is served from. Anything else — another repo, another
    tag, a branch archive, dot-segments, a query — is refused.

    Accepted (all https, exact host, path compared verbatim — a path that
    normalizes differently from how it is written is rejected outright):
      github.com/<repo>/releases/download/v<ver>/<asset>   (release asset;
                                     what `update.sh check` emits first)
      api.github.com/repos/<repo>/tarball/v<ver>           (tarball_url
                                     fallback of `update.sh check`)
      codeload.github.com/<repo>/tar.gz/[refs/tags/]v<ver> (tarball_url's
                                     redirect target)
      github.com/<repo>/archive/refs/tags/v<ver>.tar.gz    (UI "Source code")
    """
    try:
        parts = urlsplit(url)
        if (parts.scheme != "https" or parts.query or parts.fragment
                or parts.username is not None or parts.password is not None
                or parts.port is not None):  # .port raises on a bad port
            return False
    except ValueError:
        return False
    path = parts.path
    if not path or posixpath.normpath(path) != path:
        return False
    tag = f"v{version}"
    host = parts.hostname or ""
    if host == "github.com":
        prefix = f"/{repo}/releases/download/{tag}/"
        if path.startswith(prefix):
            return bool(_OTA_ASSET_RE.fullmatch(path[len(prefix):]))
        return path == f"/{repo}/archive/refs/tags/{tag}.tar.gz"
    if host == "api.github.com":
        return path == f"/repos/{repo}/tarball/{tag}"
    if host == "codeload.github.com":
        return path in (f"/{repo}/tar.gz/{tag}", f"/{repo}/tar.gz/refs/tags/{tag}")
    return False


def _has_internet(timeout: float = 3.0) -> bool:
    """Best-effort reachability probe, used ONLY to improve an error message.

    Never gates behaviour -- a false negative would just mean the operator
    gets the raw script output instead of the friendlier hint. Costs up to
    `timeout` seconds and only ever runs on a path that has already failed.
    """
    try:
        with socket.create_connection(("api.github.com", 443), timeout=timeout):
            return True
    except OSError:
        return False


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

# Written by playback_services_pause.sh while the kiosk has intentionally
# stopped the RAM-heavy Media Browser containers for a game/movie. Lives in
# /tmp (tmpfs) so a marker orphaned by a power cut cannot survive into the
# next boot; neither this service's unit nor the kiosk's uses PrivateTmp, so
# both processes see the same file.
PLAYBACK_PAUSE_MARKER = Path("/tmp/mdb_playback_services_paused")

# The containers that script stops. Gluetun (VPN netns) and qBittorrent
# (active downloads) intentionally stay up during playback.
PLAYBACK_PAUSED_CONTAINERS = frozenset({"mdb_radarr", "mdb_sonarr", "mdb_prowlarr", "mdb_byparr"})


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
# Bounded, and locked. GET /admin/csrf-token mints a token per call, so the
# dict grew without limit for anything that looped it, and the cleanup
# iterated it while request threads inserted ("dictionary changed size during
# iteration" -> a 500 on a random request). A household holds a handful of
# live tokens; at the cap the earliest-expiring (= oldest) are dropped, and
# an operator whose token was evicted just reloads the page.
_CSRF_TOKEN_MAX = 2048
_csrf_lock = threading.Lock()


def _cleanup_expired_tokens():
    """Remove expired CSRF tokens. Caller holds _csrf_lock."""
    current_time = time.time()
    expired = [token for token, expiry in _csrf_tokens.items() if current_time > expiry]
    for token in expired:
        del _csrf_tokens[token]


def _generate_csrf_token() -> str:
    """Generate a new CSRF token."""
    token = secrets.token_urlsafe(32)
    with _csrf_lock:
        _cleanup_expired_tokens()
        overflow = len(_csrf_tokens) - (_CSRF_TOKEN_MAX - 1)
        if overflow > 0:
            for old in sorted(_csrf_tokens, key=_csrf_tokens.get)[:overflow]:
                del _csrf_tokens[old]
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
    with _csrf_lock:
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


# Hard cap for the small text entries (playlist YAML, settings/device JSON,
# manifest) read out of a backup or playlist-package ZIP. Real ones are a
# few KB; 4 MB is ~1000x headroom. Without it, zf.read() allocated whatever
# the archive declared — and deflate shrinks a run of spaces ~1000:1, so a
# sub-MB upload could make the web service allocate gigabytes on a 1.5 GB
# Pi 4B, where the OOM killer then picks the kiosk or the web service.
_ZIP_TEXT_ENTRY_MAX = 4 * 1024 * 1024


class _ZipEntryTooLarge(ValueError):
    """A ZIP text entry exceeds _ZIP_TEXT_ENTRY_MAX."""


def _read_zip_entry_capped(zf: "zipfile.ZipFile", name: str,
                           cap: int = _ZIP_TEXT_ENTRY_MAX) -> bytes:
    """Read one ZIP entry, refusing anything over `cap` bytes.

    Two checks, because the first is the archive's own claim: the declared
    (central-directory) size, which rejects an honest oversized entry before
    any decompression; then a bounded read(cap + 1), so an archive that
    under-declares can still never make us hold more than cap + 1 bytes.
    """
    info = zf.getinfo(name)
    if info.file_size > cap:
        raise _ZipEntryTooLarge(
            f"{name} is too large ({info.file_size} bytes; limit {cap})")
    with zf.open(info) as f:
        data = f.read(cap + 1)
    if len(data) > cap:
        raise _ZipEntryTooLarge(f"{name} is too large (limit {cap} bytes)")
    return data


class _ExtractTooLarge(Exception):
    """Internal signal: a ZIP entry would push extraction past the byte cap.

    Raised from inside the streaming loop so the staging temp file is cleaned up
    by one handler rather than at each bail-out point. Never escapes the import
    endpoint.
    """


def _staged_save_upload(file_storage, dest: Path, mode: int = 0o644) -> None:
    """Save a werkzeug upload to dest via same-directory staging + os.replace.

    A direct f.save(dest) writes the final name from byte 0: an interrupted
    transfer leaves a truncated file that lists as real content (a video the
    kiosk fails to play, a ROM that fails to launch), and re-uploading over
    an existing file destroys the ORIGINAL the moment the transfer starts
    rather than when it succeeds. Staging in the same directory keeps the
    os.replace atomic (one filesystem), and the dot-prefix + .tmp suffix
    keeps the staging file out of the extension globs the listings use.
    """
    dest.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(
        dir=str(dest.parent), prefix=f".{dest.name}.", suffix=".tmp")
    os.close(fd)
    tmp_path = Path(tmp_name)
    try:
        file_storage.save(str(tmp_path))
        os.chmod(tmp_path, mode)
        os.replace(tmp_path, dest)
    except BaseException:
        try:
            tmp_path.unlink()
        except OSError:
            pass
        raise


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
        # A basename that IS the extension ('.yaml') is a legacy ghost: an
        # emoji-only title used to slug to '' and write that literal file.
        # splitext puts the whole thing in the STEM, so ext is '' and the
        # check below rejects it — which is why those rows were listed by
        # Path.glob (pathlib does not hide dotfiles) yet could never be
        # fetched or deleted through the API. Creation is prevented upstream
        # now by _playlist_filename_for_title, so accepting it here only ever
        # lets an operator clean one up. No separator is involved, so this
        # widens nothing for path traversal.
        if not ext and basename.lower() in allowed_extensions:
            return basename
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


def _max_transcodes_for(pi_model: str, env_value: Optional[str]) -> int:
    """Concurrent-transcode cap: env override, else 2 on Pi 5, else 1."""
    if env_value is not None and str(env_value).strip():
        try:
            return max(1, int(env_value))
        except ValueError:
            pass
    return 2 if pi_model == "pi5" else 1


def _detect_pi_model() -> str:
    """Return 'pi5', 'pi4', or 'unknown' from the device-tree model.

    Python-side mirror of the C++ kiosk's PlatformProfile detection
    (dual-board contract: board differences resolve at RUNTIME, never
    at deploy time — one golden image serves both boards). Module-level
    so tests can stand in for a board."""
    try:
        model = Path('/proc/device-tree/model').read_text(errors='ignore')
    except OSError:
        return 'unknown'
    if 'Raspberry Pi 5' in model:
        return 'pi5'
    if 'Raspberry Pi 4' in model:
        return 'pi4'
    return 'unknown'


# TEST-ONLY pre-release rehearsal knob, mirrored from the kiosk's
# platform::apply_policy_override (platform_profile.h). With
# MDB_PLATFORM_POLICY_OVERRIDE=pi4 on a Pi 5, the web admin applies the
# Pi 4B's SOFTWARE POLICIES (one concurrent transcode, ultrafast/CRF 28
# encoder tier). The kiosk and this service each read their OWN unit
# environment, so a rehearsal sets it in BOTH units' drop-ins.
# verify_box.sh FAILS while either unit carries it — never ship a box with it.
PLATFORM_POLICY_OVERRIDE_ENV = "MDB_PLATFORM_POLICY_OVERRIDE"


def _policy_pi_model(real_model: str,
                     env_value: Optional[str]) -> tuple[str, str]:
    """Resolve the board whose POLICIES the web admin applies.

    Returns (policy_model, log_line). Same contract as the kiosk: only the
    exact value "pi4" is accepted, and only on a real Pi 5; anything else
    leaves `real_model` in force. log_line is "" when the env var is unset,
    otherwise the one loud line to print at startup (active or ignored)."""
    if not env_value:
        return real_model, ""
    if env_value != "pi4":
        reason = f"unrecognized value {env_value!r} (only 'pi4' is accepted)"
    elif real_model == "pi4":
        reason = "board is already a Pi 4B, so the override is a no-op here"
    elif real_model != "pi5":
        reason = "board is not a Pi 5; the override applies only on a Pi 5"
    else:
        return "pi4", (
            "PLATFORM POLICY OVERRIDE ACTIVE: running Pi 4B policies on "
            f"Raspberry Pi 5 ({PLATFORM_POLICY_OVERRIDE_ENV}=pi4 is "
            "TEST-ONLY: never ship; verify_box.sh fails while set)")
    return real_model, (f"{PLATFORM_POLICY_OVERRIDE_ENV} IGNORED: {reason} "
                        "(remove it from the unit environment)")


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


# ===== HOST HEADER ALLOWLIST (DNS-rebinding defence) =====
#
# The web admin deliberately has NO login (owner decision: a customer must
# never need a password or PIN). That makes it a DNS-rebinding target: a web
# page on the internet can point its own hostname at this box's LAN IP and
# then script same-origin requests at it from the victim's browser. The one
# thing such a request cannot fake is the Host header — it carries the
# ATTACKER's domain. So we accept only names a person on the LAN could
# legitimately type, and refuse everything else. Zero friction for real
# users; no credentials involved.
#
# Allowed:
#   * any IP literal (v4 / bracketed v6, optional port) — the pairing QR and
#     the typed address on the Connect screen are the LAN IP
#   * single-label names ("localhost", "magicpi-ab12", "dingus") — not
#     registrable on the public internet, so not rebindable
#   * *.local (mDNS: magicpi-XXXX.local), *.localhost, and the router-local
#     suffixes (.lan, .home, .home.arpa, .internal, .localdomain) — none are
#     publicly delegated
#   * dingus.box — the name advertised for the USB-C cable
#     (scripts/data/dnsmasq-usb0.conf, pairing_screen_renderer.cpp)
#   * this machine's own hostname / FQDN
#   * <this box's hostname>.<router suffix> for the suffixes home routers
#     append in their own DNS (_ROUTER_DNS_SUFFIXES: magicpi-ab12.fritz.box,
#     magicpi-ab12.attlocal.net, magicpi-ab12.router). The first label must
#     equal socket.gethostname() exactly (case-insensitive). These suffixes
#     sit under publicly registered domains (fritz.box is AVM's,
#     attlocal.net AT&T's), so unlike .lan they are not accepted for ANY
#     first label: the box's own unique name narrows them to this box. The
#     suffix list stays explicit on purpose. "Own hostname + any suffix"
#     would admit magicpi-ab12.<attacker's domain>: the hostname is visible
#     to anything on the LAN (mDNS) and its magicpi-XXXX pattern is
#     guessable, and the attacker controls the suffix, which is exactly
#     what this check exists to refuse.
#   * anything in MAGIC_ALLOWED_HOSTS (comma-separated; ".example.com" allows
#     a whole suffix) — an escape hatch that needs no release
_LOCAL_HOST_SUFFIXES = (
    ".local", ".localhost", ".lan", ".home", ".home.arpa", ".internal",
    ".localdomain",
)
_BUILTIN_ALLOWED_HOSTS = frozenset({"dingus.box"})
# Accepted only as <own hostname><suffix> (see above). Router DNS suffixes
# that are NOT already in _LOCAL_HOST_SUFFIXES (.lan, .home, .home.arpa,
# .localdomain, .internal and .local are accepted for any first label).
_ROUTER_DNS_SUFFIXES = (".fritz.box", ".attlocal.net", ".router")


def _split_host_header(host_header: str) -> str:
    """Return the lowercase host part of a Host header, port stripped."""
    host = (host_header or "").strip().lower()
    if host.startswith("["):
        end = host.find("]")
        return host[1:end] if end != -1 else host[1:]
    if host.count(":") == 1:
        host = host.rsplit(":", 1)[0]
    return host.rstrip(".")


def _host_is_allowed(host_header: str, own_names=(), extra=(),
                     own_label: str = "") -> bool:
    host = _split_host_header(host_header)
    if not host:
        # No Host header at all (HTTP/1.0 tooling). A browser — the only
        # thing DNS rebinding can drive — always sends one.
        return True
    try:
        ipaddress.ip_address(host.split("%", 1)[0])
        return True
    except ValueError:
        pass
    if "." not in host:
        return True
    if host in _BUILTIN_ALLOWED_HOSTS:
        return True
    if host.endswith(_LOCAL_HOST_SUFFIXES):
        return True
    if host in own_names:
        return True
    if own_label:
        first, _, suffix = host.partition(".")
        if first == own_label.lower() and ("." + suffix) in _ROUTER_DNS_SUFFIXES:
            return True
    for entry in extra:
        if entry.startswith("."):
            if host.endswith(entry) or host == entry[1:]:
                return True
        elif host == entry:
            return True
    return False


def _own_host_names() -> frozenset:
    names = set()
    for fn in (socket.gethostname, socket.getfqdn):
        try:
            n = (fn() or "").strip().lower().rstrip(".")
        except Exception:
            n = ""
        if n:
            names.add(n)
            names.add(n.split(".", 1)[0] + ".local")
    return frozenset(names)


def _own_host_label() -> str:
    """This box's hostname, first label only, lowercase ("" if unknown)."""
    try:
        return (socket.gethostname() or "").strip().lower().split(".", 1)[0]
    except Exception:
        return ""


def _extra_allowed_hosts() -> tuple:
    raw = os.getenv("MAGIC_ALLOWED_HOSTS", "")
    return tuple(h.strip().lower().rstrip(".")
                 for h in raw.split(",") if h.strip())


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


def _require_nopasswd_sudo():
    """Fail-fast NOPASSWD-sudo precheck for routes that shell out via `sudo -n`.

    Returns an error_response() tuple when the magic user lacks NOPASSWD sudo
    (so the caller can `if (resp := _require_nopasswd_sudo()): return resp`),
    or None when sudo is available. Extracted from three byte-identical copies
    (WireGuard setup, MB restart, MB reset) — the point is to fail with a clear
    error rather than spawn a process that hangs on a password prompt.
    """
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
    return None


def _qbit_get(env: dict, path: str):
    """Authenticated GET against the local qBittorrent WebUI.

    Returns the response body text, or None on any failure (missing/placeholder
    credentials, login failure, curl error, or an empty body). Each caller
    parses the body it expects. Credentials are passed via stdin (`-d @-`),
    never argv, so the password never lands in /proc/<pid>/cmdline. Extracted
    from the identical login + cookie-jar + finally-unlink preamble that
    _qbit_torrent_summary and _qbit_listen_port each carried.
    """
    username = env.get("QBITTORRENT_ADMIN_USERNAME", "admin")
    password = env.get("QBITTORRENT_ADMIN_PASSWORD", "").strip()
    if not password or password.startswith("__"):
        return None
    cookie_jar = tempfile.NamedTemporaryFile(suffix=".cookies", delete=False)
    cookie_jar.close()
    try:
        login = subprocess.run(
            ["curl", "-sS", "--max-time", "5",
             "-c", cookie_jar.name,
             "-d", "@-",
             "http://localhost:8080/api/v2/auth/login"],
            input=f"username={username}&password={password}",
            capture_output=True, text=True, timeout=6,
        )
        if login.returncode != 0 or "Ok." not in (login.stdout or ""):
            return None
        resp = subprocess.run(
            ["curl", "-sS", "--max-time", "5",
             "-b", cookie_jar.name,
             f"http://localhost:8080/api/v2/{path}"],
            capture_output=True, text=True, timeout=6,
        )
        if resp.returncode != 0 or not resp.stdout.strip():
            return None
        return resp.stdout
    except Exception:
        return None
    finally:
        try:
            os.unlink(cookie_jar.name)
        except Exception:
            pass


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


CONNECT_PAGE_HTML = """
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#131013">
<title>Connect a Device</title>
<style>
  /* Standalone page — matches the dark styling of the inline pair page
     (/admin/remote) rather than style.css, and carries its own copy of the
     mobile overflow hygiene rules: this page never loads style.css, so a
     long hostname or query string must not push the layout wide here
     either. */
  * { box-sizing: border-box; min-width: 0; }
  html, body {
    margin: 0; padding: 0; min-height: 100vh;
    background: #1F191F; color: #F2E4D9;
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif;
    display: flex; align-items: center; justify-content: center;
    overflow-wrap: anywhere; word-break: break-word;
  }
  .card {
    width: min(400px, 92%); padding: 32px 24px;
    background: #2A232A; border-radius: 16px;
    text-align: center; margin: 24px 0;
  }
  h1 { margin: 0 0 8px; font-size: 22px; font-weight: 600; }
  p.sub { margin: 0 0 26px; font-size: 14px; color: #968B85; line-height: 1.5; }
  a.big {
    display: block; width: 100%; padding: 18px 16px;
    border-radius: 12px; text-decoration: none;
    font-size: 17px; font-weight: 600; line-height: 1.3;
  }
  a.big span { display: block; margin-top: 4px; font-size: 13px; font-weight: 400; }
  a.big:active { filter: brightness(0.9); }
  .remote { background: #EA3A27; color: #FFF; margin-bottom: 14px; }
  .remote span { color: rgba(255, 255, 255, 0.75); }
  .manage { background: #1F191F; color: #F2E4D9; border: 2px solid #4A414A; }
  .manage span { color: #968B85; }
  p.hint { margin: 22px 0 0; font-size: 13px; color: #968B85; line-height: 1.5; }
</style>
</head>
<body>
<div class="card">
  <h1>Connect a Device</h1>
  <p class="sub">This is your Magic Dingus Box. What would you like to do?</p>
  <a class="big remote" href="{{ remote_href }}">Use this phone as a remote
    <span>D-pad control, and type with your phone&rsquo;s keyboard</span></a>
  <a class="big manage" href="/">Manage movies &amp; playlists
    <span>Upload videos, build playlists, movie setup</span></a>
  {% if has_code %}
  <p class="hint">Choosing the remote pairs this phone automatically &mdash;
     no code to type.</p>
  {% else %}
  <p class="hint">Pairing a remote needs the 6-digit code from
     Settings &rarr; Connect a Device on the kiosk.</p>
  {% endif %}
  <p class="hint">On a laptop with a USB-C cable? Any address works &mdash;
     try <strong>http://dingus.box</strong></p>
</div>
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


# services/.env is consumed by bash `.`/`source` running as ROOT
# (verify_services.sh, import_library_movies.sh), by docker compose, by
# systemd EnvironmentFile= and by `grep | cut` readers — and several of its
# values arrive from the LAN (the setup form's `country`, the uploaded
# WireGuard .conf's keys and addresses). Written raw, `country=X;cmd` ran
# `cmd` as root on the next smoke test, and a legitimate "United States"
# made bash try to execute `States`. So every value is checked against a
# charset that is inert in all of those readers. A value with spaces is
# double-quoted (all four readers strip the quotes; none of the grep|cut
# readers ever reads a spaced value); anything else outside the set is
# refused rather than escaped, because no single escaping is correct for
# all four parsers.
_ENV_KEY_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
_ENV_BARE_VALUE_RE = re.compile(r"^[A-Za-z0-9_./:+=,@%-]*$")
_ENV_QUOTED_VALUE_RE = re.compile(r"^[A-Za-z0-9_./:+=,@% -]*$")


def _format_env_line(key: str, value) -> str:
    """Serialize one KEY=VALUE line for services/.env, or raise ValueError."""
    value = "" if value is None else str(value)
    if not _ENV_KEY_RE.match(key):
        raise ValueError(f"invalid .env key: {key!r}")
    if _ENV_BARE_VALUE_RE.match(value):
        return f"{key}={value}"
    if _ENV_QUOTED_VALUE_RE.match(value):
        return f'{key}="{value}"'
    raise ValueError(f"unsafe characters in .env value for {key}")


def _unquote_env_value(value: str) -> str:
    """Inverse of _format_env_line's quoting for a raw .env value."""
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        return value[1:-1]
    return value


class EnvFileReadError(Exception):
    """services/.env exists but could not be read (permissions, I/O, encoding)."""


def _read_env_file(path: Path) -> dict:
    """Parse a KEY=VALUE .env file into a dict.

    Returns {} ONLY when the file does not exist. Any other failure raises
    EnvFileReadError. It used to return {} on every exception, and that was
    destructive: a root-owned 0600 .env raised PermissionError, the setup
    route took the {} as "empty .env", merged in only the WireGuard keys and
    wrote that back — erasing QBITTORRENT_ADMIN_PASSWORD and the API keys.
    setup_services.sh then generated a fresh qBit password qBittorrent did
    not have, and the kiosk, port-sync and password-sync all lost qBit auth.
    Callers that WRITE must treat the exception as "abort, write nothing".
    """
    try:
        text = Path(path).read_text()
    except FileNotFoundError:
        return {}
    except (OSError, UnicodeDecodeError) as e:
        raise EnvFileReadError(f"Could not read {path}: {e}") from e
    result = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        result[key.strip()] = _unquote_env_value(value.strip())
    return result


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

    
    # DNS-rebinding defence — see _host_is_allowed. Registered first so it
    # runs ahead of every other hook, including the phone-remote WebSocket
    # upgrade (flask-sock routes are ordinary Flask routes).
    _own_names = _own_host_names()
    _own_label = _own_host_label()
    _extra_hosts = _extra_allowed_hosts()

    @app.before_request
    def _check_host_header():  # type: ignore[no-redef]
        host = request.headers.get("Host", "")
        if _host_is_allowed(host, _own_names, _extra_hosts, _own_label):
            return None
        return error_response(
            "FORBIDDEN_HOST",
            "Open the Content Manager by the box's address (its IP, "
            "<name>.local, or http://dingus.box over USB).",
            status=403)

    # Cross-site request defence — the half the Host check cannot cover.
    # The allowlist must accept IP literals (the pairing QR and the Connect
    # screen are the LAN IP), so any website could still make its visitor's
    # browser hit http://<box-ip>:5000/...: spawn `update.sh check` and burn
    # the GitHub rate limit shared by the whole household, mint CSRF tokens,
    # or spend the pairing attempt budget with /?pair=000000. Browsers stamp
    # every request with Sec-Fetch-Site and a page cannot forge it, so refuse
    # 'cross-site'. Zero friction for real use:
    #   * header absent  -> allowed (curl, Retro Ripper, Safari < 16.4)
    #   * 'none'         -> allowed (camera-app QR scan, typed address,
    #                       home-screen app launch, bookmark)
    #   * same-origin / same-site -> allowed (the Content Manager itself)
    #   * a cross-site TOP-LEVEL navigation to a page (a link in a router's
    #     device list, a help article) still opens it — the user can see
    #     it; only embedding (iframe), fetch/XHR, forms and subresources are
    #     refused. One carrying a pairing code is bounced to the Connect
    #     page (admin_interface) so the code is spent only by a tap there.
    @app.before_request
    def _check_fetch_site():  # type: ignore[no-redef]
        if request.headers.get("Sec-Fetch-Site", "").lower() != "cross-site":
            return None
        if (request.method in ("GET", "HEAD")
                and request.headers.get("Sec-Fetch-Mode", "").lower() == "navigate"
                and request.headers.get("Sec-Fetch-Dest", "document").lower() == "document"):
            return None
        return error_response(
            "CROSS_SITE_REQUEST",
            "This request came from another website and was refused. Open "
            "the Content Manager directly by the box's address.",
            status=403)

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

    # ===== KIOSK RELOAD POKES =====
    #
    # The kiosk loads playlists exactly ONCE, at boot: main.cpp has a single
    # PlaylistLoader::load_playlists() call site in the whole C++ tree, with no
    # inotify watch, no poll and no SIGHUP handler. So every web write path
    # here returned a green toast while the TV kept showing the old content
    # until someone pulled the power — success in the browser, nothing on the
    # screen, and no way for the operator to tell the difference.
    #
    # The fix is the mechanism the settings restore path already proved: drop a
    # marker file the kiosk polls for (~1 Hz), which it deletes and then acts
    # on. Two properties make it the right shape here:
    #
    #   * it is one small write, so it cannot meaningfully fail or block, and
    #   * a kiosk binary that has never heard of a marker simply leaves the
    #     file alone. Shipping the web half FIRST is therefore safe, and is the
    #     order this must ship in — the marker is inert until the kiosk learns
    #     to read it, and no box regresses in the meantime.
    RELOAD_MARKERS = {
        "settings": "settings_reload_request",
        "playlists": "playlists_reload_request",
    }

    def _poke_kiosk_reload(kind: str) -> None:
        """Ask the running kiosk to re-read `kind` from disk.

        MUST NOT raise. Every caller has already completed the write the user
        asked for; turning a successful save into a 500 because a zero-byte
        marker could not be written would be strictly worse than the stale-UI
        bug this exists to fix. Failures are logged and swallowed — the
        operator's content is on disk either way, and a reboot still picks it
        up, which is exactly the old behaviour.
        """
        try:
            marker = RELOAD_MARKERS[kind]
            _atomic_write_text(data_dir / marker, str(time.time()))
        except Exception as e:
            print(f"Failed to poke kiosk for {kind} reload: {e}", file=sys.stderr)

    # ===== STORAGE PRECONDITIONS =====
    #
    # TMPDIR is redirected onto the SD card above (the tmpfs /tmp is far too
    # small for an 8 GB upload), so an import stages the whole ZIP there and
    # then extracts it into data/media — peak footprint roughly twice the ZIP.
    # Nothing checked whether that fit, and ENOSPC surfaced as a raw 500.
    #
    # A full card is worse than a failed import: the running kiosk writes
    # settings.json, RetroArch save files and paired-remote state to the same
    # volume, so an upload that fills it takes the appliance's own persistence
    # down with it. Hence the reserve below — we refuse an upload before the
    # last slice of the card is gone, not after.
    STORAGE_HEADROOM_BYTES = 512 * 1024 * 1024

    # Every multipart upload is spooled to the card by werkzeug (TMPDIR is
    # the SD card) and that spool lives until the request ends, so EVERY
    # upload route holds at least two copies at its peak:
    #   import-package    spool + extracted media (the ZIP is read in place)
    #   /admin/upload     spool + the staging copy that is renamed into place
    #   transcode routes  spool + the saved original during the request, then
    #                     the original + ffmpeg's .part until the encode ends
    # Video barely compresses, so two copies of the request size it is.
    IMPORT_PEAK_MULTIPLIER = 2

    def _storage_precondition(needed_bytes: int):
        """Return a 507 response tuple if `needed_bytes` will not fit, else None.

        Call this BEFORE touching request.files or request.form. Werkzeug parses
        the multipart body lazily, on first access to either — and parsing spools
        the upload to disk. Checking after that point has already consumed the
        space we are trying to protect, which is the whole point of answering
        early.
        """
        free = get_free_bytes(data_dir)
        if free is None or needed_bytes <= 0:
            # Unknown free space (no statvfs) or unknown request size (chunked
            # transfer sends no Content-Length). Refusing on "unknown" would
            # break dev machines and legitimate clients; let it through and let
            # the extraction caps do their job.
            return None
        if free - STORAGE_HEADROOM_BYTES >= needed_bytes:
            return None
        return error_response(
            "INSUFFICIENT_STORAGE",
            "Not enough free space on this box for that upload.",
            status=507,
            details={
                "needed_gb": round(needed_bytes / BYTES_PER_GB, 2),
                "free_gb": round(free / BYTES_PER_GB, 2),
            },
        )

    @app.errorhandler(413)
    def _payload_too_large(e):  # type: ignore[no-redef]
        """Return the standard error envelope when MAX_CONTENT_LENGTH trips.

        Werkzeug's own 413 body is HTML, so the Content Manager's fetch wrapper
        found no JSON, fell back to response.statusText, and showed the operator
        a bare "REQUEST ENTITY TOO LARGE" with no mention of a size limit or
        what it is.
        """
        max_mb = int((app.config.get("MAX_CONTENT_LENGTH") or 0) // (1024 * 1024))
        return error_response(
            "PAYLOAD_TOO_LARGE",
            f"Upload is larger than this box accepts ({max_mb} MB maximum).",
            status=413,
            details={"max_upload_mb": max_mb},
        )

    # Sweep crashed transcode staging files. run_transcode_job encodes
    # into media_dir/<name>.mp4.part and os.replace()s onto the final
    # name only on success; a restart or power cut mid-encode leaves the
    # .part behind. It is invisible to the *.mp4 globs, so unlike the old
    # encode-in-place scheme it can't reach a playlist — but it still
    # holds gigabytes. Same rule as the upload_temp sweep above: no
    # encode survives a restart, so at startup every .part is garbage.
    if media_dir.is_dir():
        _part_bytes = 0
        for _part in media_dir.rglob("*.part"):
            try:
                _part_bytes += _part.stat().st_size
                _part.unlink()
            except OSError:
                pass
        if _part_bytes:
            print(f"[media] swept {_part_bytes / (1024 * 1024):.1f} MB of "
                  "interrupted transcode staging (.part) files", flush=True)

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

    MEDIA_VIDEO_EXTENSIONS = ('.mp4', '.mkv', '.avi', '.mov', '.webm')

    @app.post("/admin/upload")
    @require_csrf
    def upload_media():  # type: ignore[no-redef]
        """Upload video file."""
        # Ahead of request.files for the reason given in _storage_precondition:
        # touching it spools the body to the card. TWO copies here, not one:
        # werkzeug's spool lives until the request ends, and
        # _staged_save_upload copies it into a staging file beside the target
        # before the rename (the rename itself duplicates nothing).
        _no_space = _storage_precondition(
            (request.content_length or 0) * IMPORT_PEAK_MULTIPLIER)
        if _no_space:
            return _no_space

        if "file" not in request.files:
            return error_response("VALIDATION_ERROR", "File field required")
        f = request.files["file"]

        # Sanitize filename to prevent path traversal, and accept only the
        # video containers the media library actually lists (list_media scans
        # exactly these). Anything else landing in data/media was invisible to
        # the operator — unlistable, undeletable from the UI — and served no
        # purpose; the web UI never uses this raw endpoint (it goes through
        # /admin/smart-upload), so this narrows nothing a real flow needs.
        try:
            safe_filename = _sanitize_filename(
                f.filename, allowed_extensions=list(MEDIA_VIDEO_EXTENSIONS))
        except ValueError as e:
            return error_response("VALIDATION_ERROR", str(e))

        out = media_dir / safe_filename
        # Ensure path stays within media_dir
        out_resolved = out.resolve()
        media_dir_resolved = media_dir.resolve()
        if not _is_within(out_resolved, media_dir_resolved):
            return error_response("VALIDATION_ERROR", "Invalid path")

        _staged_save_upload(f, out)
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

    # Framing policy for transcoded masters.
    #   'crop' — scale to FILL the 4:3 frame and trim the overflow (the
    #            original CRT look; a widescreen or vertical source loses
    #            its edges). Historical behavior, so it stays the default
    #            for API callers that don't send the field.
    #   'fit'  — scale the WHOLE frame inside the 4:3 canvas and pad the
    #            rest with black bars (nothing trimmed — vertical phone
    #            clips get side borders, widescreen gets top/bottom bars).
    # Both modes respect the no-upscale guard in run_transcode_job.
    TRANSCODE_FIT_MODES = ('crop', 'fit')
    DEFAULT_FIT_MODE = 'crop'

    # Store for tracking transcoding jobs (in-memory, cleared on restart)
    transcode_jobs: dict = {}

    # Output-name reservations for in-flight uploads. A transcode's output
    # only appears at its final os.replace — minutes after the request chose
    # the name — so "pick the first name that doesn't exist() yet" let two
    # uploads of the same file (two phones, a double-submit, or a second job
    # queued behind the encoder semaphore) both choose clip.mp4: the second
    # publish silently replaced the first video. A name is free only if it
    # is neither on disk NOR held by a job still running; check-and-reserve
    # happens under one lock. In-memory is enough: a restart kills every
    # in-flight encode, and nothing on disk is ever a placeholder, so the
    # kiosk and the library listing only ever see finished videos.
    _media_name_lock = threading.Lock()
    _reserved_media_names: set = set()

    def _reserve_media_output(preferred_name: str) -> Path:
        """Reserve media_dir/<preferred_name> or the first free <stem>_N.mp4."""
        base = Path(preferred_name).stem
        with _media_name_lock:
            name, counter = preferred_name, 1
            while name in _reserved_media_names or (media_dir / name).exists():
                name = f"{base}_{counter}.mp4"
                counter += 1
            _reserved_media_names.add(name)
        return media_dir / name

    def _release_media_output(path: Path) -> None:
        with _media_name_lock:
            _reserved_media_names.discard(path.name)


    # x264 encoder tier, resolved per-board at runtime.
    #
    # Pi 5: CRF 23 matches the Retro Ripper's visual-quality target
    # (its export pipeline encodes CRF 23), so a master transcoded on the
    # box looks the same as one exported from the desktop tool. veryfast
    # rather than the ripper's desktop 'medium' preset: CRF targets
    # constant quality, so a slower preset mostly buys smaller files, and
    # two semaphore-slots of 'medium' plus the kiosk's own software video
    # decode (~1 core at 1080p) could saturate the 4 cores and stutter
    # playback mid-upload. veryfast keeps that headroom.
    #
    # Pi 4 (and unknown boards, per the "performance envelope is the
    # Pi 4B's" rule): ultrafast/28 — one libx264 encode already saturates
    # a Pi 4B sharing 1.5 GB RAM with the kiosk. Env-overridable for
    # experiments without a release.
    #
    # _PI_MODEL is the POLICY board: the real one, unless the TEST-ONLY
    # MDB_PLATFORM_POLICY_OVERRIDE=pi4 makes a Pi 5 rehearse Pi 4B policy
    # (see _policy_pi_model). Nothing below branches on hardware facts.
    _REAL_PI_MODEL = _detect_pi_model()
    _PI_MODEL, _policy_log_line = _policy_pi_model(
        _REAL_PI_MODEL, os.getenv(PLATFORM_POLICY_OVERRIDE_ENV))
    if _policy_log_line:
        print(f"WARNING: {_policy_log_line}", file=sys.stderr)
    app.config["PLATFORM_POLICY_OVERRIDE"] = (
        _PI_MODEL if _PI_MODEL != _REAL_PI_MODEL else None)

    # Cap concurrent ffmpeg encodes, per board at runtime. One libx264 encode
    # already saturates a Pi 4B (4 cores / 1.5 GB shared with the kiosk and
    # optionally the Media Browser Docker stack), so a second concurrent
    # encode there only thrashes — Pi 4B and unknown boards get 1 (the
    # performance envelope is the Pi 4B's). The Pi 5 has the headroom for 2,
    # letting a second upload start while the first finishes. Excess jobs
    # block in run_transcode_job() and report 'queued' to the UI.
    # MAGIC_MAX_TRANSCODES overrides on any board.
    _max_transcodes = _max_transcodes_for(
        _PI_MODEL, os.getenv("MAGIC_MAX_TRANSCODES"))
    app.config["MAX_TRANSCODES"] = _max_transcodes
    _TRANSCODE_SEMAPHORE = threading.Semaphore(_max_transcodes)
    if _PI_MODEL == 'pi5':
        _tier_preset, _tier_crf = 'veryfast', '23'
    else:
        _tier_preset, _tier_crf = 'ultrafast', '28'
    _X264_PRESET = os.getenv('MAGIC_X264_PRESET', _tier_preset)
    _X264_CRF = os.getenv('MAGIC_X264_CRF', _tier_crf)

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
        terminal_states = {"complete", "completed", "success", "error", "failed", "cancelled", "canceled"}
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

    def _probe_source_dimensions(file_path: Path):
        """Return (width, height) of the first video stream, or None.

        Best-effort: any probe failure returns None and the caller falls
        back to the preset dimensions unchanged (pre-guard behavior)."""
        try:
            result = subprocess.run([
                'ffprobe', '-v', 'error', '-select_streams', 'v:0',
                '-show_entries', 'stream=width,height', '-of', 'csv=p=0',
                str(file_path)
            ], capture_output=True, text=True, timeout=30)
            if result.returncode != 0:
                return None
            parts = result.stdout.strip().split(',')
            w, h = int(parts[0]), int(parts[1])
            return (w, h) if w > 0 and h > 0 else None
        except Exception:
            return None

    def run_transcode_job(job_id: str, input_path: Path, output_path: Path, resolution: str, normalize_audio: bool = False, fit_mode: str = DEFAULT_FIT_MODE):
        """Background thread function to run FFmpeg transcoding."""
        job = transcode_jobs[job_id]
        res = TRANSCODE_RESOLUTIONS.get(resolution, TRANSCODE_RESOLUTIONS[DEFAULT_TRANSCODE])
        width, height = res['width'], res['height']

        # Never upscale. A preset is a MAXIMUM master size, not a canvas to
        # inflate into: encoding a 640x480 source at 960x720 adds zero detail,
        # doubles the file, and bakes in a lossy re-encode of an upscale.
        # Dimensions floor to even for libx264.
        src_dims = _probe_source_dimensions(input_path)

        if fit_mode == 'fit':
            # Whole frame INSIDE the 4:3 canvas, black bars fill the rest.
            # The canvas stays at the chosen preset size (a consistent 4:3
            # master); the CONTENT is scaled by min(fit, 1.0) — never
            # enlarged. A source smaller than the canvas sits centered at
            # its native size.
            if src_dims:
                src_w, src_h = src_dims
                content_scale = min(width / src_w, height / src_h, 1.0)
                content_w = max(2, int(src_w * content_scale) & ~1)
                content_h = max(2, int(src_h * content_scale) & ~1)
                vf = (f'scale={content_w}:{content_h},'
                      f'pad={width}:{height}:(ow-iw)/2:(oh-ih)/2')
            else:
                # Probe failed — let ffmpeg compute the fit. May upscale a
                # tiny source up to the canvas; acceptable fallback for a
                # file we couldn't inspect.
                vf = (f'scale={width}:{height}:force_original_aspect_ratio=decrease,'
                      f'pad={width}:{height}:(ow-iw)/2:(oh-ih)/2')
        else:
            # 'crop': scale to FILL the 4:3 frame, center-crop the overflow
            # (no bars, no distortion). Never upscale: shrink the target box
            # to fit inside the source while preserving the preset's aspect
            # policy, so the crop semantics still apply to sources whose
            # aspect differs.
            if src_dims:
                src_w, src_h = src_dims
                if src_w < width or src_h < height:
                    shrink = min(src_w / width, src_h / height)
                    width = max(2, int(width * shrink) & ~1)
                    height = max(2, int(height * shrink) & ~1)
            vf = (f'scale={width}:{height}:force_original_aspect_ratio=increase,'
                  f'crop={width}:{height}')

        # Bound concurrent encodes. Each transcode is a full libx264 encode;
        # on a Pi 4 sharing RAM/CPU with the kiosk (and possibly the Media
        # Browser Docker stack), several phones uploading at once would spawn
        # unbounded parallel ffmpeg processes and thrash/OOM. Block here until
        # a slot frees — the job sits in 'queued' rather than competing.
        job['status'] = 'queued'
        job['message'] = 'Queued — waiting for an encoder slot...'
        _TRANSCODE_SEMAPHORE.acquire()
        stderr_file = None

        # Encode into a staging name BESIDE the final path, and os.replace()
        # onto it only when ffmpeg exits 0. Encoding straight into
        # media_dir/<name>.mp4 meant the growing, moov-less (+faststart
        # writes moov at finalize) file was listed by /admin/media and
        # addable to a playlist for the whole multi-minute encode — and a
        # service restart or power cut mid-encode stranded it there forever,
        # indistinguishable from a real video. The .part suffix keeps it out
        # of every *.mp4 glob; same-directory staging guarantees os.replace
        # is an atomic same-filesystem rename wherever media_dir lives.
        # Crashed leftovers are swept at startup alongside upload_temp.
        # The job id in the staging name keeps two encoders from ever sharing
        # one .part (one's error path used to unlink the other's encode);
        # the name still ends in .part, so the sweep and globs are unchanged.
        staging_path = output_path.with_name(
            f"{output_path.name}.{job_id.replace('-', '')[:12]}.part")

        # Build FFmpeg command with the framing filter resolved above
        # (crop = fill the frame, fit = whole frame + black bars).
        ffmpeg_cmd = [
            'ffmpeg', '-y',
            '-i', str(input_path),
            '-vf', vf,
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
            # Per-board tier (see _X264_PRESET/_X264_CRF above): Pi 5 gets
            # the Retro Ripper's CRF 23 quality target, Pi 4 keeps
            # ultrafast/28.
            '-preset', _X264_PRESET,
            '-crf', _X264_CRF,
            '-c:a', 'aac',
            # 192k matches the Retro Ripper's AAC bitrate (was 128k here —
            # the one audio setting that diverged between the pipelines).
            '-b:a', '192k',
            '-ar', '48000',
            '-movflags', '+faststart',
            '-progress', 'pipe:1',
            '-nostats',
            # Explicit muxer: ffmpeg normally infers it from the output
            # extension, and the .part staging name would defeat that.
            '-f', 'mp4',
            str(staging_path)
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
                # Atomic publish: the finished encode appears in the media
                # library all at once, or not at all.
                os.replace(staging_path, output_path)
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
                    staging_path.unlink()
                except Exception:
                    pass

        except Exception as e:
            job['status'] = 'error'
            job['message'] = str(e)
            try:
                staging_path.unlink()
            except Exception:
                pass
        finally:
            # Always remove the stderr temp file.
            if stderr_file is not None:
                try:
                    stderr_file.close()
                    os.unlink(stderr_file.name)
                except Exception:
                    pass
            _TRANSCODE_SEMAPHORE.release()
            # After the publish (if any): from here the name is protected by
            # the file existing, or free again because the encode failed.
            _release_media_output(output_path)

    @app.post("/admin/upload-and-transcode")
    @require_csrf
    def upload_and_transcode():  # type: ignore[no-redef]
        """Upload video file and transcode it on the Pi."""
        # Two copies: werkzeug's spool + the saved original during the
        # request; then the original sits in upload_temp for the whole job
        # while ffmpeg writes the .part next to the media library.
        _no_space = _storage_precondition(
            (request.content_length or 0) * IMPORT_PEAK_MULTIPLIER)
        if _no_space:
            return _no_space

        if "file" not in request.files:
            return error_response("VALIDATION_ERROR", "File field required")

        f = request.files["file"]
        resolution = request.form.get("resolution", DEFAULT_TRANSCODE)
        fit_mode = request.form.get("fit_mode", DEFAULT_FIT_MODE)
        # `normalize_audio` arrives as the string "true"/"false" (FormData
        # POST). Coerce to bool. Default ON because phone-uploaded clips
        # almost always have inconsistent levels — we'd rather opt-out
        # than opt-in for the typical user flow.
        normalize_audio = (request.form.get("normalize_audio", "true").lower()
                           == "true")

        if resolution not in TRANSCODE_RESOLUTIONS:
            return error_response("VALIDATION_ERROR", f"Invalid resolution: {resolution}")
        if fit_mode not in TRANSCODE_FIT_MODES:
            return error_response("VALIDATION_ERROR", f"Invalid fit_mode: {fit_mode}")

        # Create unique job ID
        job_id = str(uuid.uuid4())

        # Sanitize filename
        try:
            original_name = _sanitize_filename(f.filename)
        except ValueError as e:
            return error_response("VALIDATION_ERROR", str(e))

        # Save to temp location
        temp_input = upload_temp_dir / f"transcode_input_{job_id}_{original_name}"

        # Unique output filename — reserved against in-flight jobs too, not
        # just files already on disk (see _reserve_media_output).
        output_path = _reserve_media_output(Path(original_name).stem + ".mp4")
        output_name = output_path.name

        # Save uploaded file
        try:
            f.save(str(temp_input))
        except BaseException:
            _release_media_output(output_path)
            raise

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
            args=(job_id, temp_input, output_path, resolution, normalize_audio,
                  fit_mode)
        )
        thread.daemon = True
        thread.start()

        return success_response(data={'job_id': job_id}, message="Transcoding started")

    @app.get("/admin/transcode-status/<job_id>")
    def transcode_status(job_id):  # type: ignore[no-redef]
        """Get status of a transcoding job.

        Jobs are in-memory on purpose: the ffmpeg encode runs inside this
        service and dies with it (and its .part is swept at startup), so a
        job cannot outlive a restart. The 404 is therefore final, and says
        so — manager.js turns it into "upload it again" rather than polling.
        """
        if job_id not in transcode_jobs:
            return error_response(
                "NOT_FOUND",
                "Job not found — the box may have restarted while converting. "
                "Please upload the file again.",
                status=404)

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

            # Check if video is already compatible.
            #
            # "Compatible" is NOT an exact dimension match: presets are
            # MAXIMUM master sizes (see TRANSCODE_RESOLUTIONS), so a
            # smaller-or-equal source with the same aspect ratio has nothing
            # to gain from re-encoding — transcoding would only add
            # generation loss. This keeps legacy 640x480 masters re-uploaded
            # under the 960x720 default as byte-identical direct copies.
            # A source with a DIFFERENT aspect still transcodes so the
            # preset's center-crop policy applies (capped at source size by
            # run_transcode_job's no-upscale guard).
            fits_target = (0 < width <= target_w and 0 < height <= target_h)
            same_aspect = (height > 0 and
                           abs(width / height - target_w / target_h) < 0.02)
            is_correct_resolution = fits_target and same_aspect
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
                if not fits_target:
                    reasons.append(f'Resolution {width}x{height} exceeds {target_w}x{target_h}')
                elif not same_aspect:
                    reasons.append(f'Aspect of {width}x{height} differs from {target_w}x{target_h}')
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
        # Two copies on every branch: werkzeug's spool + the probe copy during
        # the request, then the original + ffmpeg's .part if it transcodes
        # (the direct branch renames the probe copy, adding nothing).
        _no_space = _storage_precondition(
            (request.content_length or 0) * IMPORT_PEAK_MULTIPLIER)
        if _no_space:
            return _no_space

        if "file" not in request.files:
            return error_response("VALIDATION_ERROR", "File field required")

        f = request.files["file"]
        resolution = request.form.get("resolution", DEFAULT_TRANSCODE)
        fit_mode = request.form.get("fit_mode", DEFAULT_FIT_MODE)
        # See upload_and_transcode for normalize_audio default rationale.
        # Default ON here too so a phone upload of an already-720p clip
        # still gets its audio levels fixed even when the video itself
        # doesn't need re-encoding.
        normalize_audio = (request.form.get("normalize_audio", "true").lower()
                           == "true")

        if resolution not in TRANSCODE_RESOLUTIONS:
            return error_response("VALIDATION_ERROR", f"Invalid resolution: {resolution}")
        if fit_mode not in TRANSCODE_FIT_MODES:
            return error_response("VALIDATION_ERROR", f"Invalid fit_mode: {fit_mode}")

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
            # Unique filename — reserved against in-flight transcodes too
            # (see _reserve_media_output); released once the move has
            # published it, after which the file itself holds the name.
            output_path = _reserve_media_output(output_name)
            output_name = output_path.name

            # Move file to media folder
            try:
                shutil.move(str(temp_input), str(output_path))
            finally:
                _release_media_output(output_path)

            return success_response(data={
                'action': 'direct',
                'needs_transcode': False,
                'output_path': str(output_path.relative_to(data_dir.parent)),
                'output_filename': output_name,
                'probe': probe_result
            }, message="File uploaded directly (already compatible)")

        else:
            # Needs transcoding - start transcode job. Unique output name,
            # reserved until the job finishes (see _reserve_media_output).
            output_path = _reserve_media_output(Path(original_name).stem + ".mp4")
            output_name = output_path.name

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
                args=(job_id, temp_input, output_path, resolution,
                      normalize_audio, fit_mode)
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

    # ===== OTA UPDATE MANAGEMENT =====

    # Path to update script
    # data_dir is /opt/magic_dingus_box/magic_dingus_box_cpp/data
    # update.sh is at /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/update.sh
    UPDATE_SCRIPT = data_dir.parent / "scripts" / "update.sh"

    # Parse-state cache for update jobs. NOT the source of truth: the job
    # itself runs detached (detached_jobs.py — its own systemd unit on the
    # Pi, so a magic-dingus-web restart no longer kills an OTA mid-rsync)
    # and its progress is re-derived from its on-disk log. A Flask that
    # restarted mid-job rebuilds this entry from offset 0 on the first poll.
    update_jobs: dict = {}
    _update_jobs_lock = threading.Lock()

    # OTA install/rollback and Media Browser setup: launched detached, state
    # on disk outside the install tree (see detached_jobs.default_state_dir).
    # The launcher is resolved per launch: a test app never spawns systemd
    # units even on a CI runner that has systemd-run + passwordless sudo.
    detached = DetachedJobs(
        _default_job_state_dir(data_dir),
        mode_resolver=lambda: "popen" if app.testing else "auto")

    # One maintenance job at a time, ACROSS kinds. Two OTAs share update.sh's
    # TEMP_DIR (/tmp/magic_update — the second's `rm -rf` pulls the first's
    # download out from under it) and race the same rsync --delete; an OTA
    # and a Media Browser setup both restart services and rewrite files the
    # other reads. The lock only serialises check-and-launch inside this
    # process; "is one running" is answered from disk + liveness, so it also
    # holds across a Flask restart. update.sh flocks as well, for runs that
    # don't come through here.
    _MAINTENANCE_KINDS = ("ota-install", "ota-rollback", "mb-setup",
                          vpn_settings.JOB_KIND)
    _MAINTENANCE_LABELS = {
        "ota-install": "A software update",
        "ota-rollback": "A rollback",
        "mb-setup": "Media Browser setup",
        vpn_settings.JOB_KIND: "A VPN country change",
    }
    _maintenance_launch_lock = threading.Lock()

    def _maintenance_busy_response(running_kind=None):
        label = _MAINTENANCE_LABELS.get(running_kind, "Another maintenance task")
        resp, status = error_response(
            "JOB_ALREADY_RUNNING",
            f"{label} is already running on this box. Wait for it to "
            "finish, then try again.",
            status=409)
        resp.headers["Retry-After"] = "30"
        return resp, status

    def _maintenance_precheck():
        """409 response if a maintenance job is running, else None. For
        routes that must refuse BEFORE side effects (Media Browser setup
        rewrites services/.env, which a running setup is reading)."""
        running = detached.active(_MAINTENANCE_KINDS)
        return _maintenance_busy_response(running[1]) if running else None

    def _launch_maintenance_job(kind: str, argv: list, **kwargs):
        """Return (job_id, None), or (None, error_response) when another
        maintenance job is running (409) or the launch failed (500)."""
        if not _maintenance_launch_lock.acquire(blocking=False):
            return None, _maintenance_busy_response()
        try:
            running = detached.active(_MAINTENANCE_KINDS)
            if running:
                return None, _maintenance_busy_response(running[1])
            try:
                return detached.launch(kind, argv, **kwargs), None
            except (OSError, ValueError) as e:
                return None, error_response(
                    "INTERNAL_ERROR", f"Could not start the job: {e}", status=500)
        finally:
            _maintenance_launch_lock.release()

    # One Network Doctor run at a time. Each run is a ~20-45 s probe ladder
    # (curl, DNS, ping) and every GET used to spawn its own — a few open tabs,
    # a double-click, or anyone on the LAN looping the URL stacked them up on
    # a Pi 4B that has no CPU to spare. A request that arrives while a run is
    # in flight waits for THAT run and returns its result (same answer, one
    # set of probes); if it can't get one it gets a 429.
    _doctor_lock = threading.Lock()
    _doctor_state: dict = {"seq": 0, "result": None}
    DOCTOR_WAIT_SECONDS = 60

    @app.get("/admin/network/doctor")
    def network_doctor():  # type: ignore[no-redef]
        if _doctor_lock.acquire(blocking=False):
            try:
                resp, status = _run_network_doctor()
                try:
                    body = resp.get_json()
                except Exception:
                    body = None
                _doctor_state["result"] = (body, status) if body is not None else None
                _doctor_state["seq"] += 1
                return resp, status
            finally:
                _doctor_lock.release()

        seq_at_arrival = _doctor_state["seq"]
        if _doctor_lock.acquire(timeout=DOCTOR_WAIT_SECONDS):
            _doctor_lock.release()
            result = _doctor_state["result"]
            if _doctor_state["seq"] != seq_at_arrival and result is not None:
                return jsonify(result[0]), result[1]
        resp, status = error_response(
            "BUSY", "A network test is already running — try again in a moment.",
            status=429)
        resp.headers["Retry-After"] = "30"
        return resp, status

    def _run_network_doctor():
        """Run the Network Doctor ladder and return its verdict.

        Deliberately NOT behind the Media Browser gate: network problems hit
        games-only boxes and unprovisioned units too, and this endpoint is
        most valuable precisely when nothing else works. Read-only probes,
        bounded at ~20s by the script itself; the subprocess timeout is the
        backstop. Runs synchronously — the UI shows a spinner for the
        duration; concurrent requests share one run (see _doctor_lock).
        """
        doctor = data_dir.parent / "scripts" / "network_doctor.sh"
        if not doctor.exists():
            return error_response("NOT_AVAILABLE", "network_doctor.sh not found", status=500)
        try:
            # Invoked via bash rather than executed directly: the script
            # shipped mode 0644 in the v1.9.6/v1.9.7 tarballs, which turned
            # the owner's only network diagnostic into a 500 reading
            # "[Errno 13] Permission denied: .../network_doctor.sh". The
            # mode is fixed in git as of v1.9.8; this makes it stop
            # mattering.
            result = subprocess.run(
                ["/bin/bash", str(doctor), "--json"],
                capture_output=True, text=True, timeout=45
            )
            # The script exits 1 when any rung failed — that is a RESULT,
            # not an error; the JSON on stdout is valid either way.
            payload = json.loads(result.stdout)
            return jsonify(payload), 200
        except subprocess.TimeoutExpired:
            return error_response("TIMEOUT", "Network test timed out", status=504)
        except json.JSONDecodeError:
            return error_response(
                "PARSE_ERROR",
                strip_ansi(result.stderr)[:300] or "Doctor produced no parseable output",
                status=500)
        except Exception as e:
            return error_response("INTERNAL_ERROR", str(e), status=500)

    # ===== BOX HEALTH (verify_box.sh) + DIAGNOSTICS BUNDLE =====
    #
    # Box health runs the box's own pre-ship acceptance test and shows its
    # verdict in plain language — the same answer a technician gets over
    # SSH, without SSH. The diagnostics bundle is the zip support asks for
    # first. Neither is behind the Media Browser gate (games-only boxes
    # need support too); only the optional --with-services sweep is, since
    # it inspects the Movies stack. See box_health.py / diagnostics.py.
    install_dir = data_dir.parent.parent

    def _known_secrets() -> list:
        return diagnostics.known_secret_values(
            install_dir, data_dir, extra_paths=[_tmdb_key_file()])

    health_runner = box_health.HealthRunner(
        script=data_dir.parent / "scripts" / "verify_box.sh",
        cache_path=data_dir / box_health.CACHE_NAME,
        redactor_factory=lambda: Redactor(_known_secrets()),
    )
    app.config["HEALTH_RUNNER"] = health_runner

    @app.post("/admin/health/run")
    @require_csrf
    def health_run():  # type: ignore[no-redef]
        body = request.get_json(silent=True) or {}
        with_services = body.get("with_services") is True
        if with_services and not _media_browser_unlocked():
            return _media_browser_locked_response()
        runner = app.config["HEALTH_RUNNER"]
        started = runner.start(with_services=with_services)
        data = runner.status()
        data["already_running"] = not started
        return success_response(
            data=data,
            message="Health check started" if started else "A health check is already running")

    @app.get("/admin/health/status")
    def health_status():  # type: ignore[no-redef]
        data = app.config["HEALTH_RUNNER"].status()
        data["services_check_available"] = _media_browser_unlocked()
        return success_response(data=data)

    _diag_lock = threading.Lock()

    @app.route("/admin/diagnostics/bundle", methods=["GET", "POST"])
    def diagnostics_bundle():  # type: ignore[no-redef]
        # Read-only, but it shells out ~25 times; one build at a time.
        if not _diag_lock.acquire(blocking=False):
            resp, status = error_response(
                "BUSY", "A diagnostics file is already being prepared — try "
                "again in a moment.", status=429)
            resp.headers["Retry-After"] = "15"
            return resp, status
        try:
            builder = diagnostics.BundleBuilder(
                data_dir=data_dir, install_dir=install_dir,
                known_secrets=_known_secrets())
            handle = diagnostics.build_to_tempfile(builder)
        except Exception as e:
            return error_response(
                "INTERNAL_ERROR", f"Could not build the diagnostics file: {type(e).__name__}",
                status=500)
        finally:
            _diag_lock.release()
        name = diagnostics.bundle_filename(get_device_info().get("device_name", "box"))
        return send_file(handle, mimetype="application/zip", as_attachment=True,
                         download_name=name, max_age=0)

    @app.get("/admin/update/version")
    def get_version():  # type: ignore[no-redef]
        """Get current installed version."""
        return success_response(data={
            "version": get_app_version(),
            "device_name": get_device_info().get("device_name", "Unknown")
        })

    # `update.sh check` is a GitHub API call (60/hour unauthenticated, per
    # public IP — shared by every box behind the same router) plus a process
    # spawn, and the endpoint is a plain GET: a few open tabs, the Settings
    # tab's post-update verify loop, or a hostile web page making the
    # browser hit http://<box-ip>:5000/admin/update/check in a loop each
    # spawned its own. One check at a time; concurrent callers wait for it
    # and share its answer; a SUCCESSFUL answer is reused briefly. Every
    # install/rollback start and finish drops the cache, so the verify loop
    # never sees a pre-update current_version (and both end with a web
    # restart, which empties it anyway).
    UPDATE_CHECK_CACHE_SECONDS = 30
    _update_check_lock = threading.Lock()
    _update_check_cache: dict = {"ts": 0.0, "body": None}

    def _invalidate_update_check_cache() -> None:
        _update_check_cache["body"] = None

    def _cached_update_check():
        body = _update_check_cache["body"]
        if body is not None and time.monotonic() - _update_check_cache["ts"] < UPDATE_CHECK_CACHE_SECONDS:
            return jsonify(body), 200
        return None

    @app.get("/admin/update/check")
    def check_for_update():  # type: ignore[no-redef]
        """Check if an update is available from GitHub."""
        if not UPDATE_SCRIPT.exists():
            return error_response(
                "UPDATE_NOT_AVAILABLE",
                "Update script not found. Run deploy with --build first.",
                status=500
            )
        if (cached := _cached_update_check()) is not None:
            return cached
        if not _update_check_lock.acquire(timeout=75):
            resp, status = error_response(
                "BUSY", "An update check is already running — try again in a moment.",
                status=429)
            resp.headers["Retry-After"] = "10"
            return resp, status
        try:
            if (cached := _cached_update_check()) is not None:
                return cached
            return _run_update_check()
        finally:
            _update_check_lock.release()

    def _run_update_check():
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
                _update_check_cache["body"] = response_data
                _update_check_cache["ts"] = time.monotonic()
                return jsonify(response_data), 200
            else:
                error_msg = strip_ansi(result.stderr) or strip_ansi(result.stdout) or "Unknown error"
                # The overwhelmingly common cause on a fresh unit is simply
                # that it has not joined Wi-Fi yet, so say so instead of
                # handing back the script's progress chatter.
                if not _has_internet():
                    error_msg = ("No internet connection. Join Wi-Fi from the "
                                 "kiosk's Settings screen, then check again.\n\n" + error_msg)
                return error_response("UPDATE_CHECK_FAILED", error_msg, status=500)

        except subprocess.TimeoutExpired:
            return error_response("TIMEOUT", "Update check timed out", status=504)
        except json.JSONDecodeError as e:
            return error_response("PARSE_ERROR", f"Failed to parse update info: {e}", status=500)
        except Exception as e:
            return error_response("INTERNAL_ERROR", str(e), status=500)

    # Update channel (stable | beta). update.sh owns the file
    # (<install>/config/update_channel) and its default-to-stable rule, so
    # both endpoints go through `update.sh channel` — the same command an
    # owner runs over SSH — rather than a second reader/writer here. stdout
    # is exactly one word by contract.
    def _run_update_channel(args: list) -> str:
        result = subprocess.run(
            [str(UPDATE_SCRIPT), "channel", *args],
            capture_output=True, text=True, timeout=15)
        lines = (result.stdout or "").strip().splitlines()
        channel = lines[-1].strip() if lines else ""
        if result.returncode != 0 or channel not in _OTA_CHANNELS:
            raise RuntimeError(strip_ansi(result.stderr or "").strip()
                               or "update.sh channel failed")
        return channel

    @app.get("/admin/update/channel")
    def get_update_channel():  # type: ignore[no-redef]
        """The box's OTA update channel: "stable" (default) or "beta"."""
        if not UPDATE_SCRIPT.exists():
            return error_response("UPDATE_NOT_AVAILABLE", "Update script not found", status=500)
        try:
            channel = _run_update_channel([])
        except (OSError, subprocess.TimeoutExpired, RuntimeError) as e:
            return error_response("INTERNAL_ERROR", f"Could not read the update channel: {e}",
                                  status=500)
        return success_response(data={"channel": channel})

    @app.post("/admin/update/channel")
    @require_csrf
    def set_update_channel():  # type: ignore[no-redef]
        """Switch the OTA update channel. Body: {"channel": "stable"|"beta"}.

        No password or PIN by owner decision (same trust model as Install
        Update itself, which is strictly more powerful). Beta only widens
        which RELEASES of this project's own repo are offered; the install
        path's repo/tag pinning is unchanged.
        """
        if not UPDATE_SCRIPT.exists():
            return error_response("UPDATE_NOT_AVAILABLE", "Update script not found", status=500)
        data = request.get_json(silent=True)
        channel = data.get("channel") if isinstance(data, dict) else None
        if not isinstance(channel, str) or channel not in _OTA_CHANNELS:
            return error_response("VALIDATION_ERROR", "channel must be \"stable\" or \"beta\"")
        try:
            channel = _run_update_channel([channel])
        except (OSError, subprocess.TimeoutExpired, RuntimeError) as e:
            return error_response("INTERNAL_ERROR", f"Could not change the update channel: {e}",
                                  status=500)
        _invalidate_update_check_cache()  # the next check must use the new channel
        return success_response(data={"channel": channel},
                                message=f"Update channel set to {channel}")

    def _new_update_job_view(version: Optional[str]) -> dict:
        return {
            'status': 'running',
            'stage': 'preparing',
            'progress': 0,
            'message': 'Starting update...',
            'version': version,
            'new_version': None,
            '_cursor': 0,
            '_recent': collections.deque(maxlen=20),
            '_pruner_ts': time.time(),
        }

    def _refresh_update_job(job_id: str, job: dict) -> None:
        """Advance `job` with whatever the detached update.sh has written
        since the last poll (its stdout+stderr, merged, in the job log).

        Merged is fine: the parser ignores any non-JSON line, exactly as the
        old in-process pipe reader did. Driven by status polls rather than a
        reader thread, so there is nothing to lose on a Flask restart — a
        fresh process just re-parses from byte 0.
        """
        if job['status'] in ('complete', 'error') and job.get('_finished'):
            return
        result = detached.read(job_id, job['_cursor'])
        if result is None:
            return
        lines, job['_cursor'], state, rc = result
        for line in lines:
            line = line.strip()
            if not line:
                continue
            # Stripped at CAPTURE time: on failure this deque becomes
            # job['message'], which the Settings tab renders verbatim — the
            # same raw-ANSI-in-the-red-box leak the check endpoint already
            # fixed with strip_ansi. The JSON parse below is unaffected
            # (progress lines carry no colour).
            job['_recent'].append(strip_ansi(line))
            try:
                progress_data = json.loads(line)
            except json.JSONDecodeError:
                continue  # Non-JSON output, ignore
            if not isinstance(progress_data, dict):
                continue
            job['stage'] = progress_data.get('stage', job['stage'])
            job['progress'] = progress_data.get('progress', job['progress'])
            job['message'] = progress_data.get('message', job['message'])
            if progress_data.get('stage') == 'complete':
                job['status'] = 'complete'
                job['new_version'] = progress_data.get('new_version', job.get('version'))
            elif not progress_data.get('ok', True):
                job['status'] = 'error'
                err = progress_data.get('error')
                job['message'] = (err.get('message', 'Unknown error')
                                  if isinstance(err, dict) else 'Unknown error')

        if state == 'running':
            return
        job['_finished'] = True
        job['_pruner_ts'] = time.time()
        _invalidate_update_check_cache()  # VERSION may have just changed
        if job['status'] == 'complete':
            return
        tail = "\n".join(job['_recent'])
        if state == 'lost':
            job['status'] = 'error'
            job['message'] = ("The update process stopped unexpectedly (was the "
                              "box restarted?). Check the version shown here, "
                              "then retry the update or roll back."
                              + (f"\n\n{tail[-400:]}" if tail else ""))
        elif job['status'] != 'error':
            job['status'] = 'error'
            job['message'] = (tail[-500:] if tail and rc != 0
                              else 'Update failed' if rc != 0
                              else 'Update ended without confirming completion')

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

        # `version` is NOT an inert label. update.sh interpolates it into
        # https://api.github.com/repos/<repo>/releases/tags/v${version} to
        # find the pre-compiled binary, and curl normalizes dot-segments —
        # so "1.0.8/../../../../attacker/evil/releases/tags/v1" fetched ANOTHER
        # repo's release metadata and installed ITS binary, while the
        # download_url below stayed a perfectly valid asset of our own repo.
        # It is also written verbatim into VERSION. X.Y.Z or X.Y.Z-beta.N only
        # (what `update.sh check` emits as latest_version). [0-9], not \d:
        # Python's \d matches every Unicode digit; fullmatch, not `$`, which
        # would accept a trailing newline. update.sh re-checks this
        # independently.
        if not isinstance(version, str) or not _OTA_VERSION_RE.fullmatch(version):
            return error_response(
                "VALIDATION_ERROR", "Invalid version (expected X.Y.Z or X.Y.Z-beta.N)")
        if not isinstance(download_url, str):
            return error_response("VALIDATION_ERROR", "Invalid download URL")

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
        # would pass a prefix check yet fetch attacker/repo. And "anything
        # under the repo" was still too loose: it accepted a branch archive
        # or a DIFFERENT tag than `version`, so VERSION could claim one
        # release while another was installed. The URL must now be exactly
        # one of the shapes a tagged release of THIS version produces
        # (`update.sh check` emits the release-asset form, or the API
        # tarball_url as its fallback), with no dot-segments, query or
        # fragment, and an asset name drawn from a plain-filename alphabet so
        # percent-encoded separators (%2F, %2e) cannot ride along.
        gh_repo = os.getenv("MAGIC_GITHUB_REPO", "a-train-chain/magic_dingus_box")
        if not _ota_download_url_ok(download_url, gh_repo, version):
            return error_response(
                "VALIDATION_ERROR",
                f"Invalid download URL (must be the v{version} release of "
                f"the {gh_repo} GitHub repo)")

        # Launch detached (prune stale terminal-state entries first). The
        # job's id is the detached job's id, so a restarted Flask can find it.
        _prune_terminal_jobs(update_jobs)
        job_id, err = _launch_maintenance_job(
            "ota-install", [str(UPDATE_SCRIPT), "install", version, download_url])
        if err:
            return err
        update_jobs[job_id] = _new_update_job_view(version)
        _invalidate_update_check_cache()

        return success_response(data={'job_id': job_id}, message="Update started")

    @app.get("/admin/update/status/<job_id>")
    def update_status(job_id):  # type: ignore[no-redef]
        """Get status of an update job.

        Answered from the job's on-disk log, so it keeps working after the
        web service restarts mid-update (the job itself is unaffected — it
        runs in its own systemd unit). Still 404 for an id we have no record
        of; the Settings tab treats that as "restarted, verify the version".
        """
        job = update_jobs.get(job_id)
        if job is None:
            meta = detached.meta(job_id)
            if not meta or meta.get('kind') != 'ota-install':
                return error_response("NOT_FOUND", "Job not found", status=404)
            job = update_jobs.setdefault(job_id, _new_update_job_view(None))
        with _update_jobs_lock:  # concurrent pollers must not split the cursor
            _refresh_update_job(job_id, job)
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

        # The rollback runs DETACHED (own systemd unit — see detached_jobs.py)
        # and this request waits on its log. The response contract is
        # unchanged (synchronous JSON); what changed is that the rollback no
        # longer dies with us: update.sh restarts magic-dingus-web as its
        # last act, and anything else restarting the unit mid-rsync used to
        # kill a half-restored install along with this request.
        job_id, err = _launch_maintenance_job("ota-rollback", [str(UPDATE_SCRIPT), "rollback"])
        if err:
            return err
        _invalidate_update_check_cache()

        deadline = time.monotonic() + 180  # 3 minute timeout for rollback
        cursor, lines, state, rc = 0, [], "running", None
        try:
            while True:
                result = detached.read(job_id, cursor)
                if result is None:
                    return error_response("INTERNAL_ERROR", "Rollback job vanished", status=500)
                new, cursor, state, rc = result
                lines += new
                if state != "running":
                    break
                if time.monotonic() > deadline:
                    # The job carries on in its own unit; only our wait ends.
                    return error_response("TIMEOUT", "Rollback timed out", status=504)
                time.sleep(0.25)
        except Exception as e:
            return error_response("INTERNAL_ERROR", str(e), status=500)
        finally:
            _invalidate_update_check_cache()

        json_lines = []
        for l in lines:
            l = l.strip()
            if l.startswith("{"):
                try:
                    parsed = json.loads(l)
                except json.JSONDecodeError:
                    continue
                if isinstance(parsed, dict):
                    json_lines.append(parsed)

        if state == "exited" and rc == 0:
            # The last JSON line of output is the completion message.
            if json_lines:
                return jsonify(json_lines[-1]), 200
            return success_response(message="Rollback completed")

        err_obj = json_lines[-1].get("error") if json_lines else None
        error_msg = (err_obj.get("message") if isinstance(err_obj, dict) else None) \
            or strip_ansi("\n".join(l for l in lines if not l.startswith("{"))[-500:]) \
            or ("Rollback process stopped unexpectedly" if state == "lost" else "Rollback failed")
        return error_response("ROLLBACK_FAILED", error_msg, status=500)

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
        "mdb_sonarr",
        "mdb_prowlarr",
        "mdb_qbittorrent",
        "mdb_byparr",
    ]

    # Track media-browser setup jobs (in-memory, cleared on restart)
    media_browser_jobs: dict = {}
    _MB_LOG_BUFFER_LIMIT = 500  # keep at most this many lines per job
    _MB_LOG_TAIL_LINES = 30     # return this many lines on each status poll

    def _write_env_file(path: Path, env: dict) -> None:
        """Write a dict back to a .env file with chmod 600. Creates parent dir."""
        path.parent.mkdir(parents=True, exist_ok=True)
        # Write to a tempfile in the same dir, then atomic rename, so a crash
        # mid-write can't leave a partial .env.
        lines = [_format_env_line(k, v) for k, v in env.items()]
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
        """Return [{name, status, paused_for_playback}] for the expected
        media-browser containers.

        paused_for_playback marks the state where playback_services_pause.sh
        stopped the container to free RAM for a game/movie. Radarr and
        Prowlarr never survive the script's 2 s SIGTERM grace, so their raw
        docker status during every playback session is "Exited (137)" —
        indistinguishable from a crash/OOM without this flag. Requires all
        three signals: the script's marker file, membership in the pause
        set, and an Exited status (an Up container beats a stale marker,
        and "not found"/"unknown" is not the pause signature).
        """
        try:
            result = subprocess.run(
                ["docker", "ps", "-a", "--format", "{{.Names}}\t{{.Status}}"],
                capture_output=True, text=True, timeout=5,
            )
            if result.returncode != 0:
                return [{"name": n, "status": "unknown",
                         "paused_for_playback": False}
                        for n in EXPECTED_CONTAINERS]
            running = {}
            for line in result.stdout.splitlines():
                parts = line.split("\t", 1)
                if len(parts) == 2:
                    running[parts[0].strip()] = parts[1].strip()
            playback_paused = PLAYBACK_PAUSE_MARKER.exists()
            return [
                {
                    "name": n,
                    "status": running.get(n, "not found"),
                    "paused_for_playback": (
                        playback_paused
                        and n in PLAYBACK_PAUSED_CONTAINERS
                        and running.get(n, "").lower().startswith("exited")
                    ),
                }
                for n in EXPECTED_CONTAINERS
            ]
        except Exception:
            return [{"name": n, "status": "unknown",
                     "paused_for_playback": False}
                    for n in EXPECTED_CONTAINERS]

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
        try:
            env = _read_env_file(path)
        except EnvFileReadError:
            return False  # fail closed — an unreadable .env allows nothing
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

    # VPN server country (Media Browser > Advanced). All logic lives in
    # vpn_settings.py; this hands it the gates and helpers it must share.
    vpn_settings.register(
        app,
        require_csrf=require_csrf,
        check_gates=_check_media_browser_gates,
        env_path=SERVICES_ENV,
        script_path=data_dir.parent / "scripts" / "recreate_gluetun.sh",
        read_env=_read_env_file,
        env_read_error=EnvFileReadError,
        format_env_line=_format_env_line,
        require_sudo=lambda: _require_nopasswd_sudo(),
        maintenance_precheck=_maintenance_precheck,
        launch_job=_launch_maintenance_job,
        detached=detached,
        success_response=success_response,
        error_response=error_response,
    )

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

        # Refuse to mint a SECOND MOVIES drive. mkfs stamps LABEL=MOVIES and
        # fstab mounts by label, so formatting a new disk while another
        # MOVIES-labeled disk is attached makes which one mounts at boot
        # arbitrary — and the old code here made it worse by lazy-unmounting
        # /mnt/ssd unconditionally first, detaching the LIVE library out from
        # under the running containers even when the target was an unrelated
        # disk (writes continued into the detached filesystem; imports failed
        # with nothing in any log). Formatting the current MOVIES drive
        # itself remains allowed — that's the wipe-and-rebuild flow.
        movies_disks = movies_drive_devices(json.loads(listing))
        movies_elsewhere = [n for n in movies_disks if n != device]
        if movies_elsewhere:
            return error_response(
                "VALIDATION_ERROR",
                f"{movies_elsewhere[0]} is currently the MOVIES drive. "
                f"Formatting {device} too would leave two MOVIES-labeled "
                "disks and which one holds the library after a reboot would "
                "be arbitrary. Unplug the old drive first (or format that "
                "drive instead).", status=409)

        path = target["path"]
        try:
            # Unmount everything on the target before touching it, or wipefs
            # fails with "device is busy" — a confusing way for this to end.
            #
            # /mnt/ssd is released ONLY when the target itself is the MOVIES
            # drive (the guard above means any other MOVIES disk already
            # blocked the request, so this is scoped by construction).
            #
            # Mountpoints are queried live rather than read from the listing
            # above: the eligible-device records deliberately carry no
            # partition detail, and state can change between listing and
            # formatting anyway.
            if device in movies_disks:
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

    _mb_jobs_lock = threading.Lock()

    def _new_mb_job_view(started_ts: float) -> dict:
        return {
            "status": "running",
            "exit_code": None,
            "log": [],
            "started_at": datetime.fromtimestamp(started_ts).isoformat(),
            "started_ts": started_ts,
            "_cursor": 0,
            "_pruner_ts": time.time(),
        }

    def _refresh_media_browser_job(job_id: str, job: dict) -> None:
        """Pull setup_services.sh's new output from the detached job's log.

        setup_services.sh runs as its own systemd unit (detached_jobs.py):
        it restarts magic-dingus-web itself (Step: uinput group), which under
        KillMode=control-group used to kill the script that issued the
        restart, mid-provisioning. Status is re-derived from the log, so a
        restarted Flask resumes reporting where the old one stopped.
        """
        if job["status"] != "running":
            return
        result = detached.read(job_id, job["_cursor"])
        if result is None:
            return
        lines, job["_cursor"], state, rc = result
        buf = job["log"]
        buf.extend(lines)
        if len(buf) > _MB_LOG_BUFFER_LIMIT:
            del buf[: len(buf) - _MB_LOG_BUFFER_LIMIT]
        if state == "exited":
            job["exit_code"] = rc
            job["status"] = "success" if rc == 0 else "failed"
        elif state == "lost":
            buf.append("[admin.py] setup process stopped unexpectedly — "
                       "safe to run setup again")
            job["exit_code"] = -1
            job["status"] = "failed"
        if job["status"] != "running":
            job["_pruner_ts"] = time.time()

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

        # Fail fast if the magic user lacks NOPASSWD sudo (rather than hang on
        # a password prompt) — this route shells out via `sudo -n` below.
        if (resp := _require_nopasswd_sudo()):
            return resp

        # Refuse BEFORE touching .env: a setup (or OTA) already running is
        # reading it, and rewriting it underneath is the half of the race
        # the launch-time check alone cannot prevent.
        if (resp := _maintenance_precheck()):
            return resp

        # Merge WG vars + sensible defaults into existing .env. An existing
        # .env we cannot READ must abort the request: treating it as empty
        # would rewrite it with only the WireGuard keys and destroy the qBit
        # password + API keys (see _read_env_file).
        try:
            env = _read_env_file(SERVICES_ENV)
        except EnvFileReadError as e:
            return error_response(
                "env_read_failed",
                f"{e}. Nothing was changed. The file's owner or permissions "
                "need fixing before the VPN can be reconfigured.",
                status=500,
            )
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
        #
        # Country: the form's value, else the one this box already uses (the
        # Content Manager's VPN-country setting writes it — a Reconfigure
        # with a fresh .conf must not silently move the box back to the
        # Netherlands), else the historical default.
        country = ((request.form.get("country") or "").strip()
                   or (env.get("VPN_COUNTRIES") or "").strip()
                   or vpn_settings.DEFAULT_COUNTRY)
        env.update(_vpn_provider_env(provider, wg, country=country))

        try:
            _write_env_file(SERVICES_ENV, env)
        except Exception as e:
            return error_response(
                "env_write_failed",
                f"Could not write {SERVICES_ENV}: {e}",
                status=500,
            )

        # Start the long-running setup script DETACHED, as root (it was
        # `sudo -n setup_services.sh` before — same privilege, now in its own
        # cgroup). Prune stale terminal-state entries first.
        _prune_terminal_jobs(media_browser_jobs)
        job_id, err = _launch_maintenance_job(
            "mb-setup", [str(SETUP_SERVICES_SCRIPT)], as_root=True)
        if err:
            return err
        media_browser_jobs[job_id] = _new_mb_job_view(time.time())

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
            # Not in memory — e.g. this Flask restarted mid-setup (which
            # setup_services.sh itself causes). The job's own record on disk
            # still knows; rebuild from it.
            meta = detached.meta(job_id)
            if meta and meta.get("kind") == "mb-setup":
                started = meta.get("started_ts")
                job = media_browser_jobs.setdefault(
                    job_id, _new_mb_job_view(
                        started if isinstance(started, (int, float)) else time.time()))
        if not job:
            return success_response(data={
                "status": "unknown",
                "log_lines": [],
                "exit_code": None,
                "started_at": None,
                "elapsed_sec": 0,
            })

        with _mb_jobs_lock:  # concurrent pollers must not split the cursor
            _refresh_media_browser_job(job_id, job)

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

        try:
            env = _read_env_file(SERVICES_ENV)
        except EnvFileReadError as e:
            return error_response("env_read_failed", str(e), status=500)
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
        body = _qbit_get(env, "torrents/info")
        if body is None:
            return {"active": -1, "seeding": -1}
        try:
            torrents = json.loads(body)
            if not isinstance(torrents, list):
                return {"active": -1, "seeding": -1}
            seeding_states = {"uploading", "stalledUP", "queuedUP", "forcedUP", "checkingUP"}
            seeding = sum(1 for t in torrents if t.get("state") in seeding_states)
            return {"active": len(torrents), "seeding": seeding}
        except Exception:
            return {"active": -1, "seeding": -1}

    def _qbit_listen_port(env: dict) -> int:
        """Return qBit's currently-configured listen_port, or -1 on failure."""
        body = _qbit_get(env, "app/preferences")
        if body is None:
            return -1
        try:
            payload = json.loads(body)
            return int(payload.get("listen_port", -1))
        except Exception:
            return -1

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

        try:
            env = _read_env_file(SERVICES_ENV)
        except EnvFileReadError as e:
            return error_response("env_read_failed", str(e), status=500)
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

        if (resp := _require_nopasswd_sudo()):
            return resp

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

        if (resp := _require_nopasswd_sudo()):
            return resp

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

    @app.get("/connect")
    def connect_landing():  # type: ignore[no-redef]
        """Unified "Connect a Device" landing page — the kiosk QR target.

        The kiosk's Settings menu used to point two different QR codes at
        two different URLs (the Content Manager root, and the phone-remote
        pairing flow) and customers could not tell which one they wanted.
        Both kiosk QR codes now land HERE, and the choice is made in words.

        Accepts an optional ?code=NNNNNN from the pairing screen's QR. With
        a well-formed code the remote button submits it through the EXISTING
        pairing flow (GET /?pair=CODE&tab=remote — handle_pair_param lives
        on the root route; there is no /pair route). Without one, the button
        goes to /admin/remote, which already shows the in-app 6-digit form
        when unpaired.

        This page deliberately adds NO new pairing mechanics: iOS
        home-screen apps have a separate cookie jar from Safari and depend
        on the in-app 6-digit form, so the /admin/remote form, the
        handle_pair_param flow, and the HMAC cookie are all untouched — this
        is only a signpost in front of them. A malformed code is treated as
        absent rather than rejected: the customer still gets a working page
        and the in-app form as the fallback path.
        """
        code = (request.args.get("code") or "").strip()
        if not re.fullmatch(r"[0-9]{6}", code):
            code = ""
        remote_href = f"/?pair={code}&tab=remote" if code else "/admin/remote"
        return render_template_string(
            CONNECT_PAGE_HTML, remote_href=remote_href, has_code=bool(code))

    @app.get("/")
    @app.get("/admin")
    def admin_interface():  # type: ignore[no-redef]
        """Serve the web interface.

        If ?pair=<code> is present, delegate to the phone-remote pairing flow
        before serving the static SPA so that the kiosk QR-code link is handled
        transparently.

        If ?device_token= is present (an installed Content Manager app's
        start_url, planted there by the dynamic root manifest), redeem it
        into the mdb_remote cookie so the SPA's Remote tab — a same-origin
        iframe of /admin/remote, or a same-origin navigation on phones —
        opens already paired. Redeeming on EVERY launch (even with a live
        cookie) is deliberate: it rolls the cookie's issue time forward and
        heals a jar that iOS evicted. The 303 re-serves the same path with
        device_token stripped but every other query param preserved. An
        invalid or revoked token just proceeds to the SPA unauthenticated —
        no error, no signal about token validity.
        """
        pair_code = request.args.get("pair")
        if pair_code:
            if request.headers.get("Sec-Fetch-Site", "").lower() == "cross-site":
                # A pairing code arriving by a link on ANOTHER website is
                # never the kiosk's QR (a camera scan is Sec-Fetch-Site:
                # none) — it is how a hostile page would spend the attempt
                # budget. Show the Connect page instead: its button submits
                # the same code same-origin, so a genuine link costs one tap.
                code = pair_code.strip()
                target = (f"/connect?code={code}"
                          if re.fullmatch(r"[0-9]{6}", code) else "/connect")
                return redirect(target, code=303)
            return remote_auth.handle_pair_param(pair_code)
        submitted_token = request.args.get("device_token")
        if submitted_token:
            redeemed = remote_auth.redeem_device_token(submitted_token)
            if redeemed is not None:
                remaining = [(k, v)
                             for k, vals in request.args.lists()
                             for v in vals if k != "device_token"]
                qs = urlencode(remaining)
                resp = redirect(request.path + (f"?{qs}" if qs else ""),
                                code=303)
                remote_auth.issue_cookie(resp, redeemed)
                return resp
        static_dir = Path(__file__).parent / "static"
        return send_file(static_dir / "index.html")

    # ------------------------------------------------------------------
    # Dynamic web-app manifests (iOS home-screen install pairing).
    #
    # iOS gives an installed home-screen app a SEPARATE cookie jar from
    # Safari, so "Add to Home Screen" on a paired page used to produce an
    # app that opened UNPAIRED (the pairing cookie stayed in Safari's
    # jar). The bridge: iOS fetches the manifest at install time FROM THE
    # PAIRED SAFARI SESSION, so a request presenting a valid mdb_remote
    # cookie gets a start_url carrying that device's durable install
    # token — the token rides inside the icon, and the installed app
    # trades it for its own cookie on first launch (the redeem branches
    # in admin_interface and remote_page). An unauthenticated fetch gets
    # the same manifest with a bare start_url, which degrades to the
    # 6-digit pair form.
    #
    # TWO manifests because there are two install surfaces: the pairing
    # flow lands people on the root Content Manager SPA — where field
    # testing showed installs actually happen — and /admin/remote is the
    # standalone remote. Each embeds the SAME per-device token; only the
    # start_url/scope/identity differ.
    #
    # no-store is mandatory on both: a cached token-bearing manifest
    # served to the wrong requester would be a credential leak.
    # ------------------------------------------------------------------

    _MANIFEST_ICONS = [
        {"src": "/static/icons/icon-192.png", "sizes": "192x192",
         "type": "image/png", "purpose": "any"},
        {"src": "/static/icons/icon-512.png", "sizes": "512x512",
         "type": "image/png", "purpose": "any"},
        {"src": "/static/icons/icon-maskable-192.png", "sizes": "192x192",
         "type": "image/png", "purpose": "maskable"},
        {"src": "/static/icons/icon-maskable-512.png", "sizes": "512x512",
         "type": "image/png", "purpose": "maskable"},
    ]

    def _tokened_start_url(base: str) -> str:
        """base, plus this requester's install token when (and only when)
        the request presents a valid pairing cookie."""
        cookie = request.cookies.get(remote_auth.COOKIE_NAME, "")
        device_id = remote_auth.verify_cookie(cookie)
        if device_id is not None:
            token = remote_auth.device_token_for(device_id)
            if token:
                return f"{base}?device_token={token}"
        return base

    def _manifest_response(payload: dict):
        resp = app.response_class(
            json.dumps(payload, indent=2),
            mimetype="application/manifest+json")
        resp.headers["Cache-Control"] = "no-store"
        return resp

    # The /static/manifest.webmanifest alias is deliberate, not legacy
    # convenience: the exact rule outranks the /static/<path:filename>
    # converter rule in werkzeug's ordering, so it SHADOWS the old static
    # file's URL. Any cached page still referencing the old address gets
    # this dynamic manifest — a stale tokenless manifest cannot be served
    # by accident. The static file itself is deleted from the repo.
    @app.get("/manifest.webmanifest")
    @app.get("/static/manifest.webmanifest")
    def root_manifest():  # type: ignore[no-redef]
        """Dynamic manifest for the root Content Manager app (see the
        install-pairing comment block above). Identity preserved from the
        old static manifest — only start_url became dynamic. No forced
        tab= in start_url: someone installing the CM for management should
        land on the default tab; the token pairs the Remote tab silently."""
        return _manifest_response({
            "name": "Magic Dingus Box",
            "short_name": "Magic Dingus Box",
            "description": "Content Manager and phone remote for your "
                           "Magic Dingus Box.",
            "id": "/",
            "start_url": _tokened_start_url("/"),
            "scope": "/",
            "display": "standalone",
            "orientation": "any",
            "background_color": "#1F191F",
            "theme_color": "#131013",
            "icons": _MANIFEST_ICONS,
        })

    @app.get("/admin/remote/manifest.webmanifest")
    def remote_manifest():  # type: ignore[no-redef]
        """Dynamic manifest for the standalone phone remote (see the
        install-pairing comment block above)."""
        return _manifest_response({
            "name": "Dingus Remote",
            "short_name": "Dingus Remote",
            "description": "Phone remote for your Magic Dingus Box.",
            "id": "/admin/remote",
            "start_url": _tokened_start_url("/admin/remote"),
            "scope": "/admin/remote",
            "display": "standalone",
            "background_color": "#1F191F",
            "theme_color": "#131013",
            "icons": _MANIFEST_ICONS,
        })

    @app.route("/admin/remote", methods=["GET"])
    def remote_page():  # type: ignore[no-redef]
        cookie = request.cookies.get(remote_auth.COOKIE_NAME, "")
        device_id = remote_auth.verify_cookie(cookie)
        if device_id is None:
            # Installed-app first launch: no cookie in THIS jar yet, but the
            # icon's start_url may carry the durable install token the
            # manifest embedded at install time. Trade it for a cookie and
            # 303 to the clean URL (keeps the token out of the visible
            # URL/history). This GET is the ONLY place the token redeems.
            # An invalid/revoked token deliberately falls through to the
            # ordinary pair form — no signal about token validity.
            submitted_token = request.args.get("device_token", "")
            if submitted_token:
                redeemed = remote_auth.redeem_device_token(submitted_token)
                if redeemed is not None:
                    resp = redirect("/admin/remote", code=303)
                    remote_auth.issue_cookie(resp, redeemed)
                    return resp
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
<!-- Same installable-app tags as the paired remote shell — this page is
     served at the SAME URL (/admin/remote), so it must reference the same
     remote-scoped manifest or install behavior would depend on pairing
     state. Unauthenticated manifest fetches carry no token, so an install
     made from here opens on this pair form — the correct degradation.
     Without these tags, adding to the home screen from THIS page (a real
     possibility, since it is where an expired pairing lands you) produces
     a generic Safari bookmark with a screenshot icon instead of the app. -->
<link rel="manifest" href="/admin/remote/manifest.webmanifest" crossorigin="use-credentials">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="Dingus Remote">
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
  <p>On the kiosk, open <strong>Settings &rarr; Connect a Device</strong> and enter the 6-digit code shown there.</p>
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
            # Through devices.py, under its lock: an atomic rename alone kept
            # the file whole but not CURRENT — this read-modify-write could
            # write back a copy read before a concurrent pairing (or the
            # StatusBroadcaster's revocation reap) committed, undoing it.
            remote_devices.rename_device(paired_path, device_id, nickname)
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

