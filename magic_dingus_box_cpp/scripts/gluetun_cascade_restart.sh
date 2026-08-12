#!/bin/bash
# Gluetun cascade restart — refreshes the network namespace linkage on
# the four dependent containers (Radarr, Prowlarr, qBittorrent, Byparr)
# every time Gluetun (re)starts.
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
# lease, WireGuard handshake), then `docker compose restart` the four
# dependents so they recreate their sockets against the new netns.
#
# Idempotent + cheap. Most boots this fires exactly once (during the
# initial `docker compose up`, where the dependents would have been
# restarted anyway by the orchestrator); after that it sleeps until the
# next Gluetun start event.
#
# Run via: systemd unit 'gluetun-cascade-restart.service'.

set -euo pipefail

# All ${VAR:-default} so the test harness (scripts/tests/
# test_gluetun_cascade.py) can point COMPOSE_DIR at a fixture and zero
# the sleeps; production (systemd unit, no env) gets the defaults.
COMPOSE_DIR="${COMPOSE_DIR:-/opt/magic_dingus_box/services}"
DEPENDENTS=(radarr sonarr prowlarr qbittorrent byparr)
STABILIZE_SLEEP="${STABILIZE_SLEEP:-5}"      # seconds after Gluetun start before cascading dependents
UNHEALTHY_CONFIRM_S="${UNHEALTHY_CONFIRM_S:-300}"  # seconds to wait before declaring an unhealthy event a real failure

# Playback pause awareness. playback_services_pause.sh stops the three
# RAM-heavy dependents during games/movies and maintains this marker for
# the duration (see that script; admin.py PLAYBACK_PAUSE_MARKER reads the
# same path). Pre-fix, this watcher's cascade brought them back UP
# mid-game whenever Gluetun restarted/flapped during play — observed live
# 2026-07-31 (Super Mario 64 running with the full stack Up), defeating
# the pause on the 2 GB boxes. While the marker exists, the cascade
# re-links ONLY qbittorrent (it stays up during playback and needs the
# re-link for active downloads); the paused three get re-linked when the
# kiosk's unpause brings them back. The service/container name pairs must
# stay in sync with CONTAINERS in playback_services_pause.sh.
PAUSE_MARKER=/tmp/mdb_playback_services_paused
PAUSED_CONTAINERS=(mdb_radarr mdb_sonarr mdb_prowlarr mdb_byparr)

if ! [ -f "${COMPOSE_DIR}/docker-compose.yml" ]; then
    echo "[gluetun-cascade] no compose file at ${COMPOSE_DIR} — exiting (services not provisioned)"
    exit 0
fi

echo "[gluetun-cascade] watching docker events for mdb_gluetun start + health_status..."

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
        echo "[gluetun-cascade] gluetun ALREADY unhealthy at watcher start, waiting ${UNHEALTHY_CONFIRM_S}s to confirm..."
        sleep "${UNHEALTHY_CONFIRM_S}"
        current=$(docker inspect mdb_gluetun \
            --format '{{.State.Health.Status}}' 2>/dev/null || echo unknown)
        if [ "${current}" = "unhealthy" ]; then
            echo "[gluetun-cascade] still unhealthy after ${UNHEALTHY_CONFIRM_S}s (startup check), restarting tunnel..."
            if docker restart mdb_gluetun; then
                echo "[gluetun-cascade] gluetun restart issued (startup check); cascade will follow start event"
            else
                echo "[gluetun-cascade] gluetun restart FAILED (startup check) — manual intervention required"
            fi
        else
            echo "[gluetun-cascade] recovered to '${current}' during startup confirm — no action needed"
        fi
    fi
) &
# Subscribe to two event types from the same stream:
#   - start:         existing behavior — cascade-restart dependents so
#                    they re-bind to the new netns. Triggered whenever
#                    Gluetun (re)starts.
#   - health_status: triggered when Gluetun's healthcheck transitions.
#                    We act on "unhealthy" — restart Gluetun, which
#                    then fires a fresh `start` event that the start
#                    branch picks up to cascade dependents. This is
#                    the auto-recovery path for the DNS-wedge scenario
#                    where the tunnel stays "up" but DNS stops working
#                    (observed in production on 2026-05-26, required
#                    a manual Pi reboot pre-fix).
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
            echo "[gluetun-cascade] gluetun started at ${event_time}, sleeping ${STABILIZE_SLEEP}s before cascading..."
            sleep "${STABILIZE_SLEEP}"
            # Marker checked AFTER the stabilize sleep — i.e. at action
            # time, not event time — so a pause/unpause landing during
            # the sleep is honored. targets is re-derived per event.
            targets=("${DEPENDENTS[@]}")
            if [ -f "${PAUSE_MARKER}" ]; then
                targets=(qbittorrent)
                # Enforcement: converge any paused container a prior race
                # revived back to the kiosk's intent. No-op when they're
                # already stopped. Same 2 s timeout as the pause script.
                # Stale-marker risk is bounded: a kiosk crash mid-playback
                # restarts the kiosk (systemd), whose startup safety runs
                # unpause and clears the marker within seconds.
                echo "[gluetun-cascade] playback pause marker present — re-linking qbittorrent only, enforcing stop of ${PAUSED_CONTAINERS[*]}"
                docker stop -t 2 "${PAUSED_CONTAINERS[@]}" >/dev/null 2>&1 || true
            fi
            echo "[gluetun-cascade] cascading dependents: ${targets[*]}"
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
            local_compose() {
                docker compose -f "${COMPOSE_DIR}/docker-compose.yml" "$@"
            }
            if local_compose restart "${targets[@]}" 2>&1; then
                echo "[gluetun-cascade] dependents restarted"
            else
                # restart can fail if some are dead — fall through to up -d
                echo "[gluetun-cascade] (some dependents not running; up -d will create them)"
            fi
            if local_compose up -d "${targets[@]}"; then
                echo "[gluetun-cascade] cascade complete"
            else
                echo "[gluetun-cascade] cascade up -d failed — will retry on next event"
            fi
            ;;
        health_status:unhealthy)
            # Docker emits unhealthy after retries-many consecutive
            # failures (compose retries=3, interval=60s = ~3 minutes
            # confirmed-broken). But the healthcheck *can* still flap
            # on transient network blips — observed live: a single
            # DNS hiccup causes wget to fail, the next 2 checks fail
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
            echo "[gluetun-cascade] gluetun went UNHEALTHY at ${event_time}, waiting ${UNHEALTHY_CONFIRM_S}s to confirm..."
            sleep "${UNHEALTHY_CONFIRM_S}"
            current=$(docker inspect mdb_gluetun --format '{{.State.Health.Status}}' 2>/dev/null || echo unknown)
            if [ "${current}" = "unhealthy" ]; then
                echo "[gluetun-cascade] still unhealthy after ${UNHEALTHY_CONFIRM_S}s, restarting tunnel..."
                if docker restart mdb_gluetun; then
                    echo "[gluetun-cascade] gluetun restart issued; cascade will follow start event"
                else
                    echo "[gluetun-cascade] gluetun restart FAILED — manual intervention required"
                fi
            else
                echo "[gluetun-cascade] recovered to '${current}' during wait — no action needed"
            fi
            ;;
        health_status:healthy)
            # Recovery: just log, don't act. Dependents kept running
            # through the unhealthy window (they share the netns; docker
            # doesn't kill them on parent healthcheck failure).
            echo "[gluetun-cascade] gluetun healthy at ${event_time}"
            ;;
        *)
            # Anything else slipping past the filter — log but ignore.
            # Lets us spot future Docker behavior changes in the journal
            # without ever acting on an unexpected event type.
            echo "[gluetun-cascade] (ignored) ${event_action} at ${event_time}"
            ;;
    esac
done
