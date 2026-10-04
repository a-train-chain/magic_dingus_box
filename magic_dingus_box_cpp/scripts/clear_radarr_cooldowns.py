#!/usr/bin/env python3
"""Clear stale Radarr AND Sonarr indexer cooldowns on system boot.

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

Sonarr shares Radarr's provider-status code (both are *arr forks of
the same IndexerStatus table: InitialFailure / MostRecentFailure /
EscalationLevel / DisabledTill), so TV searches lock out the same way;
it is cleared with the same statement. The schema is still checked
before touching either DB — an app whose table lacks those columns is
skipped, never written to.

What this does: at boot, wait briefly for each app to become reachable,
then null out any cooldown state. Idempotent — if a table is already
clean, that app is not touched at all (no restart).

Safety (2026-10):
  * The container restart is in try/finally: an exception between the
    stop and the start used to leave Radarr stopped until the next boot.
  * Only a container that was RUNNING is started again — one the kiosk
    stopped for playback (pause marker) stays stopped; its DB is still
    cleaned, which is safe precisely because it is stopped.
  * Stop/start run under the shared compose lock
    (/run/lock/mdb-compose.lock) that gluetun_cascade_restart.sh,
    playback_services_pause.sh and storage_attach.sh also take, so a
    cascade or convergence pass cannot start Radarr mid-write.

Run as root (the config dirs are root-owned). Designed to be invoked
from magic-dingus-clear-cooldowns.service after
magic-dingus-services.service. The filename keeps its historical name:
the unit and OTA helper refresh reference it.
"""
import fcntl
import os
import sqlite3
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

CONFIG_DIR = Path("/opt/magic_dingus_box/services/config")
PAUSE_MARKER = Path("/tmp/mdb_playback_services_paused")
COMPOSE_LOCK = Path(os.environ.get("MDB_COMPOSE_LOCK",
                                   "/run/lock/mdb-compose.lock"))

# (label, container, db path, ping url)
APPS = [
    ("Radarr", "mdb_radarr", CONFIG_DIR / "radarr" / "radarr.db",
     "http://localhost:7878/ping"),
    ("Sonarr", "mdb_sonarr", CONFIG_DIR / "sonarr" / "sonarr.db",
     "http://localhost:8989/ping"),
]

# Wait window: an app usually responds within 30s of stack boot, but
# the cascade-restart watcher may have just bounced it. ONE shared
# deadline across both apps, and ONE lock acquisition for both, so the
# unit's TimeoutSec=180 (installed units are never replaced by OTA)
# still covers wait (90) + lock (30) + two stop/clear/start cycles.
READY_TIMEOUT_S = 90
POLL_INTERVAL_S = 5
LOCK_WAIT_S = 30

COOLDOWN_COLUMNS = {"DisabledTill", "MostRecentFailure", "InitialFailure",
                    "EscalationLevel"}


def log(msg: str) -> None:
    print(f"[clear_radarr_cooldowns] {msg}", flush=True)


def app_ready(url: str) -> bool:
    """True iff the app's /ping returns 200."""
    try:
        with urllib.request.urlopen(url, timeout=3) as r:
            return r.status == 200
    except (urllib.error.URLError, TimeoutError, ConnectionError, OSError):
        return False


def wait_for_app(url: str, deadline: float) -> bool:
    """Poll the app until it answers or the shared deadline passes."""
    while True:
        if app_ready(url):
            return True
        if time.time() >= deadline:
            return False
        time.sleep(POLL_INTERVAL_S)


def has_cooldown_schema(con: sqlite3.Connection) -> bool:
    cols = {row[1] for row in
            con.execute("PRAGMA table_info(IndexerStatus);").fetchall()}
    return COOLDOWN_COLUMNS <= cols


def count_active_cooldowns(db: Path) -> int:
    """Read-only count of indexers carrying cooldown state.

    Safe while the app runs — SQLite WAL mode allows concurrent readers.
    Returns 0 for a missing DB or an unrecognized schema (never touched).
    """
    if not db.exists():
        return 0
    con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    try:
        if not has_cooldown_schema(con):
            log(f"{db.name}: IndexerStatus lacks the cooldown columns — skipping")
            return 0
        return con.execute(
            "SELECT COUNT(*) FROM IndexerStatus "
            "WHERE DisabledTill IS NOT NULL OR EscalationLevel > 0;"
        ).fetchone()[0]
    finally:
        con.close()


def clear_cooldowns(db: Path) -> int:
    """Null out cooldown timestamps + reset escalation. Returns rows updated."""
    if not db.exists():
        log(f"{db} not found; nothing to clear")
        return 0
    con = sqlite3.connect(db)
    try:
        if not has_cooldown_schema(con):
            return 0
        cur = con.cursor()
        # DisabledTill = the future timestamp we don't want to honor.
        # MostRecentFailure / InitialFailure / EscalationLevel together
        # drive the exponential backoff curve; clearing them means the
        # next failure starts the escalation fresh (a short cooldown)
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


def container_running(name: str) -> bool:
    try:
        out = subprocess.run(
            ["docker", "inspect", "-f", "{{.State.Running}}", name],
            capture_output=True, text=True, timeout=30, check=False)
    except (OSError, subprocess.SubprocessError):
        return False
    return out.stdout.strip() == "true"


def docker(action: str, name: str) -> None:
    try:
        subprocess.run(["docker", action, name], check=False, timeout=60,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.SubprocessError) as e:
        log(f"docker {action} {name} failed: {e}")


class ComposeLock:
    """Bounded flock on the shared compose lock; proceeds without on timeout."""

    def __init__(self, path: Path, wait_s: float):
        self.path, self.wait_s, self.fd = path, wait_s, None

    def __enter__(self):
        try:
            old = os.umask(0)
            try:
                self.fd = os.open(self.path, os.O_RDONLY | os.O_CREAT, 0o666)
            finally:
                os.umask(old)
        except OSError:
            return self
        deadline = time.time() + self.wait_s
        while True:
            try:
                fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return self
            except OSError:
                if time.time() >= deadline:
                    log(f"compose lock busy for {self.wait_s}s — proceeding without it")
                    os.close(self.fd)
                    self.fd = None
                    return self
                time.sleep(0.5)

    def __exit__(self, *exc):
        if self.fd is not None:
            os.close(self.fd)   # closing releases the flock
            self.fd = None
        return False


def clear_app(label: str, container: str, db: Path) -> int:
    """Stop (if running) → clear → start (only if it was running).

    The start is in a finally: an exception anywhere between stop and
    start must never strand the container stopped until the next boot.
    Caller holds the compose lock. Running-state is read here, inside the
    lock, so a playback pause that landed while we waited is honored.
    """
    was_running = container_running(container)
    if not was_running:
        log(f"{label} is not running (stopped for playback?) — clearing its DB without starting it")
    if was_running:
        log(f"stopping {label} to safely modify its DB...")
        docker("stop", container)
        # Brief settle to let any in-flight writes flush.
        time.sleep(2)
    try:
        n = clear_cooldowns(db)
        log(f"{label}: {n} IndexerStatus row(s) reset.")
        return n
    finally:
        if was_running:
            log(f"starting {label}...")
            docker("start", container)


def main() -> int:
    # Fast-path: read-only peek BEFORE waiting for or stopping anything.
    # If the cooldown tables are already clean (the common case after the
    # DOT=off fix landed and indexer failures became rare), exit
    # immediately. Pre-fix this script restarted Radarr on every boot
    # even with zero rows to reset, costing ~60s of API downtime for
    # nothing.
    pending = []
    for label, container, db, url in APPS:
        try:
            n = count_active_cooldowns(db)
        except sqlite3.Error as e:
            log(f"{label}: could not read {db} ({e}) — skipping")
            continue
        if n:
            log(f"{label}: {n} cooldown row(s) need clearing")
            pending.append((label, container, db, url))
    if not pending:
        log("no active cooldowns to clear — exiting (nothing touched)")
        return 0

    deadline = time.time() + READY_TIMEOUT_S
    ready = []
    for label, container, db, url in pending:
        if PAUSE_MARKER.exists() and not container_running(container):
            # Stopped for playback: it cannot answer /ping, and its DB is
            # quiescent — clear without waiting.
            ready.append((label, container, db))
        elif wait_for_app(url, deadline):
            ready.append((label, container, db))
        else:
            log(f"{label} did not respond within {READY_TIMEOUT_S}s; "
                "skipping its cooldown clear (best-effort)")
    with ComposeLock(COMPOSE_LOCK, LOCK_WAIT_S):
        for label, container, db in ready:
            try:
                clear_app(label, container, db)
            except Exception as e:  # noqa: BLE001 — best-effort, never block boot
                log(f"{label}: cooldown clear failed ({e}); container state restored")
    log("done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
