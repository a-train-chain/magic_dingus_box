#!/usr/bin/env python3
"""Periodic re-search for monitored movies and TV episodes with no file yet.

Runs on a systemd timer (every 4 hours + shortly after boot). Asks
Radarr to search all monitored-but-missing movies, and Sonarr to search
all monitored-but-missing episodes, so a title that found nothing
grabbable at add-time gets retried automatically instead of sitting
idle until the user manually intervenes.

Why this is needed
------------------
The kiosk's "Add to Library" sets addOptions.searchForMovie=true
(movies) / searchForMissingEpisodes=true (TV), so exactly ONE automatic
search fires the instant a title is added; "Start Season N" likewise
fires ONE SeasonSearch. If that single search comes up empty — because
the good release is not posted yet, the indexer is in a transient
cooldown, or Byparr is mid-Cloudflare-challenge — neither *arr keeps
retrying on its own at a useful cadence (RSS sync only catches
brand-new releases going forward, useless for back-catalog titles).
The title then sits there with no download until the user notices.
Observed live with "Wolfs (2024)": auto-search at add found nothing;
the +50-scoring x264 YTS release the user later grabbed by hand was
simply not available at that exact moment. The TV pipeline has the
identical one-shot fragility (2026-08-02 audit).

This job closes that gap: every few hours it re-runs the search for the
missing backlog on both services. The selection logic itself is already
correct (x264 +50 preferred, HEVC/AV1/foreign/remux rejected by the
custom-format scores) — this just guarantees the backlog keeps getting
retried until a release that passes the quality gate appears.

Idempotent + cheap: a run with zero missing titles is a no-op. Both
*arrs internally queue + rate-limit the per-title searches. A missing
SONARR_API_KEY skips the Sonarr pass (boxes provisioned before Sonarr
existed).
"""
import http.client
import json
import os
import sys
import time
import urllib.request
import urllib.error

NET_ERRORS = (OSError, http.client.HTTPException, ValueError)

RADARR_BASE = "http://localhost:7878"
SONARR_BASE = "http://localhost:8989"

ENV_PATH = "/opt/magic_dingus_box/services/.env"


def load_env(path=ENV_PATH):
    """Parse API keys from the services .env file. Returns a dict."""
    keys = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            for name in ("RADARR_API_KEY", "SONARR_API_KEY"):
                if line.startswith(name + "="):
                    keys[name] = line.split("=", 1)[1].strip()
    return keys


def wait_for_ping(base, timeout_s=300, poll_s=5):
    """Poll a service's /ping until it answers, or give up after timeout_s.

    The timer's Persistent=true boot catch-up fires within a minute of
    boot, while the docker stack is still starting — without this wait
    the run dies on connection-reset. Generous timeout because a failed
    run is NOT retried until the next 4-hour tick: Persistent=true only
    replays triggers missed while the machine was off, it never re-runs
    a unit that ran and failed. 300s covers even an fsck-slow SD boot;
    nothing else contends for this unit (no TimeoutSec is set — oneshot
    defaults to no timeout).
    """
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            req = urllib.request.Request(base + "/ping")
            with urllib.request.urlopen(req, timeout=5) as r:
                if r.status == 200:
                    return True
        except NET_ERRORS:
            pass
        time.sleep(poll_s)
    return False


def http(base, key, method, path, body=None):
    headers = {"X-Api-Key": key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(base + path, data=data,
                                 method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read()
        return json.loads(raw) if raw else None


def radarr_pass(key) -> int:
    if not wait_for_ping(RADARR_BASE):
        print("[missing-search] Radarr not ready within 300s — "
              "will retry next timer")
        return 1

    # Count the current monitored-but-missing backlog purely for the
    # log line — Radarr's MissingMoviesSearch command does the actual
    # selection internally, so we don't need to enumerate ids ourselves.
    # OSError covers URLError plus raw socket errors (e.g. a connection
    # reset mid-read while Radarr is still warming up).
    try:
        movies = http(RADARR_BASE, key, "GET", "/api/v3/movie")
    except NET_ERRORS as e:
        print(f"[missing-search] Radarr unreachable ({e}) — will retry next timer")
        return 1

    missing = [m for m in (movies or [])
               if m.get("monitored") and not m.get("hasFile")]
    if not missing:
        print("[missing-search] no monitored-but-missing movies — nothing to do")
        return 0

    titles = ", ".join(sorted(m.get("title", "?") for m in missing)[:8])
    more = "" if len(missing) <= 8 else f" (+{len(missing) - 8} more)"
    print(f"[missing-search] {len(missing)} missing movie(s): {titles}{more}")

    # MissingMoviesSearch searches every monitored movie that has no
    # file. Radarr queues + paces the per-indexer queries internally.
    try:
        cmd = http(RADARR_BASE, key, "POST", "/api/v3/command",
                   {"name": "MissingMoviesSearch"})
        cmd_id = cmd.get("id") if isinstance(cmd, dict) else "?"
        print(f"[missing-search] MissingMoviesSearch queued (command id={cmd_id})")
    except NET_ERRORS as e:
        print(f"[missing-search] failed to queue movie search ({e})")
        return 1
    return 0


def sonarr_pass(key) -> int:
    if not wait_for_ping(SONARR_BASE):
        print("[missing-search] Sonarr not ready within 300s — "
              "will retry next timer")
        return 1

    # Count the missing-episode backlog for the log line. wanted/missing
    # already filters to monitored episodes of monitored series whose
    # air date has passed — one page is enough for the count.
    try:
        wanted = http(SONARR_BASE, key, "GET",
                      "/api/v3/wanted/missing?pageSize=1"
                      "&includeSeries=false&monitored=true")
    except NET_ERRORS as e:
        print(f"[missing-search] Sonarr unreachable ({e}) — will retry next timer")
        return 1

    total = (wanted or {}).get("totalRecords", 0)
    if not total:
        print("[missing-search] no monitored-but-missing episodes — nothing to do")
        return 0

    print(f"[missing-search] {total} missing episode(s)")

    # MissingEpisodeSearch is the library-wide sweep — Sonarr's own
    # mirror of MissingMoviesSearch. It searches every monitored episode
    # with no file, pacing per-indexer queries internally.
    try:
        cmd = http(SONARR_BASE, key, "POST", "/api/v3/command",
                   {"name": "MissingEpisodeSearch"})
        cmd_id = cmd.get("id") if isinstance(cmd, dict) else "?"
        print(f"[missing-search] MissingEpisodeSearch queued (command id={cmd_id})")
    except NET_ERRORS as e:
        print(f"[missing-search] failed to queue episode search ({e})")
        return 1
    return 0


def main():
    try:
        keys = load_env()
    except FileNotFoundError:
        print("[missing-search] no services/.env — skipping (unprovisioned Pi)")
        return 0

    rc = 0
    if keys.get("RADARR_API_KEY"):
        rc = max(rc, radarr_pass(keys["RADARR_API_KEY"]))
    else:
        print("[missing-search] RADARR_API_KEY not set in .env — skipping movies")

    if keys.get("SONARR_API_KEY"):
        rc = max(rc, sonarr_pass(keys["SONARR_API_KEY"]))
    else:
        print("[missing-search] SONARR_API_KEY not set in .env — skipping TV")

    return rc


if __name__ == "__main__":
    sys.exit(main())
