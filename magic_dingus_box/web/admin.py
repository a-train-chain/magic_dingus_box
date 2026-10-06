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
        _parse_wireguard_endpoint, _read_env_file,
        _resolve_wireguard_endpoint_ip, _unquote_env_value,
        _VPN_AUTO_NATIVE_PROVIDERS, _vpn_provider_choices,
        _VPN_PROVIDER_CUSTOM, _vpn_provider_env,
        _vpn_supports_port_forwarding, EnvFileReadError,
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
        _parse_wireguard_endpoint, _read_env_file,
        _resolve_wireguard_endpoint_ip, _unquote_env_value,
        _VPN_AUTO_NATIVE_PROVIDERS, _vpn_provider_choices,
        _VPN_PROVIDER_CUSTOM, _vpn_provider_env,
        _vpn_supports_port_forwarding, EnvFileReadError,
    )


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
    get_app_version = ctx.get_app_version
    get_device_info = ctx.get_device_info

    admin_playlists.register(app, ctx)

    ctx.IMPORT_PEAK_MULTIPLIER = IMPORT_PEAK_MULTIPLIER
    ctx.STORAGE_HEADROOM_BYTES = STORAGE_HEADROOM_BYTES
    ctx._storage_precondition = _storage_precondition
    admin_playlist_import.register(app, ctx)

    ctx.upload_temp_dir = upload_temp_dir
    admin_media.register(app, ctx)
    _prune_terminal_jobs = ctx._prune_terminal_jobs

    admin_roms.register(app, ctx)

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

