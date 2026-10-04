#!/bin/bash
# Gluetun cascade restart — refreshes the network namespace linkage on
# the dependent containers (Radarr, Sonarr, Prowlarr, qBittorrent, Byparr)
# every time Gluetun (re)starts, and keeps those dependents alive.
#
# Why: those services use `network_mode: "service:gluetun"` so they
# share Gluetun's netns. When Gluetun restarts in isolation (manual
# `docker stop`, container OOM-kill, anything not orchestrated through
# `docker compose`), Docker reattaches them to the new netns at the
# kernel level, but the host->container DNAT rules and the dependents'
# bind sockets end up out of sync — packets arriving on the host's
# 127.0.0.1:7878 reach Radarr's container but get RST'd because the
# container's veth pair has been swapped underneath it. The host then
# can't reach Radarr/Prowlarr/qBit/Byparr at all even though they
# report "healthy" inside the netns.
#
# Strategy: subscribe to Docker's event stream filtered to mdb_gluetun
# `start` events, sleep a few seconds for Gluetun to stabilize (NAT-PMP
# lease, WireGuard handshake), then `docker compose restart` the
# dependents so they recreate their sockets against the new netns.
#
# Dependent convergence (2026-10): the watcher also brings back any
# dependent that is MISSING or NOT RUNNING — on every Gluetun `healthy`
# event and every CONVERGE_INTERVAL_S in the background — and restarts a
# *arr/Byparr container whose own healthcheck has been unhealthy for
# UNHEALTHY_CONFIRM_S. Before this, the healthy branch only logged, so a
# dependent that was removed, left "Created" by a boot-time `compose up`
# that magic-dingus-services' TimeoutStartSec killed, or wedged
# unhealthy, stayed down until someone rebooted the box. Containers the
# kiosk stopped for playback (pause marker) are never touched.
#
# Idempotent + cheap. Most boots the cascade fires exactly once (during
# the initial `docker compose up`, where the dependents would have been
# restarted anyway by the orchestrator); after that it sleeps until the
# next Gluetun start event, and the convergence pass is a handful of
# `docker inspect` calls every five minutes.
#
# Run via: systemd unit 'gluetun-cascade-restart.service'.
# Sourceable for tests (tests/local/cascade_converge_decision.bats).

# All ${VAR:-default} so the test harness (scripts/tests/
# test_gluetun_cascade.py) can point COMPOSE_DIR at a fixture and zero
# the sleeps; production (systemd unit, no env) gets the defaults.
COMPOSE_DIR="${COMPOSE_DIR:-/opt/magic_dingus_box/services}"
DEPENDENTS=(radarr sonarr prowlarr qbittorrent byparr)
STABILIZE_SLEEP="${STABILIZE_SLEEP:-5}"      # seconds after Gluetun start before cascading dependents
UNHEALTHY_CONFIRM_S="${UNHEALTHY_CONFIRM_S:-300}"  # seconds to wait before declaring an unhealthy event a real failure
CONVERGE_INTERVAL_S="${CONVERGE_INTERVAL_S:-300}"  # periodic dependent convergence; 0 disables the background loop
CASCADE_STATE_DIR="${CASCADE_STATE_DIR:-/run/mdb-cascade}"  # unhealthy-since stamps (tmpfs: a reboot restarts the clock)

# Playback pause awareness. playback_services_pause.sh stops the
# RAM-heavy dependents during games/movies and maintains this marker for
# the duration (see that script; admin.py PLAYBACK_PAUSE_MARKER reads the
# same path). Pre-fix, this watcher's cascade brought them back UP
# mid-game whenever Gluetun restarted/flapped during play — observed live
# 2026-07-31 (Super Mario 64 running with the full stack Up), defeating
# the pause on the 2 GB boxes. While the marker exists, the cascade
# re-links ONLY qbittorrent (it stays up during playback and needs the
# re-link for active downloads); the paused ones get re-linked when the
# kiosk's unpause brings them back. The service/container name pairs must
# stay in sync with CONTAINERS in playback_services_pause.sh.
PAUSE_MARKER=/tmp/mdb_playback_services_paused
PAUSED_CONTAINERS=(mdb_radarr mdb_sonarr mdb_prowlarr mdb_byparr)

# ---------------------------------------------------------------------------
# Shared compose lock — one actor at a time starts/stops/recreates the
# stack's containers. The same lock (same path, fd 9) is taken by
# playback_services_pause.sh, storage_attach.sh, migrate_hardlink_layout.sh
# and clear_radarr_cooldowns.py; before it, e.g. the kiosk's playback pause
# could stop Radarr in the middle of this watcher's `compose up -d`, or a
# storage re-link could race a cascade restart. Every wait is BOUNDED: on
# timeout the caller logs and proceeds without the lock (the pre-lock
# behavior) or, for a periodic pass, skips the round — never a deadlock.
# Returns 0 = held, 1 = timed out, 2 = no flock / lock file unusable.
MDB_COMPOSE_LOCK="${MDB_COMPOSE_LOCK:-/run/lock/mdb-compose.lock}"
compose_lock() {
    command -v flock >/dev/null 2>&1 || return 2
    if [ ! -e "$MDB_COMPOSE_LOCK" ]; then
        (umask 000; : >> "$MDB_COMPOSE_LOCK") 2>/dev/null
    fi
    [ -r "$MDB_COMPOSE_LOCK" ] || return 2
    exec 9<"$MDB_COMPOSE_LOCK" || return 2
    flock -w "$1" 9 || return 1
}
compose_unlock() { flock -u 9 2>/dev/null; exec 9<&-; }

log() { echo "[gluetun-cascade] $*"; }

local_compose() {
    docker compose -f "${COMPOSE_DIR}/docker-compose.yml" "$@"
}

# Pure decision for one dependent during a convergence pass.
#   $1 container state: running|restarting|paused|created|exited|dead|absent|...
#   $2 container health: healthy|unhealthy|starting|"" (no healthcheck)
#   $3 1 if the kiosk has it stopped for playback, else 0
#   $4 epoch it was first seen unhealthy ("" if not tracked)
#   $5 now (epoch)   $6 confirm window (s)
# Prints: up | restart | mark | wait | ok
#   up      — missing or not running: `compose up -d` it
#   restart — unhealthy for at least the confirm window: docker restart
#   mark    — newly unhealthy: start the confirm clock
#   wait    — unhealthy, still inside the confirm window
#   ok      — leave it alone (and clear any confirm clock)
dependent_action() {
    local state="$1" health="$2" paused="$3" since="$4" now="$5" confirm="$6"
    if [ "$paused" = "1" ]; then
        echo ok
        return
    fi
    case "$state" in
        absent|created|exited|dead) echo up; return ;;
        running) ;;
        *) echo ok; return ;;   # restarting/paused/removing: docker or an operator owns it
    esac
    if [ "$health" != "unhealthy" ]; then
        echo ok
    elif [ -z "$since" ]; then
        echo mark
    elif [ $((now - since)) -ge "$confirm" ]; then
        echo restart
    else
        echo wait
    fi
}

# Bring back missing/stopped dependents and restart confirmed-unhealthy
# ones. Only acts while Gluetun is healthy: every dependent shares its
# netns and `depends_on: service_healthy`, so `up -d` against a broken
# tunnel would just block — the unhealthy branch below owns that case.
converge_dependents() {
    if [ ! -f "${COMPOSE_DIR}/.env" ] || [ ! -f "${COMPOSE_DIR}/docker-compose.yml" ]; then
        return 0   # unprovisioned, or the stack was reset from the Content Manager
    fi
    local g
    g=$(docker inspect mdb_gluetun --format '{{.State.Health.Status}}' 2>/dev/null || echo absent)
    if [ "$g" != "healthy" ]; then
        return 0
    fi

    local rc=0
    compose_lock 60 || rc=$?
    if [ "$rc" = "1" ]; then
        log "converge: compose lock busy for 60s — skipping this round"
        return 0
    fi

    # Marker re-checked INSIDE the lock: a playback pause that landed while
    # we waited must win.
    local paused_now=0
    [ -f "${PAUSE_MARKER}" ] && paused_now=1
    mkdir -p "${CASCADE_STATE_DIR}" 2>/dev/null

    local svc c info state health p since action now to_up=()
    now=$(date +%s)
    for svc in "${DEPENDENTS[@]}"; do
        c="mdb_${svc}"
        info=$(docker inspect "$c" --format '{{.State.Status}} {{if .State.Health}}{{.State.Health.Status}}{{end}}' 2>/dev/null) \
            || info="absent"
        read -r state health <<< "$info"
        p=0
        if [ "$paused_now" = "1" ] && [ "$svc" != "qbittorrent" ]; then p=1; fi
        since=$(cat "${CASCADE_STATE_DIR}/unhealthy_since.${svc}" 2>/dev/null || true)
        action=$(dependent_action "$state" "${health:-}" "$p" "$since" "$now" "$UNHEALTHY_CONFIRM_S")
        case "$action" in
            up)
                to_up+=("$svc")
                rm -f "${CASCADE_STATE_DIR}/unhealthy_since.${svc}"
                ;;
            mark)
                echo "$now" > "${CASCADE_STATE_DIR}/unhealthy_since.${svc}"
                log "converge: ${c} unhealthy — restarting it if still unhealthy in ${UNHEALTHY_CONFIRM_S}s"
                ;;
            restart)
                log "converge: ${c} unhealthy for $((now - since))s — restarting it"
                docker restart "$c" >/dev/null 2>&1 || log "converge: restart of ${c} FAILED"
                rm -f "${CASCADE_STATE_DIR}/unhealthy_since.${svc}"
                ;;
            wait) ;;
            *) rm -f "${CASCADE_STATE_DIR}/unhealthy_since.${svc}" ;;
        esac
    done

    if [ "${#to_up[@]}" -gt 0 ]; then
        log "converge: bringing up missing/stopped dependents: ${to_up[*]}"
        if local_compose up -d "${to_up[@]}" 2>&1; then
            log "converge: up -d complete"
        else
            log "converge: up -d failed — will retry next round"
        fi
    fi
    [ "$rc" = "0" ] && compose_unlock
    return 0
}

# docker restart mdb_gluetun under the compose lock (bounded wait).
# Clear the *arr indexer cooldowns a tunnel outage caused. While Gluetun
# is down every Radarr/Sonarr search fails, and both apps escalate the
# indexers into cooldowns of up to 24 h — which, before this, only the
# boot-time magic-dingus-clear-cooldowns oneshot cleared. A box whose
# tunnel blipped (observed 2026-10-04: unhealthy every ~10 min for hours)
# then searched nothing long after the tunnel was back. The helper is a
# no-op when nothing is in cooldown, and restarts an app only when it has
# rows to reset. Backgrounded after a settle delay (the apps must answer
# first) and single-flight, so the event loop never blocks on it.
clear_cooldowns_after_recovery() {
    local cmd="${COOLDOWN_CLEAR_CMD:-/usr/local/bin/clear_radarr_cooldowns.py}"
    [ -x "$cmd" ] || return 0
    local lock="${COOLDOWN_CLEAR_LOCK:-/run/lock/mdb-cooldown-clear.lock}"
    _run_clear() {
        sleep "${COOLDOWN_CLEAR_DELAY_S:-60}"
        if command -v flock >/dev/null 2>&1; then
            flock -n "$lock" "$cmd" || log "cooldown clear skipped (another run in progress or failed)"
        else
            "$cmd" || true
        fi
    }
    if [ "${COOLDOWN_CLEAR_SYNC:-0}" = "1" ]; then
        _run_clear
    else
        _run_clear &
    fi
}

restart_gluetun() {
    local rc=0
    compose_lock 120 || rc=$?
    [ "$rc" = "1" ] && log "compose lock busy for 120s — restarting gluetun anyway"
    if docker restart mdb_gluetun; then
        log "gluetun restart issued$1; cascade will follow start event"
    else
        log "gluetun restart FAILED$1 — manual intervention required"
    fi
    [ "$rc" = "0" ] && compose_unlock
    return 0
}

cascade_dependents() {
    local rc=0
    compose_lock 120 || rc=$?
    [ "$rc" = "1" ] && log "compose lock busy for 120s — cascading anyway"
    # Marker checked AFTER the stabilize sleep and INSIDE the lock — i.e.
    # at action time, not event time — so a pause/unpause landing during
    # the sleep or the lock wait is honored. targets is re-derived per
    # event.
    local targets=("${DEPENDENTS[@]}")
    if [ -f "${PAUSE_MARKER}" ]; then
        targets=(qbittorrent)
        # Enforcement: converge any paused container a prior race
        # revived back to the kiosk's intent. No-op when they're
        # already stopped. Same 2 s timeout as the pause script.
        # Stale-marker risk is bounded: a kiosk crash mid-playback
        # restarts the kiosk (systemd), whose startup safety runs
        # unpause and clears the marker within seconds.
        log "playback pause marker present — re-linking qbittorrent only, enforcing stop of ${PAUSED_CONTAINERS[*]}"
        docker stop -t 2 "${PAUSED_CONTAINERS[@]}" >/dev/null 2>&1 || true
    fi
    log "cascading dependents: ${targets[*]}"
    # Two recovery paths needed depending on Gluetun's prior state:
    #
    #   - Gluetun was RECREATED (config change → new container):
    #     dependents sharing its netns crash with exit 128 the
    #     instant the old gluetun container is destroyed.
    #     They're "stopped/dead" containers.  → need `up -d`
    #
    #   - Gluetun just RESTARTED (same container, new netns):
    #     dependents stay RUNNING but the host-to-container port
    #     DNAT rules get torn down with the old netns. They're
    #     "running but unreachable from host."  → need `restart`
    #
    # `up -d` alone is a no-op for the second case (containers
    # already running, no config change). `restart` alone fails
    # the first case (can't restart a dead container that needs
    # creating). Run BOTH, restart first to handle the running
    # case, then `up -d` as a safety net to ensure anything
    # that ended up stopped during/after the restart gets back up.
    # Both are idempotent.
    if local_compose restart "${targets[@]}" 2>&1; then
        log "dependents restarted"
    else
        # restart can fail if some are dead — fall through to up -d
        log "(some dependents not running; up -d will create them)"
    fi
    if local_compose up -d "${targets[@]}"; then
        log "cascade complete"
    else
        log "cascade up -d failed — will retry on next event"
    fi
    [ "$rc" = "0" ] && compose_unlock
    return 0
}

main() {
    if ! [ -f "${COMPOSE_DIR}/docker-compose.yml" ]; then
        log "no compose file at ${COMPOSE_DIR} — exiting (services not provisioned)"
        exit 0
    fi

    log "watching docker events for mdb_gluetun start + health_status..."

    # Blind-start guard: `docker events` reports TRANSITIONS only, so a
    # gluetun that is ALREADY unhealthy when this watcher starts emits no
    # event and sits broken forever. Hit live 2026-08-12: boot-time
    # `compose up` marked gluetun unhealthy (NAT-PMP lease lost right after
    # acquisition — the port-forward ratchet) moments BEFORE this unit
    # started, the transition fired into the void, and no restart ever came
    # (gluetun sat unhealthy 30+ min until manual intervention). Check the
    # CURRENT state once at startup — in the background, because a
    # synchronous 5-minute confirm before the `docker events` subscription
    # below would MISS events outright, not merely delay them. Same
    # confirm-then-restart contract as the health_status:unhealthy branch:
    # the restart fires a fresh `start` event that the main loop's cascade
    # branch picks up.
    (
        startup_state=$(docker inspect mdb_gluetun \
            --format '{{.State.Health.Status}}' 2>/dev/null || echo absent)
        if [ "${startup_state}" = "unhealthy" ]; then
            log "gluetun ALREADY unhealthy at watcher start, waiting ${UNHEALTHY_CONFIRM_S}s to confirm..."
            sleep "${UNHEALTHY_CONFIRM_S}"
            current=$(docker inspect mdb_gluetun \
                --format '{{.State.Health.Status}}' 2>/dev/null || echo unknown)
            if [ "${current}" = "unhealthy" ]; then
                log "still unhealthy after ${UNHEALTHY_CONFIRM_S}s (startup check), restarting tunnel..."
                restart_gluetun " (startup check)"
            else
                log "recovered to '${current}' during startup confirm — no action needed"
            fi
        fi
    ) &

    # Periodic convergence. The event stream alone cannot see a dependent
    # that was removed or never started (no gluetun transition happens),
    # nor a *arr that wedged on its own. set +e: one failed docker call
    # must not end the loop for the rest of the watcher's life.
    if [ "${CONVERGE_INTERVAL_S}" -gt 0 ] 2>/dev/null; then
        (
            set +e
            while sleep "${CONVERGE_INTERVAL_S}"; do
                converge_dependents
            done
        ) &
    fi

    # Subscribe to two event types from the same stream:
    #   - start:         cascade-restart dependents so they re-bind to the
    #                    new netns. Triggered whenever Gluetun (re)starts.
    #   - health_status: triggered when Gluetun's healthcheck transitions.
    #                    "unhealthy" — restart Gluetun, which then fires a
    #                    fresh `start` event that the start branch picks up
    #                    to cascade dependents. This is the auto-recovery
    #                    path for the DNS-wedge scenario where the tunnel
    #                    stays "up" but DNS stops working (observed in
    #                    production on 2026-05-26, required a manual Pi
    #                    reboot pre-fix). "healthy" — converge dependents.
    docker events --filter container=mdb_gluetun \
                  --filter event=start \
                  --filter event=health_status \
                  --format '{{.Time}} {{.Action}}' | \
    while IFS= read -r line; do
        # Docker formats health-status actions as `health_status: healthy`
        # (with a space) when printed via {{.Action}}, but raw start events
        # are just `start`. Earlier versions of this script naively split on
        # whitespace via `read -r event_time event_action`, which clipped
        # the action at the space and made every health_status event miss
        # the case-statement matches below — silently disabling the entire
        # auto-restart-on-unhealthy feature.
        #
        # Parse: timestamp is field 1; action is everything after the first
        # space, then normalize "health_status: X" → "health_status:X" so the
        # case patterns can match without depending on whitespace.
        event_time="${line%% *}"
        event_action="${line#* }"
        event_action="${event_action// /}"   # collapse internal spaces
        case "${event_action}" in
            start)
                log "gluetun started at ${event_time}, sleeping ${STABILIZE_SLEEP}s before cascading..."
                sleep "${STABILIZE_SLEEP}"
                cascade_dependents
                ;;
            health_status:unhealthy)
                # Docker emits unhealthy after retries-many consecutive
                # failures (compose retries=5, interval=60s = ~5 minutes
                # confirmed-broken). But the healthcheck *can* still flap
                # on transient network blips — observed live: a single
                # DNS hiccup causes wget to fail, the next checks fail
                # too (TCP timeout for instance), unhealthy fires, then
                # the next check succeeds and the container recovers
                # without intervention.
                #
                # Restarting gluetun on every blip would be worse than the
                # blip itself: the cascade brings down dependents (Radarr
                # loses queue tracking, qBit re-handshakes peers, downloads
                # slow). Add a 5-minute confirmation wait before declaring
                # a real failure. If still unhealthy after the wait, the
                # tunnel is genuinely stuck and a restart is warranted.
                #
                # Bash `while read` is serial — events that arrive during
                # the sleep queue up and process after. The post-sleep
                # check looks at the *current* state, not the queued
                # events, so a recovery during the wait is handled cleanly.
                log "gluetun went UNHEALTHY at ${event_time}, waiting ${UNHEALTHY_CONFIRM_S}s to confirm..."
                sleep "${UNHEALTHY_CONFIRM_S}"
                current=$(docker inspect mdb_gluetun --format '{{.State.Health.Status}}' 2>/dev/null || echo unknown)
                if [ "${current}" = "unhealthy" ]; then
                    log "still unhealthy after ${UNHEALTHY_CONFIRM_S}s, restarting tunnel..."
                    restart_gluetun ""
                else
                    log "recovered to '${current}' during wait — no action needed"
                fi
                ;;
            health_status:healthy)
                # Dependents normally kept running through the unhealthy
                # window (they share the netns; docker doesn't kill them on
                # parent healthcheck failure) — but a boot whose `compose
                # up` was cut short, or a dependent removed while the
                # tunnel was down, is only noticed here.
                log "gluetun healthy at ${event_time} — converging dependents"
                converge_dependents || true
                clear_cooldowns_after_recovery || true
                ;;
            *)
                # Anything else slipping past the filter — log but ignore.
                # Lets us spot future Docker behavior changes in the journal
                # without ever acting on an unexpected event type.
                log "(ignored) ${event_action} at ${event_time}"
                ;;
        esac
    done
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    set -euo pipefail
    main "$@"
fi
