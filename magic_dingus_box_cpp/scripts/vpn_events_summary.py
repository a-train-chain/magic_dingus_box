#!/usr/bin/env python3
"""Summarize the VPN tunnel event log for verify_box.sh (the Box Health card).

gluetun_cascade_restart.sh appends one line per tunnel transition to
/var/lib/magic-dingus/vpn_events.log (`<epoch> <kind> [detail]`, kinds
watch / unhealthy <portfwd|tunnel> / healthy / restart — see the notes in
that script). Before the log existed, ~170 drops over two days on the
owner's box were only discoverable by digging through the journal.

This turns the log into one plain-language Box Health line:

    pass<TAB>VPN tunnel dropped 2 times in the last 24 h (down 11 min total, ...)
    warn<TAB>VPN tunnel dropped 23 times in the last 24 h (...) — ...

`summarize()` and `verdict()` are pure (no clock, no I/O) and unit-tested in
scripts/tests/test_vpn_events_summary.py. Stdlib only: it runs on the box
under the system python3.

Usage: vpn_events_summary.py [--file PATH] [--now EPOCH]
                             [--current-health healthy|unhealthy|...] [--json]
Exit 0 whenever a verdict was printed (a missing file is a verdict too).
"""
from __future__ import annotations

import argparse
import json
import sys
import time

DEFAULT_FILE = "/var/lib/magic-dingus/vpn_events.log"
WINDOW_S = 24 * 3600

# WARN thresholds. Calibrated against the owner's box: a sound tunnel logs
# zero or one drop a day; the 2026-10-03/04 burst was one every ~11 min.
WARN_DROPS = 6
WARN_DOWN_S = 30 * 60
# An outage still open longer than this is "down right now", not a blip the
# watcher is about to handle (it confirms for 5 min, then restarts).
WARN_ONGOING_S = 10 * 60


def parse(lines):
    """[(ts, kind, detail)] sorted by time; malformed lines are skipped.

    Sorted because file order is not time order: the watcher stamps a queued
    healthy event with Docker's original time after its confirm sleep.
    The sort is stable, so same-second events keep file order.
    """
    events = []
    for raw in lines:
        parts = raw.split()
        if len(parts) < 2 or not parts[0].isdigit():
            continue
        events.append((int(parts[0]), parts[1], parts[2] if len(parts) > 2 else ""))
    events.sort(key=lambda e: e[0])
    return events


def summarize(lines, now, window_s=WINDOW_S, current_health=None):
    """Outage statistics for the window ending at `now`.

    An outage opens at an `unhealthy` event and closes at the next `healthy`.
    Repeated `unhealthy` inside an open outage (a watcher restart re-checking
    the same outage) is the same outage. An outage still open at the end is
    "ongoing" and runs to `now` — unless the caller knows gluetun is healthy
    right now (`current_health="healthy"`): then its recovery simply went
    unrecorded, and it is closed at the last event seen after it began.
    """
    events = parse(lines)
    outages = []          # [start, end, cause]
    open_ = None
    last_seen = None
    restarts = []
    for ts, kind, detail in events:
        if ts > now:
            continue      # clock stepped backwards since it was written
        last_seen = ts
        if kind == "unhealthy":
            if open_ is None:
                open_ = [ts, None, detail or "unknown"]
        elif kind == "healthy":
            if open_ is not None:
                open_[1] = ts
                outages.append(open_)
                open_ = None
        elif kind == "restart":
            restarts.append(ts)
    ongoing = False
    if open_ is not None:
        if current_health == "healthy":
            open_[1] = max(open_[0], last_seen or open_[0])
        else:
            open_[1] = now
            ongoing = True
        outages.append(open_)

    start = now - window_s
    recent = [o for o in outages if o[1] >= start]
    def clipped(o):
        return max(0, min(o[1], now) - max(o[0], start))
    drops = [o for o in recent if o[0] >= start]
    first_ts = events[0][0] if events else None
    return {
        "has_history": bool(events),
        "recording_since": first_ts,
        "window_s": window_s,
        "drops": len(drops),
        "drops_portfwd": sum(1 for o in drops if o[2] == "portfwd"),
        "drops_tunnel": sum(1 for o in drops if o[2] == "tunnel"),
        "down_s": sum(clipped(o) for o in recent),
        "longest_s": max((clipped(o) for o in recent), default=0),
        "restarts": sum(1 for t in restarts if t >= start),
        "last_drop_ts": outages[-1][0] if outages else None,
        "ongoing": ongoing,
        "ongoing_since": outages[-1][0] if ongoing else None,
        "now": now,
    }


def fmt_duration(seconds):
    """Plain-language duration: 'under 1 min', '9 min', '1 h 40 min', '2 d 3 h'."""
    s = int(seconds)
    if s < 60:
        return "under 1 min"
    m = s // 60
    if m < 60:
        return f"{m} min"
    h, m = divmod(m, 60)
    if h < 24:
        return f"{h} h {m} min" if m else f"{h} h"
    d, h = divmod(h, 24)
    return f"{d} d {h} h" if h else f"{d} d"


def _times(n):
    return "once" if n == 1 else f"{n} times"


def verdict(summary):
    """(level, message) — level is 'pass' or 'warn'. Never 'fail': a flaky
    VPN does not make a box unshippable, and the owner can act on a WARN."""
    s = summary
    if not s["has_history"]:
        return ("pass", "VPN tunnel: no history recorded yet (the tunnel watcher "
                        "starts recording after its next restart)")
    now = s["now"]
    span = s["window_s"]
    if s["recording_since"] is not None and now - s["recording_since"] < span:
        period = f"in the {fmt_duration(now - s['recording_since'])} since recording began"
    else:
        period = f"in the last {span // 3600} h"

    if s["drops"] == 0 and not s["ongoing"]:
        msg = f"VPN tunnel steady: no drops {period}"
        if s["last_drop_ts"] is not None:
            msg += f" (last drop {fmt_duration(now - s['last_drop_ts'])} ago)"
        return ("pass", msg)

    msg = (f"VPN tunnel dropped {_times(s['drops'])} {period} "
           f"(down {fmt_duration(s['down_s'])} total, "
           f"longest {fmt_duration(s['longest_s'])}")
    if s["last_drop_ts"] is not None:
        msg += f", last {fmt_duration(now - s['last_drop_ts'])} ago"
    msg += ")"
    if s["drops_portfwd"]:
        if s["drops_portfwd"] == s["drops"]:
            msg += ("; each time the tunnel itself still worked and only the "
                    "forwarded port was lost")
        else:
            msg += (f"; {s['drops_portfwd']} of those, the tunnel itself still "
                    "worked and only the forwarded port was lost")

    down_now = s["ongoing"] and now - s["ongoing_since"] >= WARN_ONGOING_S
    if down_now:
        return ("warn", f"VPN tunnel is DOWN right now (for "
                        f"{fmt_duration(now - s['ongoing_since'])}) — movie downloads "
                        f"wait until it reconnects. {msg}")
    if s["drops"] >= WARN_DROPS or s["down_s"] >= WARN_DOWN_S:
        return ("warn", f"{msg} — downloads stall while it is down; if this keeps "
                        "happening, try another VPN country (Media Browser tab, "
                        "Advanced)")
    return ("pass", msg)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--file", default=DEFAULT_FILE)
    ap.add_argument("--now", type=int, default=None)
    ap.add_argument("--current-health", default="")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)
    now = args.now if args.now is not None else int(time.time())
    try:
        with open(args.file, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except FileNotFoundError:
        lines = []
    except OSError as e:
        print(f"warn\tVPN tunnel history unreadable ({e.strerror or e}): {args.file}")
        return 0
    summary = summarize(lines, now, current_health=args.current_health or None)
    if args.json:
        print(json.dumps(summary, sort_keys=True))
        return 0
    level, message = verdict(summary)
    print(f"{level}\t{message}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
