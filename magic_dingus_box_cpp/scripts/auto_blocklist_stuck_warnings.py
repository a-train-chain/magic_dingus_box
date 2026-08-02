#!/usr/bin/env python3
"""Auto-blocklist Radarr/Sonarr queue items that will never finish on their own.

Two failure classes are handled, for BOTH *arrs:

1. STUCK WARNINGS (the original 2026-05-26 scam-completion case): trash
   indexers post torrents whose name reads as a legitimate release
   (correct title, year, "1080p", quality tag) but whose actual content
   is a .txt redirector or a .exe malware payload. The release passes
   the quality-profile filters AND the Custom Format scam-rejection
   filters (which only see the release TITLE), the torrent completes,
   the import fails with "unsupported extension" — and the queue item
   sits in warning forever, blocking the operator from getting the real
   content.

2. DEAD-SWARM STALLS (the 2026-08-02 Game of Thrones case): an indexer
   with fabricated seeder counts (TorrentDownload advertised 24 seeders
   on a swarm qBittorrent measured at 0) wins a grab; the torrent sits
   in stalledDL forever. Neither *arr ever recovers on its own — qBit
   stall states surface as a WARNING, never a FAILURE, so Failed
   Download Handling can't fire (Sonarr#7382, closed not-planned) —
   and worse, the stalled item actively rejects every live replacement
   with "Release in queue already meets cutoff". Reap policy,
   deliberately conservative for a family appliance:
     - errorMessage says "stalled with no connections"
     - AND essentially zero progress (<= 2% downloaded; a mid-download
       stall might still recover peers, and re-grabbing throws bytes
       away — leave those for the operator)
     - AND the grab is older than STALL_GRACE_MIN (a fresh torrent
       legitimately spends its first moments at 0 seeds while DHT
       spins up; the healthy GoT replacement showed stalledDL for its
       first ~10 seconds)
   NEVER reaped: paused downloads. The kiosk pauses ALL torrents during
   movie playback (the playback contention guard) and resumes them
   after; paused torrents surface a different message, but guard on the
   word anyway.

For each condemned record this watchdog:
  1. Removes the queue item with blocklist=true (so the same release
     never gets grabbed again) + removeFromClient=true (qBit deletes
     the torrent) + skipRedownload=true (we fire our own search below,
     deterministically, instead of relying on *arr-internal redownload
     behavior that only covers some states)
  2. Triggers a fresh search — MoviesSearch per movie for Radarr,
     SeasonSearch per (series, season) for Sonarr — so a real
     alternative is grabbed. autoRedownloadFailed=true already handles
     FAILED items; warnings and stalls need this explicit kick.

Sonarr caveat: a season pack is N queue rows (one per episode) sharing
one downloadId, and deleting any one row cancels the whole download
(siblings then 404 by design) — so condemned rows are grouped by
downloadId and exactly one row per download is deleted.

Idempotent + best-effort: if nothing matches, exits cleanly without
changing anything. A missing API key skips that *arr's pass (boxes
provisioned before Sonarr existed have no SONARR_API_KEY). Designed to
run via a 15-min systemd timer.
"""
import json
import re
import sys
import urllib.request
import urllib.error
from datetime import datetime, timedelta, timezone
from pathlib import Path

ENV_FILE = Path("/opt/magic_dingus_box/services/.env")
RADARR_BASE = "http://localhost:7878/api/v3"
SONARR_BASE = "http://localhost:8989/api/v3"

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

# Dead-swarm stall reaping (failure class 2 above).
STALL_RE = re.compile(r"stalled with no connections", re.IGNORECASE)
PAUSE_RE = re.compile(r"paused", re.IGNORECASE)
STALL_GRACE_MIN = 45
STALL_MAX_PROGRESS = 0.02


def env_var(name):
    if not ENV_FILE.exists():
        return None
    with open(ENV_FILE) as f:
        for line in f:
            if line.startswith(name + "="):
                return line.split("=", 1)[1].strip()
    return None


def api(base, key, method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(
        base + path, data=data, method=method,
        headers={"X-Api-Key": key, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        raw = r.read()
        return json.loads(raw) if raw else None


def message_matches_bad_signature(messages_block) -> bool:
    """Walk the *arr statusMessages structure looking for our signatures.

    statusMessages is a list of dicts each with 'title' + 'messages':
      [{ "title": "outer summary", "messages": ["inner line 1", ...] }, ...]
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


def is_reapable_stall(rec, now) -> bool:
    """Failure class 2: dead-swarm stall old enough and empty enough to reap."""
    msg = rec.get("errorMessage") or ""
    if PAUSE_RE.search(msg):
        return False
    if not STALL_RE.search(msg):
        return False
    size = rec.get("size") or 0
    sizeleft = rec.get("sizeleft") or 0
    # size 0 = metadata never even fetched; that counts as zero progress.
    if size > 0 and (size - sizeleft) / size > STALL_MAX_PROGRESS:
        return False
    added = rec.get("added")
    if not added:
        return False
    try:
        added_dt = datetime.fromisoformat(added.replace("Z", "+00:00"))
    except ValueError:
        return False
    return (now - added_dt) > timedelta(minutes=STALL_GRACE_MIN)


def condemn(rec, now):
    """Return a reason string if this record should be reaped, else None."""
    if (rec.get("trackedDownloadStatus") == "warning"
            and message_matches_bad_signature(rec.get("statusMessages"))):
        return "bad-signature"
    if is_reapable_stall(rec, now):
        return "dead-swarm stall"
    return None


def radarr_pass(now) -> None:
    key = env_var("RADARR_API_KEY")
    if not key:
        print("[auto-blocklist] RADARR_API_KEY missing from .env; skipping radarr",
              file=sys.stderr)
        return

    try:
        q = api(RADARR_BASE, key, "GET",
                "/queue?includeUnknownMovieItems=true&pageSize=200")
    except OSError as e:
        # OSError, not just urllib.error.URLError. urlopen only wraps failures
        # it hits while ESTABLISHING the connection; a reset part-way through
        # the RESPONSE surfaces raw from http.client as ConnectionResetError,
        # which is an OSError but not a URLError, so it escaped this handler
        # and the unit exited 1.
        #
        # That is a real state on this box: Radarr shares Gluetun's network
        # namespace and gets recreated whenever the tunnel cycles or the
        # storage-attach unit re-links the stack. If this timer happens to fire
        # inside that window it dies, and systemd then reports the unit as
        # failed until the next run up to 15 minutes later — long enough for
        # verify_box to call an otherwise healthy box NOT SHIPPABLE. Observed
        # three times in one session. URLError is an OSError subclass, so this
        # still covers everything the narrower clause did.
        print(f"[auto-blocklist] Radarr unreachable: {e}", file=sys.stderr)
        return

    records = q.get("records", []) if q else []
    suspects = []
    for r in records:
        reason = condemn(r, now)
        if reason:
            suspects.append((r, reason))

    if not suspects:
        print(f"[auto-blocklist] radarr: {len(records)} queue items, "
              "0 condemned — nothing to do")
        return

    print(f"[auto-blocklist] radarr: {len(suspects)} item(s) to blocklist:")
    affected_movie_ids = set()
    for r, reason in suspects:
        rid = r["id"]
        title = (r.get("title") or "?")[:70]
        movie_id = r.get("movieId")
        print(f"  - id={rid}  movie_id={movie_id}  [{reason}]  title={title!r}")
        try:
            api(RADARR_BASE, key, "DELETE",
                f"/queue/{rid}?removeFromClient=true&blocklist=true"
                "&skipRedownload=true")
            print("    ✓ blocklisted + removed from qBit")
            if movie_id:
                affected_movie_ids.add(movie_id)
        except urllib.error.HTTPError as e:
            print(f"    ✗ delete failed: HTTP {e.code} {e.read().decode()[:120]}",
                  file=sys.stderr)

    # Re-trigger search for each affected movie. autoRedownloadFailed
    # handles FAILED items automatically; for blocklisted warning/stall
    # items we need an explicit kick.
    for mid in affected_movie_ids:
        print(f"[auto-blocklist] re-searching movie {mid}...")
        try:
            api(RADARR_BASE, key, "POST", "/command",
                {"name": "MoviesSearch", "movieIds": [mid]})
            print("  ✓ search triggered")
        except Exception as e:
            print(f"  ✗ search failed: {e}", file=sys.stderr)


def sonarr_pass(now) -> None:
    key = env_var("SONARR_API_KEY")
    if not key:
        print("[auto-blocklist] SONARR_API_KEY missing from .env; skipping sonarr")
        return

    try:
        q = api(SONARR_BASE, key, "GET",
                "/queue?includeUnknownSeriesItems=true&pageSize=200")
    except OSError as e:
        # Same netns story as Radarr above — Sonarr rides gluetun too.
        print(f"[auto-blocklist] Sonarr unreachable: {e}", file=sys.stderr)
        return

    records = q.get("records", []) if q else []
    suspects = []
    for r in records:
        reason = condemn(r, now)
        if reason:
            suspects.append((r, reason))

    if not suspects:
        print(f"[auto-blocklist] sonarr: {len(records)} queue items, "
              "0 condemned — nothing to do")
        return

    # A season pack is one row PER EPISODE sharing a downloadId; deleting
    # any one row cancels the whole download (siblings then 404 by
    # design). Group and delete exactly once per download.
    by_download = {}
    for r, reason in suspects:
        by_download.setdefault(r.get("downloadId") or f"row-{r['id']}",
                               (r, reason))

    print(f"[auto-blocklist] sonarr: {len(suspects)} row(s) across "
          f"{len(by_download)} download(s) to blocklist:")
    affected_seasons = set()
    for dl_id, (r, reason) in by_download.items():
        rid = r["id"]
        title = (r.get("title") or "?")[:70]
        print(f"  - id={rid}  download={str(dl_id)[:12]}  [{reason}]  "
              f"title={title!r}")
        try:
            api(SONARR_BASE, key, "DELETE",
                f"/queue/{rid}?removeFromClient=true&blocklist=true"
                "&skipRedownload=true")
            print("    ✓ blocklisted + removed from qBit")
            if r.get("seriesId") and r.get("seasonNumber") is not None:
                affected_seasons.add((r["seriesId"], r["seasonNumber"]))
        except urllib.error.HTTPError as e:
            print(f"    ✗ delete failed: HTTP {e.code} {e.read().decode()[:120]}",
                  file=sys.stderr)

    for series_id, season in sorted(affected_seasons):
        print(f"[auto-blocklist] re-searching series {series_id} "
              f"season {season}...")
        try:
            api(SONARR_BASE, key, "POST", "/command",
                {"name": "SeasonSearch", "seriesId": series_id,
                 "seasonNumber": season})
            print("  ✓ search triggered")
        except Exception as e:
            print(f"  ✗ search failed: {e}", file=sys.stderr)


def main() -> int:
    now = datetime.now(timezone.utc)
    radarr_pass(now)
    sonarr_pass(now)
    return 0


if __name__ == "__main__":
    sys.exit(main())
