#!/usr/bin/env python3
"""Periodic re-search for monitored movies that have no file yet.

Runs on a systemd timer (every 4 hours + shortly after boot). Asks
Radarr to search all monitored-but-missing movies, so a title that
found nothing grabbable at add-time gets retried automatically instead
of sitting idle until the user manually opens the release picker.

Why this is needed
------------------
The kiosk's "Add to Library" sets addOptions.searchForMovie=true, so
Radarr fires ONE automatic search the instant a movie is added. If that
single search comes up empty — because the good release is not posted
yet, the indexer is in a transient cooldown, or Byparr is mid-Cloudflare-
challenge — Radarr does NOT keep retrying on its own at a useful cadence
(RSS sync only catches brand-new releases going forward). The movie then
sits there with no download until the user notices and manually picks a
release. Observed live with "Wolfs (2024)": auto-search at add found
nothing, the +50-scoring x264 YTS release the user later grabbed by hand
was simply not available at that exact moment.

This job closes that gap: every few hours it re-runs the search for the
backlog of missing monitored movies. The selection logic itself is
already correct (x264 +50 preferred, HEVC/AV1/foreign/remux rejected by
the custom-format scores) — this just guarantees that backlog keeps
getting retried until a release that passes the quality gate appears.

Idempotent + cheap: a run with zero missing movies is a no-op. Radarr
internally queues + rate-limits the per-movie searches.
"""
import json
import os
import sys
import time
import urllib.request
import urllib.error

RADARR_API_KEY = ""
RADARR_BASE = "http://localhost:7878"


def wait_for_radarr(timeout_s=120, poll_s=5):
    """Poll Radarr's /ping until it answers, or give up after timeout_s.

    The timer's Persistent=true boot catch-up fires within a minute of
    boot, while the docker stack is still starting — without this wait
    the run dies on connection-reset and the missing backlog is not
    retried for another 4 hours.
    """
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            req = urllib.request.Request(RADARR_BASE + "/ping")
            with urllib.request.urlopen(req, timeout=5) as r:
                if r.status == 200:
                    return True
        except OSError:
            pass
        time.sleep(poll_s)
    return False


def load_env(path="/opt/magic_dingus_box/services/.env"):
    """Parse RADARR_API_KEY from the services .env file."""
    global RADARR_API_KEY
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("RADARR_API_KEY="):
                RADARR_API_KEY = line.split("=", 1)[1].strip()


def http(method, path, body=None):
    headers = {"X-Api-Key": RADARR_API_KEY, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(RADARR_BASE + path, data=data,
                                 method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read()
        return json.loads(raw) if raw else None


def main():
    try:
        load_env()
    except FileNotFoundError:
        print("[missing-search] no services/.env — skipping (unprovisioned Pi)")
        return 0
    if not RADARR_API_KEY:
        print("[missing-search] RADARR_API_KEY not set in .env — skipping")
        return 0

    if not wait_for_radarr():
        print("[missing-search] Radarr not ready within 120s — "
              "will retry next timer")
        return 1

    # Count the current monitored-but-missing backlog purely for the
    # log line — Radarr's MissingMoviesSearch command does the actual
    # selection internally, so we don't need to enumerate ids ourselves.
    # OSError covers URLError plus raw socket errors (e.g. a connection
    # reset mid-read while Radarr is still warming up).
    try:
        movies = http("GET", "/api/v3/movie")
    except OSError as e:
        print(f"[missing-search] Radarr unreachable ({e}) — will retry next timer")
        return 1

    missing = [m for m in (movies or [])
               if m.get("monitored") and not m.get("hasFile")]
    if not missing:
        print("[missing-search] no monitored-but-missing movies — nothing to do")
        return 0

    titles = ", ".join(sorted(m.get("title", "?") for m in missing)[:8])
    more = "" if len(missing) <= 8 else f" (+{len(missing) - 8} more)"
    print(f"[missing-search] {len(missing)} missing: {titles}{more}")

    # MissingMoviesSearch searches every monitored movie that has no
    # file. Radarr queues + paces the per-indexer queries internally.
    try:
        cmd = http("POST", "/api/v3/command", {"name": "MissingMoviesSearch"})
        cmd_id = cmd.get("id") if isinstance(cmd, dict) else "?"
        print(f"[missing-search] MissingMoviesSearch queued (command id={cmd_id})")
    except OSError as e:
        print(f"[missing-search] failed to queue search ({e})")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
