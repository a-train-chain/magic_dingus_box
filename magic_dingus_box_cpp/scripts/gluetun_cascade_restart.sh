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

echo "[gluetun-cascade] watching docker events for mdb_gluetun start..."
docker events --filter container=mdb_gluetun --filter event=start --format '{{.Time}}' | \
while read -r event_time; do
    echo "[gluetun-cascade] gluetun started at ${event_time}, sleeping ${STABILIZE_SLEEP}s before cascading..."
    sleep "${STABILIZE_SLEEP}"
    echo "[gluetun-cascade] restarting dependents: ${DEPENDENTS[*]}"
    if docker compose -f "${COMPOSE_DIR}/docker-compose.yml" restart "${DEPENDENTS[@]}"; then
        echo "[gluetun-cascade] cascade restart complete"
    else
        echo "[gluetun-cascade] cascade restart failed (compose exit=$?), will retry on next event"
    fi
done
