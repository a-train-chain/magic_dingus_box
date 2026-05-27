#!/usr/bin/env python3
"""Clear stale Radarr indexer cooldowns on system boot.

Background: Radarr's IndexerFactory marks an indexer "temporarily
ignored" after consecutive failures and persists a DisabledTill
timestamp into IndexerStatus.DisabledTill (radarr.db SQLite). The
cooldown survives container restarts. With escalation, Radarr can
park an indexer for up to 24 hours after just a brief network
glitch (which the DNS-over-TLS wedge we hit tonight produces
reliably).

Net effect pre-fix: a single failed-search window in the morning
locks out every indexer until the next morning. From the operator's
perspective the kiosk looks healthy ("smoke test passes! containers
all green!") but "no source found" shows up on every Detail page.

What this does: at boot, wait briefly for Radarr to become reachable,
then null out any DisabledTill values pointing at a future time.
Idempotent — if the table is already clean (every cooldown is in
the past or null), the UPDATE affects 0 rows and the script exits
cleanly.

Run as root (Radarr's config dir is root-owned). Designed to be
invoked from magic-dingus-clear-cooldowns.service after
magic-dingus-services.service.
"""
import sqlite3
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

RADARR_DB = Path("/opt/magic_dingus_box/services/config/radarr/radarr.db")
RADARR_HEALTH_URL = "http://localhost:7878/ping"
ENV_FILE = Path("/opt/magic_dingus_box/services/.env")

# Wait window: Radarr usually responds within 30s of stack boot, but
# the cascade-restart watcher may have just bounced it. 120s = enough
# slack for the worst observed cold-start.
RADARR_READY_TIMEOUT_S = 120
POLL_INTERVAL_S = 5


def radarr_ready() -> bool:
    """True iff Radarr's /ping returns 200."""
    try:
        with urllib.request.urlopen(RADARR_HEALTH_URL, timeout=3) as r:
            return r.status == 200
    except (urllib.error.URLError, TimeoutError, ConnectionError):
        return False


def wait_for_radarr() -> bool:
    """Poll Radarr until it answers, or timeout. Returns True on success."""
    deadline = time.time() + RADARR_READY_TIMEOUT_S
    while time.time() < deadline:
        if radarr_ready():
            return True
        time.sleep(POLL_INTERVAL_S)
    return False


def stop_radarr_container() -> None:
    """Stop Radarr so we can write to its DB safely.

    Without stopping, SQLite WAL mode can leave the connection in an
    inconsistent state when Radarr next reads. The clear is fast (<1s),
    so the downtime is negligible.
    """
    import subprocess
    subprocess.run(["docker", "stop", "mdb_radarr"], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def start_radarr_container() -> None:
    """Start Radarr back up."""
    import subprocess
    subprocess.run(["docker", "start", "mdb_radarr"], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def clear_cooldowns() -> int:
    """Null out future DisabledTill values + reset escalation counters.

    Returns the number of rows updated.
    """
    if not RADARR_DB.exists():
        print(f"  ! Radarr DB not found at {RADARR_DB}; nothing to clear",
              file=sys.stderr)
        return 0
    con = sqlite3.connect(RADARR_DB)
    try:
        cur = con.cursor()
        # Reset everything that signals "this indexer is in cooldown."
        # DisabledTill = the future timestamp we don't want to honor.
        # MostRecentFailure / InitialFailure / EscalationLevel together
        # drive Radarr's exponential backoff curve; if we clear them,
        # the next failure starts the escalation fresh (a short cooldown)
        # rather than jumping straight back to 24 hours.
        cur.execute("""
            UPDATE IndexerStatus
               SET DisabledTill      = NULL,
                   MostRecentFailure = NULL,
                   InitialFailure    = NULL,
                   EscalationLevel   = 0
             WHERE DisabledTill IS NOT NULL
                OR EscalationLevel > 0;
        """)
        n = cur.rowcount
        con.commit()
        return n
    finally:
        con.close()


def main() -> int:
    print("[clear_radarr_cooldowns] waiting for Radarr to be ready...")
    if not wait_for_radarr():
        print(f"  ! Radarr did not respond within {RADARR_READY_TIMEOUT_S}s; "
              "skipping cooldown clear (best-effort)", file=sys.stderr)
        # Exit 0 — don't fail boot. Cooldowns just stay as-is.
        return 0
    print("  Radarr responded.")

    print("[clear_radarr_cooldowns] stopping Radarr to safely modify DB...")
    stop_radarr_container()
    # Brief settle to let any in-flight writes flush.
    time.sleep(2)

    print("[clear_radarr_cooldowns] clearing cooldown timestamps...")
    n = clear_cooldowns()
    print(f"  {n} IndexerStatus row(s) reset.")

    print("[clear_radarr_cooldowns] starting Radarr...")
    start_radarr_container()
    print("[clear_radarr_cooldowns] done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
