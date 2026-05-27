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

COMPOSE_DIR='/opt/magic_dingus_box/services'
DEPENDENTS=(radarr prowlarr qbittorrent byparr)
STABILIZE_SLEEP=5            # seconds after Gluetun start before cascading dependents
UNHEALTHY_CONFIRM_S=300      # seconds to wait before declaring an unhealthy event a real failure

if ! [ -f "${COMPOSE_DIR}/docker-compose.yml" ]; then
    echo "[gluetun-cascade] no compose file at ${COMPOSE_DIR} — exiting (services not provisioned)"
    exit 0
fi

echo "[gluetun-cascade] watching docker events for mdb_gluetun start + health_status..."
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
            echo "[gluetun-cascade] cascading dependents: ${DEPENDENTS[*]}"
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
            if local_compose restart "${DEPENDENTS[@]}" 2>&1; then
                echo "[gluetun-cascade] dependents restarted"
            else
                # restart can fail if some are dead — fall through to up -d
                echo "[gluetun-cascade] (some dependents not running; up -d will create them)"
            fi
            if local_compose up -d "${DEPENDENTS[@]}"; then
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
