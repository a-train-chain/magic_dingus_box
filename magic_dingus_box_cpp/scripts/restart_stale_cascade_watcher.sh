#!/bin/bash
#
# Magic Dingus Box — restart the Gluetun cascade watcher when it is running
# an older copy of its script than the one installed.
#
# gluetun-cascade-restart.service is a long-running bash loop that executes
# /usr/local/bin/gluetun_cascade_restart.sh. Replacing that file (install(1)
# writes a new inode) does NOT change what the running process executes —
# it keeps the old code until the unit restarts.
#
# Why this exists: the OTA that delivers a new watcher is executed by the
# box's OLD update.sh (a field box updates with the update.sh it already
# has). v1.9.14's refresh_out_of_tree_files copies the new script into
# /usr/local/bin but never restarts the unit; the NEXT update's
# refresh_out_of_tree_files then finds identical files and skips its own
# restart too — so the new watcher would not run until the box rebooted.
# setup_memory_tuning.sh is the one root-run hook every update.sh version
# executes from the NEW tree, so it calls this.
#
# Rule: restart iff the installed script is at least as new as the running
# process (file mtime >= the unit's ExecMainStartTimestamp). After the
# restart the process is newer than the file, so a re-run is a no-op.
# Equal seconds count as stale: one spare restart beats a missed one.
#
# No-op (exit 0) when the script is not installed or the unit is not
# active — a games-only box has neither. Never fails: every error path
# logs and exits 0, because the caller must never fail an OTA over this.
# `--no-block`: never wait on the stop/start job (this also runs from
# first_boot.sh, during boot, where waiting on another job can deadlock).
#
# Test seams:
#   MAGIC_CASCADE_WATCHER_BIN - installed script path
#                               (default /usr/local/bin/gluetun_cascade_restart.sh)
#   systemctl / stat / date are resolved through PATH (BATS stubs).

set -uo pipefail

UNIT="gluetun-cascade-restart.service"
WATCHER="${MAGIC_CASCADE_WATCHER_BIN:-/usr/local/bin/gluetun_cascade_restart.sh}"

log() { echo "[cascade-watcher] $1"; }

if [[ ! -f "$WATCHER" ]]; then
    log "watcher not installed; nothing to do"
    exit 0
fi

if [[ "$(systemctl is-active "$UNIT" 2>/dev/null)" != "active" ]]; then
    log "${UNIT} not running; nothing to do"
    exit 0
fi

# When did the running process start? `--timestamp=unix` (systemd >= 248;
# Trixie ships 257) prints "@<epoch>"; older systemd falls back to parsing
# the human-readable form with GNU date.
started="$(systemctl show --timestamp=unix -p ExecMainStartTimestamp --value "$UNIT" 2>/dev/null)" || started=""
started="${started#@}"
if [[ ! "$started" =~ ^[0-9]+$ ]]; then
    human="$(systemctl show -p ExecMainStartTimestamp --value "$UNIT" 2>/dev/null)" || human=""
    started="$(date -d "$human" +%s 2>/dev/null)" || started=""
fi
if [[ ! "$started" =~ ^[0-9]+$ ]]; then
    log "WARNING: could not read ${UNIT}'s start time; leaving it running"
    exit 0
fi

# GNU stat first (the Pi), BSD stat as the fallback (a Mac test run).
mtime="$(stat -c %Y "$WATCHER" 2>/dev/null || stat -f %m "$WATCHER" 2>/dev/null)" || mtime=""
if [[ ! "$mtime" =~ ^[0-9]+$ ]]; then
    log "WARNING: could not read ${WATCHER}'s mtime; leaving ${UNIT} running"
    exit 0
fi

if (( mtime < started )); then
    log "${UNIT} already runs the installed script"
    exit 0
fi

log "${WATCHER} is newer than the running watcher; restarting ${UNIT}"
systemctl try-restart --no-block "$UNIT" 2>/dev/null \
    || log "WARNING: restart failed (the new watcher applies on the next restart)"
exit 0
