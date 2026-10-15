"""The Content Manager (web admin) Flask app: create_app() and its wiring.

The routes live in the admin_*.py modules next to this file, one per area
(see "ROUTE MODULE WIRING" below for how create_app() hands them their
shared state); this module keeps the process-wide startup work that must run
exactly once per serving process — secret key, status broadcaster, uinput
virtual gamepad, upload_temp sweep — plus the storage/reload helpers and the
maintenance-job launcher the route modules share.
"""
from __future__ import annotations

# socket, subprocess, tempfile and zipfile are not used below any more, but
# the test suite patches stdlib functions THROUGH this module
# (admin.socket.gethostname, admin.subprocess.run, admin.tempfile.mkstemp,
# admin.zipfile.ZipFile, admin.os.open, admin.threading.Thread) — keep them.
import json
import os
import secrets
import shutil
import socket  # noqa: F401 - reached by tests as admin.socket
import subprocess  # noqa: F401 - reached by tests as admin.subprocess
import sys
import tempfile  # noqa: F401 - reached by tests as admin.tempfile
import threading
import time
import zipfile  # noqa: F401 - reached by tests as admin.zipfile
from pathlib import Path
from types import SimpleNamespace

from flask import Flask

# admin.py is imported both as a package member and as a flat module (the test
# harness does the latter), so sibling imports need the same dual form the
# remote/* imports below use.
try:  # noqa: E402
    from detached_jobs import DetachedJobs, default_state_dir as _default_job_state_dir
    import vpn_settings
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from .detached_jobs import DetachedJobs, default_state_dir as _default_job_state_dir
    from . import vpn_settings

try:
    from remote.uinput_writer import UinputWriter
    from remote.text_input_writer import TextInputWriter
except ImportError:
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
        _kiosk_started_at, _tmdb_classify_key, _TMDB_KEY_FILE_ENV,
        _tmdb_redact, _tmdb_verify_key, KIOSK_SERVICE,
    )
    import admin_support
    import admin_updates
    from admin_updates import _OTA_VERSION_RE
    import admin_media_browser
    from admin_media_browser import (
        _require_nopasswd_sudo, PLAYBACK_PAUSED_CONTAINERS,
    )
    import admin_media_browser_ops
    import admin_remote
    from admin_remote import CONNECT_PAGE_HTML, NICKNAME_PROMPT_HTML
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
        _kiosk_started_at, _tmdb_classify_key, _TMDB_KEY_FILE_ENV,
        _tmdb_redact, _tmdb_verify_key, KIOSK_SERVICE,
    )
    from . import admin_support
    from . import admin_updates
    from .admin_updates import _OTA_VERSION_RE
    from . import admin_media_browser
    from .admin_media_browser import (
        _require_nopasswd_sudo, PLAYBACK_PAUSED_CONTAINERS,
    )
    from . import admin_media_browser_ops
    from . import admin_remote
    from .admin_remote import CONNECT_PAGE_HTML, NICKNAME_PROMPT_HTML


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
# still reaches every caller. To make another name patchable as
# admin.<name> for routes in an admin_*.py module: add it here AND bind it
# from ctx at the top of that module's register() (`name = ctx.name`).
# Module STATE (a dict, a Path constant) cannot be shimmed — tests patch
# those on the module that owns them.
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

    admin_media_browser_ops.register(app, ctx)

    ctx.sock = sock
    admin_remote.register(app, ctx)

    return app

