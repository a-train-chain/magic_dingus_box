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
from types import SimpleNamespace
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


# The admin_*.py modules split out of this file (2026-10). Re-exported:
# every public name they took over, the helpers create_app() below still
# uses, and the private names the test suite reaches as admin.<name>.
# NOT re-exported, on purpose: module STATE that tests rebind
# (admin_security._csrf_tokens, admin_media_browser.PLAYBACK_PAUSE_MARKER) —
# rebinding a copy here would silently miss the real one; patch the owner.
try:  # noqa: E402
    from admin_common import (
        _atomic_write_text, _read_zip_entry_capped, _staged_save_upload,
        _ZIP_TEXT_ENTRY_MAX, _ZipEntryTooLarge, BYTES_PER_GB,
        check_service_status, error_response, get_cpu_temperature,
        get_cpu_usage, get_disk_info, get_free_bytes, get_memory_info,
        get_uptime, strip_ansi, success_response,
    )
    import admin_security
    from admin_security import (
        _CSRF_TOKEN_MAX, _generate_csrf_token, _host_is_allowed, _is_within,
        _validate_csrf_token,
    )
    import admin_playlists
    from admin_playlists import (
        _invalid_emulator_core, format_playlist_yaml,
        UNTITLED_PLAYLIST_FILENAME,
    )
    import admin_system
    from admin_system import DEVICE_NAME_MAX, get_local_ip
    import admin_playlist_import
    import admin_media
    from admin_media import (
        _detect_pi_model, _max_transcodes_for, _policy_pi_model,
        PLATFORM_POLICY_OVERRIDE_ENV,
    )
    import admin_roms
    from admin_roms import MULTI_DISC_SYSTEMS
    from admin_vpn_config import (
        _detect_vpn_brand, _format_env_line, _parse_wireguard_config,
        _parse_wireguard_endpoint, _read_env_file, _unquote_env_value,
        _vpn_provider_env, _vpn_supports_port_forwarding, EnvFileReadError,
    )
    from admin_tmdb import (
        _kiosk_started_at, _tmdb_classify_key, _tmdb_key_file,
        _TMDB_KEY_FILE_ENV, _tmdb_redact, _tmdb_verify_key, KIOSK_SERVICE,
    )
    import admin_support
    import admin_updates
    from admin_updates import _OTA_VERSION_RE
    import admin_media_browser
    from admin_media_browser import (
        _require_nopasswd_sudo, PLAYBACK_PAUSED_CONTAINERS,
    )
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from .admin_common import (
        _atomic_write_text, _read_zip_entry_capped, _staged_save_upload,
        _ZIP_TEXT_ENTRY_MAX, _ZipEntryTooLarge, BYTES_PER_GB,
        check_service_status, error_response, get_cpu_temperature,
        get_cpu_usage, get_disk_info, get_free_bytes, get_memory_info,
        get_uptime, strip_ansi, success_response,
    )
    from . import admin_security
    from .admin_security import (
        _CSRF_TOKEN_MAX, _generate_csrf_token, _host_is_allowed, _is_within,
        _validate_csrf_token,
    )
    from . import admin_playlists
    from .admin_playlists import (
        _invalid_emulator_core, format_playlist_yaml,
        UNTITLED_PLAYLIST_FILENAME,
    )
    from . import admin_system
    from .admin_system import DEVICE_NAME_MAX, get_local_ip
    from . import admin_playlist_import
    from . import admin_media
    from .admin_media import (
        _detect_pi_model, _max_transcodes_for, _policy_pi_model,
        PLATFORM_POLICY_OVERRIDE_ENV,
    )
    from . import admin_roms
    from .admin_roms import MULTI_DISC_SYSTEMS
    from .admin_vpn_config import (
        _detect_vpn_brand, _format_env_line, _parse_wireguard_config,
        _parse_wireguard_endpoint, _read_env_file, _unquote_env_value,
        _vpn_provider_env, _vpn_supports_port_forwarding, EnvFileReadError,
    )
    from .admin_tmdb import (
        _kiosk_started_at, _tmdb_classify_key, _tmdb_key_file,
        _TMDB_KEY_FILE_ENV, _tmdb_redact, _tmdb_verify_key, KIOSK_SERVICE,
    )
    from . import admin_support
    from . import admin_updates
    from .admin_updates import _OTA_VERSION_RE
    from . import admin_media_browser
    from .admin_media_browser import (
        _require_nopasswd_sudo, PLAYBACK_PAUSED_CONTAINERS,
    )


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


# ===== ROUTE MODULE WIRING =====
#
# create_app() owns the process-wide startup work (secret key, status
# broadcaster, uinput gamepad, upload_temp sweep, storage/reload helpers,
# the maintenance-job launcher) and hands each area's routes to its module's
# register(app, ctx), in the order they were once defined inline, so the URL
# map, hook order and every per-process singleton are unchanged. `ctx` is a
# plain namespace: each module reads the create_app() state it needs from it
# and publishes the helpers later modules share (require_csrf,
# get_device_info, _prune_terminal_jobs, ...).
#
# Test seams. The suite monkeypatches these names ON THIS MODULE
# (monkeypatch.setattr(admin, "get_free_bytes", ...)); route modules get
# late-binding shims that look the name up here at call time, so such a patch
# still reaches every caller. Add a name here if a test needs to patch it
# for routes that live in an admin_*.py module.
_ROUTE_SEAMS = (
    "get_free_bytes",
    "_atomic_write_text",
    "check_service_status",
    "_require_nopasswd_sudo",
    "_detect_pi_model",
    "_kiosk_started_at",
    "_tmdb_verify_key",
    "_media_browser_unlocked",
)


def _late_bound(name: str):
    """A callable that resolves admin.<name> at call time (see _ROUTE_SEAMS)."""
    def shim(*args, **kwargs):
        return globals()[name](*args, **kwargs)
    shim.__name__ = shim.__qualname__ = name
    return shim


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

    
    # Shared state for the route modules' register() calls (admin_*.py).
    ctx = SimpleNamespace(
        data_dir=data_dir,
        _media_browser_locked_response=_media_browser_locked_response,
        **{name: _late_bound(name) for name in _ROUTE_SEAMS},
    )
    admin_security.register(app, ctx)
    require_csrf = ctx.require_csrf

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

    ctx._poke_kiosk_reload = _poke_kiosk_reload
    ctx.device_info_file = device_info_file
    ctx.media_dir = media_dir
    ctx.playlists_dir = playlists_dir
    ctx.roms_dir = roms_dir
    admin_system.register(app, ctx)

    admin_playlists.register(app, ctx)

    ctx.IMPORT_PEAK_MULTIPLIER = IMPORT_PEAK_MULTIPLIER
    ctx.STORAGE_HEADROOM_BYTES = STORAGE_HEADROOM_BYTES
    ctx._storage_precondition = _storage_precondition
    admin_playlist_import.register(app, ctx)

    ctx.upload_temp_dir = upload_temp_dir
    admin_media.register(app, ctx)

    admin_roms.register(app, ctx)

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

    admin_support.register(app, ctx)

    ctx._launch_maintenance_job = _launch_maintenance_job
    ctx.detached = detached
    admin_updates.register(app, ctx)

    ctx._maintenance_precheck = _maintenance_precheck
    admin_media_browser.register(app, ctx)
    SERVICES_DIR = ctx.SERVICES_DIR
    SERVICES_ENV = ctx.SERVICES_ENV
    _check_media_browser_gates = ctx._check_media_browser_gates

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

