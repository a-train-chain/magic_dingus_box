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
STABILIZE_SLEEP=5  # seconds to wait after Gluetun start before restarting dependents

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
while read -r event_time event_action; do
    case "${event_action}" in
        start)
            echo "[gluetun-cascade] gluetun started at ${event_time}, sleeping ${STABILIZE_SLEEP}s before cascading..."
            sleep "${STABILIZE_SLEEP}"
            echo "[gluetun-cascade] (re)bringing up dependents: ${DEPENDENTS[*]}"
            # Use `up -d` not `restart`. `restart` is a no-op on dead
            # containers — which is what we get when Gluetun was
            # RECREATED (config change in docker-compose.yml), not just
            # restarted: dependents sharing its netns crash with
            # exit 128 the moment Gluetun's old container is destroyed.
            # `up -d` is idempotent: starts stopped containers,
            # restarts running ones if config changed, creates them
            # if missing. Handles every recovery scenario.
            if docker compose -f "${COMPOSE_DIR}/docker-compose.yml" up -d "${DEPENDENTS[@]}"; then
                echo "[gluetun-cascade] cascade up -d complete"
            else
                echo "[gluetun-cascade] cascade up -d failed (compose exit=$?), will retry on next event"
            fi
            ;;
        health_status:unhealthy)
            # Docker emits `health_status: unhealthy` after retries-many
            # consecutive failures (per docker-compose retries=3,
            # interval=60s = ~3 minutes confirmed-broken). By the time
            # we see this the data path is genuinely down. Restart
            # gluetun; the resulting `start` event re-enters the case
            # above and cascades to dependents.
            echo "[gluetun-cascade] gluetun went UNHEALTHY at ${event_time}, restarting tunnel..."
            if docker restart mdb_gluetun; then
                echo "[gluetun-cascade] gluetun restart issued; cascade will follow start event"
            else
                echo "[gluetun-cascade] gluetun restart FAILED — manual intervention required"
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
