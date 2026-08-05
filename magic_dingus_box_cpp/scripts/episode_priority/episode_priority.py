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


# ---------------------------------------------------------------------------
# Season packs: order episodes WITHIN one torrent, via per-file priority.
# ---------------------------------------------------------------------------
# The queue reordering above deliberately skips packs, and that reasoning is
# correct for what it does: moving a single pack around the queue achieves
# nothing. But it left a real hole. When Sonarr grabs a SEASON PACK -- often
# the only or the best-quality option -- qBittorrent downloads all ten
# episodes concurrently at equal priority, so NOTHING is playable until
# roughly everything is. Measured on hardware 2026-08-04, House of the
# Dragon S01: all 10 files at priority 1, between 0.0% and 5.7%, ETA 3.6h
# before a single episode could be watched.
#
# Setting per-file priority is a different operation from queue order, and
# qBit supports it: /api/v2/torrents/filePrio. Give the earliest incomplete
# episode MAXIMUM and the next HIGH, leave the rest NORMAL, and qBit finishes
# them in order -- E1 lands in minutes instead of hours.
#
# Priority 0 (skip) is NEVER used. A skipped file is excluded from the
# torrent, which would leave the pack permanently incomplete and block
# Sonarr's import of the whole season.
PRIO_NORMAL = 1
PRIO_HIGH = 6
PRIO_MAX = 7

# A file is "done enough" to stop prioritising well before 100%: the tail of
# a piece can lag while the episode is already fully playable, and holding
# MAX on it starves the next episode.
FILE_DONE = 0.999


def plan_file_prios(files):
    """{file_index: new_priority} for one pack's file list. Pure.

    Returns only files whose priority must CHANGE, so a correctly ordered
    pack produces zero API calls -- the same idempotence property the queue
    planner relies on.
    """
    eps = []
    for idx, f in enumerate(files):
        name = f.get("name", "")
        m = EPISODE_RE.search(name)
        if not m:
            continue  # RARBG.txt, samples, artwork -- left alone entirely
        eps.append((int(m.group(1)), int(m.group(2)), idx, f))
    if len(eps) < 2:
        return {}  # single-episode torrent: the queue planner owns it

    eps.sort(key=lambda e: (e[0], e[1]))
    incomplete = [e for e in eps if (e[3].get("progress") or 0) < FILE_DONE]

    want = {}
    for rank, (_s, _e, idx, _f) in enumerate(incomplete):
        want[idx] = PRIO_MAX if rank == 0 else (PRIO_HIGH if rank == 1
                                                else PRIO_NORMAL)
    # Finished episodes drop back to NORMAL so they stop competing.
    for _s, _e, idx, f in eps:
        if idx not in want:
            want[idx] = PRIO_NORMAL

    return {i: p for i, p in want.items()
            if (files[i].get("priority") or PRIO_NORMAL) != p}


def is_pack(name):
    """True for a season pack or multi-episode range -- what plan_moves skips."""
    return bool(PACK_RE.search(name) or RANGE_RE.search(name))


def qbit_files(sid, torrent_hash):
    req = urllib.request.Request(
        QBIT_BASE + "/api/v2/torrents/files?hash="
        + urllib.parse.quote(torrent_hash),
        headers={"Cookie": sid})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())


def qbit_set_file_prio(sid, torrent_hash, ids, priority):
    data = urllib.parse.urlencode({
        "hash": torrent_hash,
        "id": "|".join(str(i) for i in ids),
        "priority": str(priority),
    }).encode()
    req = urllib.request.Request(QBIT_BASE + "/api/v2/torrents/filePrio",
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
    # Season packs: order episodes inside the torrent.
    packs = 0
    prio_calls = 0
    for t in torrents:
        if t.get("state") not in DOWNLOAD_STATES:
            continue
        if not is_pack(t.get("name", "")):
            continue
        packs += 1
        try:
            files = qbit_files(sid, t["hash"])
            changes = plan_file_prios(files)
            # Group indices by target priority -- one POST per distinct
            # priority instead of one per file.
            by_prio = {}
            for idx, prio in changes.items():
                by_prio.setdefault(prio, []).append(idx)
            for prio, ids in sorted(by_prio.items()):
                qbit_set_file_prio(sid, t["hash"], ids, prio)
                prio_calls += 1
        except NET_ERRORS as e:
            # Same fail-soft contract as the queue moves: ordering is
            # ephemeral and the next tick redoes it.
            print(f"[episode-priority] filePrio failed on "
                  f"{t.get('name','?')[:40]} ({e}) — left to next tick")
            continue

    print(f"[episode-priority] {len(groups)} group(s) checked, "
          f"{made} move(s) made; {packs} pack(s) checked, "
          f"{prio_calls} file-priority call(s)")
    return 0


# ---------------------------------------------------------------------------
# --self-test: table cases over the pure planning core. No network, no .env.
# ---------------------------------------------------------------------------

def _t(name, h, state="downloading", priority=1):
    return {"name": name, "hash": h, "state": state, "priority": priority}


def _f(name, prio=1, progress=0.0):
    return {"name": name, "priority": prio, "progress": progress}


def _pack_tests():
    """Season-pack per-file prioritisation. Pure, no network."""
    P = "House.of.the.Dragon.S01"
    # 1. Fresh pack, all normal -> E1 MAX, E2 HIGH, rest untouched (already 1).
    files = [_f(f"{P}E{n:02d}.1080p.WEB.x264") for n in range(1, 11)]
    files.append(_f("RARBG.txt"))
    got = plan_file_prios(files)
    assert got == {0: PRIO_MAX, 1: PRIO_HIGH}, got
    assert 10 not in got, "non-episode file must never be touched"

    # 2. Idempotent: re-running on the result produces zero calls. This is
    #    what makes the 2-minute cadence free.
    files[0]["priority"] = PRIO_MAX
    files[1]["priority"] = PRIO_HIGH
    assert plan_file_prios(files) == {}, plan_file_prios(files)

    # 3. E1 finishes -> MAX rolls to E2, HIGH to E3, and E1 drops back to
    #    NORMAL so it stops competing for bandwidth.
    files[0]["progress"] = 1.0
    got = plan_file_prios(files)
    assert got[0] == PRIO_NORMAL, got
    assert got[1] == PRIO_MAX, got
    assert got[2] == PRIO_HIGH, got

    # 4. A file at 99.95% counts as done -- the tail of a piece must not
    #    starve the next episode.
    files2 = [_f(f"{P}E{n:02d}.1080p") for n in range(1, 4)]
    files2[0]["progress"] = 0.9995
    got = plan_file_prios(files2)
    assert got[1] == PRIO_MAX, got

    # 5. Out-of-order file listing still orders by EPISODE, not by index.
    files3 = [_f(f"{P}E03.1080p"), _f(f"{P}E01.1080p"), _f(f"{P}E02.1080p")]
    got = plan_file_prios(files3)
    assert got[1] == PRIO_MAX and got[2] == PRIO_HIGH, got

    # 6. Single-episode torrent -> the QUEUE planner owns it, not this.
    assert plan_file_prios([_f("Show.S01E01.1080p")]) == {}

    # 7. PRIO_SKIP is never emitted: a skipped file is excluded from the
    #    torrent and would block Sonarr's import of the whole season.
    files4 = [_f(f"{P}E{n:02d}.1080p") for n in range(1, 11)]
    assert 0 not in plan_file_prios(files4).values()

    # 8. is_pack agrees with what plan_moves skips.
    assert is_pack("House.of.the.Dragon.S01.1080p.HMAX.WEBRip")
    assert is_pack("Show.S02E01-E05.1080p")
    assert not is_pack("Show.S01E01.1080p.x264")
    print("  pack tests: 8/8 passed")


def self_test():
    _pack_tests()
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
