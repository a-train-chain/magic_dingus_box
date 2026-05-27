#!/usr/bin/env python3
"""Auto-blocklist Radarr queue items stuck in 'warning' state with known-bad signatures.

Radarr distinguishes between:
  - FAILED downloads (qBit reports error / torrent stalled past threshold):
    if autoRedownloadFailed=true, Radarr searches for a replacement.
  - WARNING downloads (completed in qBit but Radarr can't import the file):
    Radarr sits on these forever, waiting for the operator to decide.

The warning state is the scam-completion outcome we saw on 2026-05-26:
trash indexers post torrents whose name reads as a legitimate movie
release (correct title, year, "1080p", quality tag) but whose actual
content is a .txt redirector or a .exe malware payload. The release
passes our quality-profile filters AND our Custom Format scam-rejection
filters (which only see the release TITLE), the torrent completes, the
import fails with "unsupported extension" — and the queue item sits in
warning forever, blocking the operator from getting the real movie.

This watchdog scans Radarr's queue periodically. For each record with
trackedDownloadStatus=warning whose statusMessages contain one of the
known-bad signatures below, it:
  1. Removes the queue item with blocklist=true (so the same release
     never gets grabbed again by this Radarr install)
  2. Asks qBit to delete the underlying torrent + data
  3. Triggers a fresh MoviesSearch for the movie so Radarr finds a
     real alternative (autoRedownloadFailed=true already handles this
     for FAILED items, but warnings need an explicit trigger)

Idempotent + best-effort: if no stuck warnings match the signatures,
exits cleanly without changing anything. Designed to run via a 15-min
systemd timer.
"""
import json
import re
import sys
import urllib.request
import urllib.error
from pathlib import Path

ENV_FILE = Path("/opt/magic_dingus_box/services/.env")
RADARR_BASE = "http://localhost:7878/api/v3"

# Message signatures that mean "this download is junk, blocklist it."
# Conservative — only patterns where we're confident a re-grab is wanted.
# Add patterns here as new scam variants are observed.
BAD_SIGNATURES = [
    r"unsupported extension",
    r"no videos? in folder",
    r"sample file is too large",
    r"sample\b.*not.*acceptable",
    r"invalid video file",
    # File-level: any executable in the imported tree
    r"\.exe\b", r"\.bat\b", r"\.scr\b", r"\.cmd\b", r"\.com\b",
    r"\.vbs\b", r"\.lnk\b", r"\.msi\b", r"\.ps1\b",
]
BAD_RE = re.compile("|".join(BAD_SIGNATURES), re.IGNORECASE)


def env_var(name):
    if not ENV_FILE.exists():
        return None
    with open(ENV_FILE) as f:
        for line in f:
            if line.startswith(name + "="):
                return line.split("=", 1)[1].strip()
    return None


def radarr(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(
        RADARR_BASE + path, data=data, method=method,
        headers={"X-Api-Key": API_KEY, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        raw = r.read()
        return json.loads(raw) if raw else None


API_KEY = env_var("RADARR_API_KEY")
if not API_KEY:
    print("[auto-blocklist] RADARR_API_KEY missing from .env; nothing to do", file=sys.stderr)
    sys.exit(0)


def message_matches_bad_signature(messages_block) -> bool:
    """Walk Radarr's statusMessages structure looking for our signatures.

    statusMessages is a list of dicts each with 'title' + 'messages':
      [{ "title": "outer summary", "messages": ["inner line 1", "inner line 2"] }, ...]
    """
    if not messages_block:
        return False
    for entry in messages_block:
        if BAD_RE.search(entry.get("title", "") or ""):
            return True
        for m in entry.get("messages", []) or []:
            if BAD_RE.search(m or ""):
                return True
    return False


def main() -> int:
    try:
        q = radarr("GET", "/queue?includeUnknownMovieItems=true&pageSize=200")
    except urllib.error.URLError as e:
        print(f"[auto-blocklist] Radarr unreachable: {e}", file=sys.stderr)
        return 0

    records = q.get("records", []) if q else []
    suspects = []
    for r in records:
        if r.get("trackedDownloadStatus") != "warning":
            continue
        if not message_matches_bad_signature(r.get("statusMessages")):
            continue
        suspects.append(r)

    if not suspects:
        print(f"[auto-blocklist] {len(records)} queue items, 0 match bad-signature — nothing to do")
        return 0

    print(f"[auto-blocklist] found {len(suspects)} stuck-warning item(s) to blocklist:")
    affected_movie_ids = set()
    for r in suspects:
        rid = r["id"]
        title = (r.get("title") or "?")[:70]
        movie_id = r.get("movieId")
        print(f"  - id={rid}  movie_id={movie_id}  title={title!r}")
        # Best-effort delete with blocklist + removeFromClient
        try:
            urllib.request.urlopen(urllib.request.Request(
                f"{RADARR_BASE}/queue/{rid}?removeFromClient=true&blocklist=true",
                headers={"X-Api-Key": API_KEY}, method="DELETE"))
            print(f"    ✓ blocklisted + removed from qBit")
            if movie_id:
                affected_movie_ids.add(movie_id)
        except urllib.error.HTTPError as e:
            print(f"    ✗ delete failed: HTTP {e.code} {e.read().decode()[:120]}",
                  file=sys.stderr)

    # Re-trigger search for each affected movie. autoRedownloadFailed
    # handles FAILED items automatically; for blocklisted-warning items
    # we need an explicit kick.
    for mid in affected_movie_ids:
        print(f"[auto-blocklist] re-searching movie {mid}...")
        try:
            radarr("POST", "/command", {"name": "MoviesSearch", "movieIds": [mid]})
            print(f"  ✓ search triggered")
        except Exception as e:
            print(f"  ✗ search failed: {e}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
