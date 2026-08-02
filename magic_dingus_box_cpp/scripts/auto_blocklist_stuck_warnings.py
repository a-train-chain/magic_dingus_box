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
     - AND (the two-strike rule) the SAME download was already
       stall-condemned on a previous run >= STALL_PERSIST_MIN ago with
       sizeleft UNCHANGED. One observation is not enough: qBit reports
       stalledDL during the perfectly healthy reconnection window right
       after torrents are RESUMED — and the kiosk resumes all torrents
       at the end of every movie (the playback contention guard) and on
       every boot. The *arrs cache that message in the queue API for up
       to ~1 min between refreshes, so a single 15-min timer tick can
       absolutely land inside it. Candidates live in a state file under
       /tmp (tmpfs): a reboot wipes it, so the boot-catch-up run can
       never reap on stale pre-reboot evidence — dead torrents just
       wait one extra tick. Any progress (sizeleft change) resets the
       candidacy clock.
   NEVER reaped: paused downloads. The kiosk pauses ALL torrents during
   movie playback; paused/stopped torrents carry NO errorMessage at all
   (verified against Sonarr 4.0.19 / Radarr 5.14 source: stoppedDL and
   pausedDL map to Paused with no message set), so STALL_RE has nothing
   to match. PAUSE_RE is kept as cheap insurance should that mapping
   ever change. Note the stall string is UI-language-localized — on a
   non-English *arr the reaper goes inert (fail-safe direction).

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
# Two-strike persistence: a stall-condemned download is only reaped when
# it was ALSO condemned >= this many minutes earlier with no progress in
# between. 12 rather than 15 so normal 15-min timer jitter can't make
# every second strike miss by seconds. /tmp is tmpfs — reboot resets.
STALL_PERSIST_MIN = 12
STALL_STATE_FILE = Path("/tmp/mdb_stall_candidates.json")


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


def is_stall_shaped(rec, now) -> bool:
    """Failure class 2 shape: stalled message, ~no progress, past grace."""
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


def load_stall_state():
    """{download_id: {"first_seen": unix_ts, "sizeleft": int}} — corrupt/absent = {}."""
    try:
        with open(STALL_STATE_FILE) as f:
            state = json.load(f)
        return state if isinstance(state, dict) else {}
    except (OSError, ValueError):
        return {}


def save_stall_state(state):
    try:
        tmp = STALL_STATE_FILE.with_suffix(".tmp")
        with open(tmp, "w") as f:
            json.dump(state, f)
        tmp.replace(STALL_STATE_FILE)
    except OSError as e:
        print(f"[auto-blocklist] could not persist stall state: {e}",
              file=sys.stderr)


def stall_strike(rec, now, state) -> bool:
    """Two-strike gate. Registers/refreshes the candidate in `state`;
    returns True only when this download was already condemned
    >= STALL_PERSIST_MIN ago with sizeleft unchanged since."""
    dl_id = str(rec.get("downloadId") or f"row-{rec['id']}")
    sizeleft = rec.get("sizeleft") or 0
    now_ts = now.timestamp()
    prev = state.get(dl_id)
    if prev and prev.get("sizeleft") == sizeleft:
        return (now_ts - prev.get("first_seen", now_ts)) >= STALL_PERSIST_MIN * 60
    # New candidate, or it made progress since last time — (re)start the clock.
    state[dl_id] = {"first_seen": now_ts, "sizeleft": sizeleft}
    return False


def condemn(rec, now, stall_state):
    """Return a reason string if this record should be reaped, else None.

    Mutates stall_state (candidate registration) as a side effect.
    """
    if (rec.get("trackedDownloadStatus") == "warning"
            and message_matches_bad_signature(rec.get("statusMessages"))):
        return "bad-signature"
    if is_stall_shaped(rec, now) and stall_strike(rec, now, stall_state):
        return "dead-swarm stall"
    return None


def stall_shaped_ids(records, now):
    """downloadIds of every currently stall-shaped record (for state pruning)."""
    return {str(r.get("downloadId") or f"row-{r['id']}")
            for r in records if is_stall_shaped(r, now)}


def radarr_pass(now, stall_state):
    """Returns the set of currently stall-shaped downloadIds, or None if the
    service could not be observed (state pruning must then be skipped)."""
    key = env_var("RADARR_API_KEY")
    if not key:
        print("[auto-blocklist] RADARR_API_KEY missing from .env; skipping radarr",
              file=sys.stderr)
        return set()

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
        return None

    records = q.get("records", []) if q else []
    seen = stall_shaped_ids(records, now)
    suspects = []
    for r in records:
        reason = condemn(r, now, stall_state)
        if reason:
            suspects.append((r, reason))

    if not suspects:
        print(f"[auto-blocklist] radarr: {len(records)} queue items, "
              "0 condemned — nothing to do")
        return seen

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
            body = e.read().decode(errors="replace")[:120]
            print(f"    ✗ delete failed: HTTP {e.code} {body}", file=sys.stderr)
        except OSError as e:
            # Same netns-cycle class the GET guard documents — the service
            # went away mid-run. Bail out of the delete loop (every further
            # call would burn its full timeout too); already-deleted items
            # get their search below (each attempt individually guarded),
            # and the 4h missing sweep is the backstop for anything missed.
            print(f"    ✗ Radarr went away mid-run: {e}", file=sys.stderr)
            break

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


def sonarr_search_already_active(key, series_id, season) -> bool:
    """Cheap idempotence guard against a double-grab: skip firing a
    SeasonSearch when Sonarr already has an equivalent search in flight
    (the 4h MissingEpisodeSearch sweep, or a kiosk-triggered
    SeasonSearch). Best-effort — any error means 'not active'."""
    try:
        cmds = api(SONARR_BASE, key, "GET", "/command") or []
        for c in cmds:
            if c.get("status") not in ("queued", "started"):
                continue
            name = c.get("name")
            body = c.get("body") or {}
            if name == "MissingEpisodeSearch":
                return True
            if (name == "SeasonSearch"
                    and body.get("seriesId") == series_id
                    and body.get("seasonNumber") == season):
                return True
    except Exception:
        pass
    return False


def sonarr_pass(now, stall_state):
    """Returns the set of currently stall-shaped downloadIds, or None if the
    service could not be observed (state pruning must then be skipped)."""
    key = env_var("SONARR_API_KEY")
    if not key:
        print("[auto-blocklist] SONARR_API_KEY missing from .env; skipping sonarr")
        return set()

    try:
        q = api(SONARR_BASE, key, "GET",
                "/queue?includeUnknownSeriesItems=true&pageSize=200")
    except OSError as e:
        # Same netns story as Radarr above — Sonarr rides gluetun too.
        print(f"[auto-blocklist] Sonarr unreachable: {e}", file=sys.stderr)
        return None

    records = q.get("records", []) if q else []
    seen = stall_shaped_ids(records, now)
    suspects = []
    for r in records:
        reason = condemn(r, now, stall_state)
        if reason:
            suspects.append((r, reason))

    if not suspects:
        print(f"[auto-blocklist] sonarr: {len(records)} queue items, "
              "0 condemned — nothing to do")
        return seen

    # A season pack is one row PER EPISODE sharing a downloadId; deleting
    # any one row cancels the whole download (siblings then 404 by
    # design). Group and delete exactly once per download — but collect
    # the (series, season) of EVERY condemned row, so a pack spanning
    # multiple seasons re-searches all of them.
    by_download = {}
    for r, reason in suspects:
        key_id = str(r.get("downloadId") or f"row-{r['id']}")
        by_download.setdefault(key_id, []).append((r, reason))

    print(f"[auto-blocklist] sonarr: {len(suspects)} row(s) across "
          f"{len(by_download)} download(s) to blocklist:")
    affected_seasons = set()
    for dl_id, rows in by_download.items():
        r, reason = rows[0]
        rid = r["id"]
        title = (r.get("title") or "?")[:70]
        print(f"  - id={rid}  download={dl_id[:12]}  [{reason}]  "
              f"title={title!r}")
        try:
            api(SONARR_BASE, key, "DELETE",
                f"/queue/{rid}?removeFromClient=true&blocklist=true"
                "&skipRedownload=true")
            print("    ✓ blocklisted + removed from qBit")
            for rr, _ in rows:
                if rr.get("seriesId") and rr.get("seasonNumber") is not None:
                    affected_seasons.add((rr["seriesId"], rr["seasonNumber"]))
        except urllib.error.HTTPError as e:
            body = e.read().decode(errors="replace")[:120]
            print(f"    ✗ delete failed: HTTP {e.code} {body}", file=sys.stderr)
        except OSError as e:
            print(f"    ✗ Sonarr went away mid-run: {e}", file=sys.stderr)
            break

    for series_id, season in sorted(affected_seasons):
        if sonarr_search_already_active(key, series_id, season):
            print(f"[auto-blocklist] series {series_id} season {season}: "
                  "an equivalent search is already in flight — skipping")
            continue
        print(f"[auto-blocklist] re-searching series {series_id} "
              f"season {season}...")
        try:
            api(SONARR_BASE, key, "POST", "/command",
                {"name": "SeasonSearch", "seriesId": series_id,
                 "seasonNumber": season})
            print("  ✓ search triggered")
        except Exception as e:
            print(f"  ✗ search failed: {e}", file=sys.stderr)

    return seen


def main() -> int:
    now = datetime.now(timezone.utc)
    stall_state = load_stall_state()
    seen_radarr = radarr_pass(now, stall_state)
    seen_sonarr = sonarr_pass(now, stall_state)

    # Prune candidates that are no longer stall-shaped anywhere (they
    # recovered, finished, or were removed) — but only when BOTH services
    # were actually observed this run; an unreachable service must not
    # cause its candidates' clocks to reset.
    if seen_radarr is not None and seen_sonarr is not None:
        keep = seen_radarr | seen_sonarr
        for k in list(stall_state):
            if k not in keep:
                del stall_state[k]
    save_stall_state(stall_state)
    return 0


if __name__ == "__main__":
    sys.exit(main())
