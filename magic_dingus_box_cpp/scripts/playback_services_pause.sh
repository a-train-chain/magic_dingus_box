#!/bin/bash
# Stop/start the Media Browser background services during movie playback so
# the kiosk's video pipeline doesn't compete with Radarr metadata fetches,
# Prowlarr indexer syncs, or Byparr Cloudflare-challenge solving.
#
# Frees ~320 MB RAM (Radarr ~155 MB + Prowlarr ~165 MB on the Pi 4B) and
# ~6 % CPU for the duration of the movie. Re-started on leave() so the
# operator's normal browsing experience is unaffected after ~10-15 s
# cold-start.
#
# History note: this script originally used `docker pause`/`docker unpause`
# (kept the containers resident, just suspended scheduling). On the
# memory-constrained Pi 4B 2 GB variant that wasn't enough — the kiosk
# pages still got swapped out to make room for v4l2h264dec DMA buffers,
# producing the "frozen frame, then 5-second catch-up burst" symptom. We
# switched to `docker stop`/`docker start` so the RSS is actually freed
# during playback. The 10-15 s cold-start on the next Movies-tab visit
# is acceptable; the alternative is unwatchable 1080p H.264 playback.
#
# Only containers observed running and unpaused are stopped. Their names
# are journalled before the stop; unpause restores only those owned names,
# preserving services the operator had already stopped or paused.
#
# Called by PlaybackScreen::enter() / leave() via std::system(). The
# kiosk runs as `magic`; for this script to work without sudo, `magic`
# must be in the `docker` group (handled by setup_services.sh).
#
# qBit + Gluetun are intentionally NOT touched here:
#   - qBit: paused via its own HTTP API (qbit_->pause_owned()) which is
#     much faster + preserves internal state better than docker pause.
#   - Gluetun: stopping it would tear down the netns the other 4
#     containers share, breaking them entirely. Stays running.
#
# Usage (the action verbs are kept as pause/unpause so the C++ caller
# doesn't need to change; internally we now use docker stop/start):
#   playback_services_pause.sh pause     # before movie  → docker stop
#   playback_services_pause.sh unpause   # after movie   → docker start

set -u  # NOTE: no -e: a `docker stop` failure on one container should
        # NOT stop us from trying the others.

if [ "${1:-}" != "pause" ] && [ "${1:-}" != "unpause" ]; then
    echo "Usage: $0 {pause|unpause} [kiosk|ota:PID]" >&2
    exit 1
fi

ACTION="${1}"
OWNER="${2:-kiosk}"
case "$OWNER" in
    kiosk) ;;
    ota:*)
        owner_pid="${OWNER#ota:}"
        case "$owner_pid" in ''|*[!0-9]*) echo "Invalid OTA owner" >&2; exit 1 ;; esac ;;
    *) echo "Invalid pause owner" >&2; exit 1 ;;
esac
recovery_failed=0
CONTAINERS=(mdb_radarr mdb_sonarr mdb_prowlarr mdb_byparr)

# Marker read by the Content Manager (admin_media_browser.py PLAYBACK_PAUSE_MARKER) so the
# services panel can label these containers "paused for playback" instead of
# their raw docker exit codes — Radarr/Prowlarr always blow the 2 s SIGTERM
# grace below and land at "Exited (137)", which reads as a crash/OOM (caused
# a support question, 2026-07-31). /tmp is tmpfs, so a marker orphaned by a
# power cut cannot survive into the next boot; neither the kiosk's unit nor
# magic-dingus-web uses PrivateTmp, so both processes see the same file.
PAUSE_MARKER="${PAUSE_MARKER:-/tmp/mdb_playback_services_paused}"
# Durable ownership survives a kiosk restart and a reboot; the tmpfs
# visibility marker alone is never authority to start a container.
PLAYBACK_OWNED_FILE="${PLAYBACK_OWNED_FILE:-/opt/magic_dingus_box/magic_dingus_box_cpp/data/qbit_paused_by_kiosk.services}"
CLAIMS_FILE="${PLAYBACK_OWNED_FILE}.claims"

# Compose file location, needed only by the unpause fallback below.
# Env-overridable for the test harness (scripts/tests/test_gluetun_cascade.py).
COMPOSE_DIR="${COMPOSE_DIR:-/opt/magic_dingus_box/services}"

# `docker stop -t TIMEOUT` gives the container TIMEOUT seconds for a
# SIGTERM-clean shutdown before SIGKILL. We pass ALL containers in a
# single `docker stop` so they're stopped in PARALLEL — sequential stop
# of 3 containers at 5 s each can take up to 15 s, which exceeded the
# 10 s systemd watchdog on PlaybackScreen::enter() and caused the kiosk
# to be SIGABRT'd mid-playback-init. Parallel stop with a tighter 2 s
# timeout puts worst-case wall time at ~3 s — comfortable headroom.
STOP_TIMEOUT_S=2

# Shared compose lock (same path/fd as gluetun_cascade_restart.sh,
# storage_attach.sh, migrate_hardlink_layout.sh, clear_radarr_cooldowns.py):
# one actor at a time starts/stops/recreates the stack's containers, so a
# watcher's `compose up -d` can no longer revive a container this script is
# stopping, nor a storage re-link race our start. The kiosk calls this from
# a worker thread, with a bounded 20 s wait. Ownership records cannot be
# mutated without the lock: timeout or unavailability fails closed, leaving
# the visibility marker in place for a later recovery attempt.
# Returns 0 = held, 1 = timed out, 2 = no flock / lock file unusable.
MDB_COMPOSE_LOCK="${MDB_COMPOSE_LOCK:-/run/lock/mdb-compose.lock}"
LOCK_WAIT_S=20
compose_lock() {
    command -v flock >/dev/null 2>&1 || return 2
    if [ ! -e "$MDB_COMPOSE_LOCK" ]; then
        (umask 000; : >> "$MDB_COMPOSE_LOCK") 2>/dev/null
    fi
    [ -r "$MDB_COMPOSE_LOCK" ] || return 2
    exec 9<"$MDB_COMPOSE_LOCK" || return 2
    flock -w "$1" 9 || return 1
}

# Marker goes down BEFORE the lock wait on pause, so a watcher holding the
# lock (which re-checks the marker inside it) already honors this pause,
# and a Content Manager status poll racing the stop window reads "paused".
# Timestamp content is for visibility only; durable ownership is separate.
if [ "$ACTION" = "pause" ]; then
    date -u +%Y-%m-%dT%H:%M:%SZ > "$PAUSE_MARKER" 2>/dev/null || true
fi

lock_rc=0
compose_lock "$LOCK_WAIT_S" || lock_rc=$?
if [ "$lock_rc" != "0" ]; then
    echo "[playback-services] compose lock unavailable/busy — ownership unchanged" >&2
    exit 1
fi

# A kiosk claim is idempotent; the kiosk's startup unpause retires its own
# old claim. OTA runs while the kiosk is alive, so it gets a process lease
# (PID + birth time). Never release a live updater's claim at kiosk startup,
# but recover a SIGKILLed updater or a PID reused after reboot.
process_stamp() { LC_ALL=C ps -p "$1" -o lstart= 2>/dev/null | tr -d '[:space:]'; }
if [ "$OWNER" != kiosk ]; then
    stamp=$(process_stamp "$owner_pid")
    if [ -z "$stamp" ] && [ "$ACTION" = pause ]; then exit 1; fi
    OWNER="ota:${owner_pid}:${stamp}"
fi
OWNERS=()
if [ -f "$CLAIMS_FILE" ]; then
    [ -r "$CLAIMS_FILE" ] || exit 1
    while IFS= read -r claim; do
        case "$claim" in
            kiosk) ;;
            ota:*)
                rest="${claim#ota:}"; pid="${rest%%:*}"; saved="${rest#*:}"
                case "$pid" in ''|*[!0-9]*) exit 1 ;; esac
                current=$(process_stamp "$pid")
                [ -n "$current" ] && [ "$current" = "$saved" ] || continue ;;
            *) echo "[playback-services] unrecognized ownership record" >&2; exit 1 ;;
        esac
        [ "$claim" = "$OWNER" ] || OWNERS+=("$claim")
    done < "$CLAIMS_FILE"
fi
if [ "$ACTION" = pause ]; then OWNERS+=("$OWNER"); fi
if [ ${#OWNERS[@]} -gt 0 ]; then
    printf '%s\n' "${OWNERS[@]}" > "$CLAIMS_FILE.tmp" || exit 1
else
    : > "$CLAIMS_FILE.tmp" || exit 1
fi
mv "$CLAIMS_FILE.tmp" "$CLAIMS_FILE" || exit 1
if [ "$ACTION" = unpause ] && [ ${#OWNERS[@]} -gt 0 ]; then
    # Another owner still needs the pause; it owns the same original set.
    date -u +%Y-%m-%dT%H:%M:%SZ > "$PAUSE_MARKER" 2>/dev/null || true
    exit 0
fi

# Read only allowlisted names. Legacy timestamp-only markers cannot tell
# which containers the operator stopped, so recovery never adopts them.
EXISTING=()
if [ -f "$PLAYBACK_OWNED_FILE" ]; then
    [ -r "$PLAYBACK_OWNED_FILE" ] || exit 1
    while IFS= read -r c; do
        case "$c" in
            mdb_radarr|mdb_sonarr|mdb_prowlarr|mdb_byparr)
                if docker inspect "$c" >/dev/null 2>&1; then EXISTING+=("$c"); fi ;;
        esac
    done < "$PLAYBACK_OWNED_FILE"
fi

if [ "$ACTION" = "pause" ]; then
    RUNNING=()
    for c in "${CONTAINERS[@]}"; do
        state=$(docker inspect -f '{{.State.Running}} {{.State.Paused}}' "$c" 2>/dev/null)
        if [ "$state" = "true false" ]; then
            RUNNING+=("$c")
            found=false
            for owned in "${EXISTING[@]:-}"; do [ "$owned" = "$c" ] && found=true; done
            $found || EXISTING+=("$c")
        fi
    done
    if [ ${#EXISTING[@]} -gt 0 ]; then
        # Record before stop so a crash cannot strand services; inability to
        # persist ownership means no stop, never an unowned start later.
        if ! printf '%s\n' "${EXISTING[@]}" > "$PLAYBACK_OWNED_FILE.tmp" ||
           ! mv "$PLAYBACK_OWNED_FILE.tmp" "$PLAYBACK_OWNED_FILE"; then
            rm -f "$PLAYBACK_OWNED_FILE.tmp"
            exit 1
        fi
    fi
    if [ ${#RUNNING[@]} -gt 0 ]; then
        docker stop -t "$STOP_TIMEOUT_S" "${RUNNING[@]}" >/dev/null 2>&1 || true
    fi
    if [ ${#EXISTING[@]} -eq 0 ]; then rm -f "$PAUSE_MARKER"; fi
else
    if [ ${#EXISTING[@]} -eq 0 ]; then
        rm -f "$PAUSE_MARKER" "$PLAYBACK_OWNED_FILE" "$CLAIMS_FILE"
        exit 0
    fi
    # Bring containers back up. Container could be in any of three
    # states when we're called:
    #   - stopped (new path): need `docker start`
    #   - paused  (legacy path / from older sessions): need `docker unpause`
    #   - running (idempotent re-entry): both are benign no-ops
    # Try both in parallel; verify each is actually running afterwards.
    docker unpause "${EXISTING[@]}" >/dev/null 2>&1 || true
    docker start  "${EXISTING[@]}" >/dev/null 2>&1 || true
    failed=()
    for c in "${EXISTING[@]}"; do
        if [ "$(docker inspect -f '{{.State.Running}}' "$c" 2>/dev/null)" != "true" ]; then
            failed+=("$c")
        fi
    done
    if [ ${#failed[@]} -gt 0 ]; then
        # Plain `docker start` joins the netns pinned in HostConfig at
        # CREATE time: network_mode "service:gluetun" resolves to
        # Gluetun's CONTAINER ID (verified on hardware), so if Gluetun
        # was RECREATED while these were paused, start tries to join a
        # dead container and fails. Only compose re-resolves against the
        # current Gluetun — `up -d` recreates exactly the stale ones.
        # (Compose service name = container name minus the mdb_ prefix.)
        if [ -f "${COMPOSE_DIR}/docker-compose.yml" ]; then
            services=()
            for c in "${failed[@]}"; do services+=("${c#mdb_}"); done
            echo "[playback-services] plain start failed for ${failed[*]} — compose up -d fallback (Gluetun recreated mid-pause?)"
            docker compose -f "${COMPOSE_DIR}/docker-compose.yml" \
                up -d "${services[@]}" >/dev/null 2>&1 || true
        fi
        for c in "${failed[@]}"; do
            if [ "$(docker inspect -f '{{.State.Running}}' "$c" 2>/dev/null)" != "true" ]; then
                echo "[playback-services] WARN: failed to bring $c back up" >&2
            fi
        done
    fi
    # Marker comes up AFTER the start attempts — removed unconditionally:
    # if a container failed to restart, showing its raw exited state is
    # correct (a down container with no playback active IS alarming).
    # Retain failed owned services for a later recovery attempt.
    remaining=()
    for c in "${EXISTING[@]}"; do
        if docker inspect "$c" >/dev/null 2>&1 &&
           [ "$(docker inspect -f '{{.State.Running}}' "$c" 2>/dev/null)" != true ]; then
            remaining+=("$c")
        fi
    done
    if [ ${#remaining[@]} -eq 0 ]; then
        rm -f "$PLAYBACK_OWNED_FILE" "$PAUSE_MARKER" "$CLAIMS_FILE"
    else
        printf '%s\n' "${remaining[@]}" > "$PLAYBACK_OWNED_FILE.tmp" &&
            mv "$PLAYBACK_OWNED_FILE.tmp" "$PLAYBACK_OWNED_FILE"
        rm -f "$PAUSE_MARKER"
        recovery_failed=1
    fi
fi

# Tiny success log so journalctl shows when each transition fired.
# The action verb stays "pause"/"unpause" externally; internally we now
# stop/start. The log line keeps the legacy verb so existing log-grep
# tooling continues to work.
echo "[playback-services] ${ACTION}d background services"

exit "$recovery_failed"
