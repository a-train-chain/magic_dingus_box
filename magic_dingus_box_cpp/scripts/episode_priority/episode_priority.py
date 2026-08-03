#!/usr/bin/env python3
"""Keep per-episode TV torrents downloading in EPISODE order.

Runs on a systemd timer (every 2 minutes). Looks at qBittorrent's
`sonarr` category, finds SINGLE-EPISODE torrents that are still
queued/downloading, groups them by (series, season), and reorders
qBit's download queue so the lowest un-finished episode of each group
downloads first.

Why this is needed
------------------
The kiosk's Quick Start fires a single-episode E1 search alongside
every season-pack search so something watchable lands in minutes
(~2 GB single vs a 10-30 GB pack). The auto-blocklist and
missing-search timers can then backfill further per-episode singles —
but qBittorrent downloads queued torrents in the order they were
ADDED, which for search results is indexer-response order, not episode
order. A binge session wants E2 arriving before E5: this timer keeps
each group's queue position ascending by episode number, so the next
episode is always the next download.

Scope is deliberately narrow:
  - single-episode torrents only (SxxEyy names); season packs and
    multi-episode ranges (Sxx bare, Eaa-Ebb) are never touched — the
    pack IS the backfill and reordering it would achieve nothing;
  - only queued/downloading states; completed, seeding, paused and
    errored torrents keep whatever position they have;
  - groups never cross-interfere: each (series, season) is checked and
    fixed on its own.

IDEMPOTENT: a correctly ordered queue produces ZERO API calls — the
2-minute cadence costs one login + one torrents/info round-trip.

The fix uses topPrio (move to top of queue) on each of a wrong group's
torrents in DESCENDING episode order: the last call wins the top slot,
so after the sequence the group sits at the head of the queue in
ascending episode order, and the next run sees it as correct.

Exit discipline: qBittorrent being down/unreachable exits 0, not 1 —
at a 2-minute cadence a failing unit would flap systemd's failed-unit
state on every Gluetun blip, and ordering is ephemeral anyway (the
next tick redoes the whole decision from scratch; there is nothing to
catch up on, which is also why the timer sets Persistent=false).
"""
import http.client
import json
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

# OSError covers URLError plus raw socket errors (e.g. a connection
# reset mid-read while qBit is still warming up) — same tuple and
# reasoning as missing_search.py.
NET_ERRORS = (OSError, http.client.HTTPException, ValueError)

QBIT_BASE = "http://localhost:8080"
QBIT_USER = "admin"
CATEGORY = "sonarr"

# MDB_QBIT_PASS is written by setup_services.sh Step 7.6 and re-applied
# by the boot-time magic-dingus-sync-qbit-password.service oneshot; it
# is the canonical name the kiosk's QbittorrentClient reads too.
ENV_PATH = "/opt/magic_dingus_box/services/.env"

# Actively-fetching states only. Completed/seeding (uploading, stalledUP,
# pausedUP/stoppedUP, ...), errored ("error", "missingFiles") and paused
# downloads (pausedDL/stoppedDL — an operator's explicit choice) are all
# excluded: reordering them is meaningless or rude respectively.
DOWNLOAD_STATES = {
    "downloading", "stalledDL", "metaDL", "queuedDL",
    "forcedDL", "allocating", "checkingDL",
}

# Single-episode shape: SxxEyy. The two exclusions run FIRST because a
# match alone is not enough: a season pack named "Show.S01.COMPLETE"
# has a bare Sxx token (Sxx NOT followed by E), and a multi-episode
# release "Show.S01E01-E03" carries an Eaa-Ebb range — both must never
# be treated as (or reordered around) a single episode.
EPISODE_RE = re.compile(r"S(\d{2})E(\d{2})", re.IGNORECASE)
PACK_RE = re.compile(r"S\d{2}(?!E)", re.IGNORECASE)
RANGE_RE = re.compile(r"E\d+-E\d+", re.IGNORECASE)


def parse_episode(name):
    """(season, episode, series_prefix) for a single-episode name, else None."""
    if RANGE_RE.search(name):
        return None
    if PACK_RE.search(name):
        return None
    m = EPISODE_RE.search(name)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2)), name[: m.start()]


def normalize_series(prefix):
    """Lowercase-alnum collapse so 'Breaking.Bad.' == 'breaking bad'."""
    return re.sub(r"[^a-z0-9]", "", prefix.lower())


def group_torrents(torrents):
    """Group ELIGIBLE torrents by (normalized series, season). Pure.

    Eligible = single-episode name AND an actively-fetching state. Each
    entry is a shallow copy with the parsed episode number added under
    "_episode" — the input dicts are never mutated (plan_moves and the
    self-tests rely on that).
    """
    groups = {}
    for t in torrents:
        if t.get("state", "") not in DOWNLOAD_STATES:
            continue
        parsed = parse_episode(t.get("name", ""))
        if parsed is None:
            continue
        season, episode, prefix = parsed
        key = (normalize_series(prefix), season)
        groups.setdefault(key, []).append({**t, "_episode": episode})
    return groups


def plan_moves(torrents):
    """Hashes to topPrio, in POST order. Empty when already correct. Pure.

    The invariant checked per group: among the group's QUEUED members
    (priority >= 1; 0/-1 means qBit does not hold the torrent in the
    queue at all, so it has no position to reason about), queue position
    ascending must equal episode number ascending. When it does not, the
    fix is every queued member in DESCENDING episode order — topPrio
    moves to the head, so the last (lowest) episode ends up first and
    the whole group lands at the head in ascending order. The re-check
    next run then passes: zero further POSTs (the idempotence the
    2-minute cadence depends on).

    Duplicate episode numbers (two releases of the same episode) tie-
    break by current priority in BOTH the check and the fix, so a dup
    can never flap the plan between runs.
    """
    moves = []
    all_groups = group_torrents(torrents)
    for key in sorted(all_groups):  # deterministic cross-group order
        queued = [t for t in all_groups[key]
                  if int(t.get("priority", 0)) >= 1]
        if len(queued) < 2:
            continue  # nothing to order
        by_ep = sorted(queued,
                       key=lambda t: (t["_episode"], int(t["priority"])))
        prios = [int(t["priority"]) for t in by_ep]
        if prios == sorted(prios):
            continue  # this group already downloads in episode order
        moves.extend(t["hash"] for t in reversed(by_ep))
    return moves


def load_env(path=ENV_PATH):
    """Parse qBit credentials from the services .env file. Returns a dict."""
    vals = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("MDB_QBIT_PASS="):
                vals["MDB_QBIT_PASS"] = line.split("=", 1)[1].strip()
    return vals


def qbit_login(password):
    """POST /auth/login; returns the SID cookie ('SID=...') or None."""
    data = urllib.parse.urlencode(
        {"username": QBIT_USER, "password": password}).encode()
    req = urllib.request.Request(QBIT_BASE + "/api/v2/auth/login",
                                 data=data, method="POST")
    with urllib.request.urlopen(req, timeout=10) as r:
        body = r.read().decode(errors="replace")
        cookie = r.headers.get("Set-Cookie", "") or ""
    # qBit answers 200 with a literal "Ok."/"Fails." body — the status
    # code alone cannot distinguish a bad password.
    if "Ok." not in body:
        return None
    sid = cookie.split(";", 1)[0].strip()
    return sid or None


def qbit_torrents(sid):
    req = urllib.request.Request(
        QBIT_BASE + "/api/v2/torrents/info?category="
        + urllib.parse.quote(CATEGORY),
        headers={"Cookie": sid})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())


def qbit_top_prio(sid, torrent_hash):
    data = urllib.parse.urlencode({"hashes": torrent_hash}).encode()
    req = urllib.request.Request(QBIT_BASE + "/api/v2/torrents/topPrio",
                                 data=data, method="POST",
                                 headers={"Cookie": sid})
    with urllib.request.urlopen(req, timeout=30) as r:
        r.read()


def run():
    try:
        env = load_env()
    except FileNotFoundError:
        print("[episode-priority] no services/.env — skipping "
              "(unprovisioned Pi)")
        return 0
    password = env.get("MDB_QBIT_PASS", "")
    if not password:
        print("[episode-priority] MDB_QBIT_PASS not set in .env — skipping")
        return 0

    try:
        sid = qbit_login(password)
        if sid is None:
            # Wrong password is a provisioning-drift state the boot-time
            # password-sync oneshot heals; flapping this unit red every
            # 2 minutes meanwhile would only bury that signal.
            print("[episode-priority] qBittorrent login refused — skipping")
            return 0
        torrents = qbit_torrents(sid)
    except NET_ERRORS as e:
        # qBit down/unreachable (Gluetun blip, stack restarting) must not
        # flap the unit — ordering is ephemeral, the next tick redoes it.
        print(f"[episode-priority] qBittorrent unreachable ({e}) — skipping")
        return 0

    groups = group_torrents(torrents)
    moves = plan_moves(torrents)
    made = 0
    for h in moves:
        try:
            qbit_top_prio(sid, h)
            made += 1
        except NET_ERRORS as e:
            # A 409 here means torrent queueing is disabled in qBit
            # (topPrio has nothing to act on); anything else is the same
            # transient-unreachable class as above. Either way: log,
            # stop POSTing, still exit 0.
            print(f"[episode-priority] topPrio failed after {made} "
                  f"move(s) ({e}) — remaining moves left to next tick")
            break
    print(f"[episode-priority] {len(groups)} group(s) checked, "
          f"{made} move(s) made")
    return 0


# ---------------------------------------------------------------------------
# --self-test: table cases over the pure planning core. No network, no .env.
# ---------------------------------------------------------------------------

def _t(name, h, state="downloading", priority=1):
    return {"name": name, "hash": h, "state": state, "priority": priority}


def self_test():
    # 1. Correct order → zero moves (the idempotence property the
    #    2-minute cadence depends on).
    ts = [_t("Show.S01E01.720p.x264", "a", priority=1),
          _t("Show.S01E02.720p.x264", "b", priority=2),
          _t("Show.S01E03.720p.x264", "c", priority=3)]
    assert plan_moves(ts) == [], plan_moves(ts)

    # 2. Reversed → topPrio in DESCENDING episode order, so the queue
    #    ends ascending (last call wins the top slot).
    ts = [_t("Show.S01E01.720p.x264", "a", priority=3),
          _t("Show.S01E02.720p.x264", "b", priority=2),
          _t("Show.S01E03.720p.x264", "c", priority=1)]
    assert plan_moves(ts) == ["c", "b", "a"], plan_moves(ts)

    # 3. Season pack and multi-episode range are excluded: the fix for
    #    the wrongly-ordered singles never touches them.
    ts = [_t("Show.S01.COMPLETE.720p.x264", "pack", priority=1),
          _t("Show.S01E01-E03.720p.x264", "range", priority=2),
          _t("Show.S01E02.720p.x264", "b", priority=3),
          _t("Show.S01E01.720p.x264", "a", priority=4)]
    assert plan_moves(ts) == ["b", "a"], plan_moves(ts)

    # 4. Mixed series don't cross-interfere: a correctly-ordered series
    #    contributes nothing even while another needs fixing, and the
    #    same series' seasons are separate groups.
    ts = [_t("Alpha.Show.S01E01.720p", "a1", priority=1),
          _t("Alpha.Show.S01E02.720p", "a2", priority=2),
          _t("Beta.Show.S01E01.720p", "b1", priority=4),
          _t("Beta.Show.S01E02.720p", "b2", priority=3),
          _t("Alpha.Show.S02E05.720p", "s2", priority=5)]
    assert plan_moves(ts) == ["b2", "b1"], plan_moves(ts)

    # 5. Completed/seeding torrents are skipped entirely: E01 done and
    #    uploading keeps its place; the still-downloading E02/E03 pair is
    #    fixed among themselves.
    ts = [_t("Show.S01E01.720p", "done", state="uploading", priority=0),
          _t("Show.S01E02.720p", "b", priority=2),
          _t("Show.S01E03.720p", "c", priority=1)]
    assert plan_moves(ts) == ["c", "b"], plan_moves(ts)

    # 6. Not-queued members (priority 0/-1: queueing off, or a forced
    #    torrent) have no position to reason about and never plan moves.
    ts = [_t("Show.S01E01.720p", "a", priority=0),
          _t("Show.S01E02.720p", "b", priority=-1)]
    assert plan_moves(ts) == [], plan_moves(ts)

    # 7. Duplicate episode numbers tie-break by current priority in both
    #    the check and the fix — a settled dup pair never re-plans.
    ts = [_t("Show.S01E01.720p.YIFY", "a1", priority=1),
          _t("Show.S01E01.1080p.RARBG", "a2", priority=2),
          _t("Show.S01E02.720p", "b", priority=3)]
    assert plan_moves(ts) == [], plan_moves(ts)

    # 8. group_torrents never mutates its input (purity contract).
    original = _t("Show.S01E01.720p", "a", priority=1)
    group_torrents([original])
    assert "_episode" not in original

    print("[episode-priority] self-test: all cases passed")
    return 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    return run()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
