"""TMDB API-key helpers (classify, verify, redact, key-file path) and the
kiosk unit's start time used to tell whether the running kiosk has the
current key. Pure functions, no routes.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
from datetime import datetime
from pathlib import Path
from typing import Optional


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
