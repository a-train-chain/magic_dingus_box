#!/bin/bash
# Recreate ONLY the Gluetun container so a changed services/.env value (the
# VPN server country, VPN_COUNTRIES -> SERVER_COUNTRIES) takes effect.
#
# Run as root by the Content Manager's VPN-country setting
# (magic_dingus_box/web/vpn_settings.py), detached in its own systemd unit
# (detached_jobs.py) so a web-service restart cannot cut it off halfway.
#
#   1. `compose up -d --no-deps gluetun` under the shared compose lock
#      (/run/lock/mdb-compose.lock, fd 9 — the same lock every actor that
#      starts/stops/recreates the stack takes; see gluetun_cascade_restart.sh).
#      Compose sees the changed environment and recreates gluetun alone.
#      NEVER --force-recreate: it renames the old container to a
#      hash-prefixed name first, and a leftover rename makes every later
#      recreate die on "Conflict. The container name ... is already in use".
#   2. Release the lock, THEN wait. The dependents (Radarr, Sonarr, Prowlarr,
#      qBittorrent, Byparr) share gluetun's network namespace and die with
#      it; gluetun-cascade-restart.service sees the new container's `start`
#      event and re-links them — under the same lock, so holding it here
#      while waiting would only delay that.
#   3. Wait (bounded) for gluetun to report healthy and print the exit
#      country it got. Exit 0 = healthy, 1 = not healthy in time (the
#      tunnel keeps trying; the cascade watcher owns recovery from here).
#
# Test seams: COMPOSE_DIR, MDB_COMPOSE_LOCK, RECREATE_WAIT_S, POLL_S, and
# `docker` / `flock` resolved through PATH.
set -uo pipefail

COMPOSE_DIR="${COMPOSE_DIR:-/opt/magic_dingus_box/services}"
MDB_COMPOSE_LOCK="${MDB_COMPOSE_LOCK:-/run/lock/mdb-compose.lock}"
RECREATE_WAIT_S="${RECREATE_WAIT_S:-180}"
POLL_S="${POLL_S:-5}"

say() { echo "[vpn-country] $*"; }

# Same bounded lock helper as gluetun_cascade_restart.sh: 0 = held,
# 1 = timed out (proceed without it, the pre-lock behavior), 2 = unusable.
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

if [ ! -f "${COMPOSE_DIR}/docker-compose.yml" ] || [ ! -f "${COMPOSE_DIR}/.env" ]; then
    say "Media Browser services are not set up on this box — nothing to restart."
    exit 1
fi

country=$(sed -n 's/^VPN_COUNTRIES=//p' "${COMPOSE_DIR}/.env" | tail -1 | tr -d '"')
say "Reconnecting the VPN${country:+ to a server in ${country}}…"

rc=0
compose_lock 120 || rc=$?
[ "$rc" = "1" ] && say "compose lock busy for 120s — continuing anyway"
docker compose -f "${COMPOSE_DIR}/docker-compose.yml" up -d --no-deps gluetun 2>&1
up_rc=$?
[ "$rc" = "0" ] && compose_unlock
if [ "$up_rc" -ne 0 ]; then
    say "FAILED to recreate the VPN container (docker compose exit ${up_rc})."
    exit 1
fi
say "VPN container recreated; the other Movies services reconnect automatically."

# Deadline by wall clock, not by summing POLL_S (a 0 poll would never end).
deadline=$((SECONDS + RECREATE_WAIT_S))
health=""
while :; do
    health=$(docker inspect mdb_gluetun --format '{{.State.Health.Status}}' 2>/dev/null || echo absent)
    [ "$health" = "healthy" ] && break
    [ "$SECONDS" -ge "$deadline" ] && break
    sleep "$POLL_S"
done

if [ "$health" != "healthy" ]; then
    say "The VPN has not reported healthy after ${RECREATE_WAIT_S}s (state: ${health:-unknown}). It keeps retrying on its own."
    exit 1
fi

exit_country=$(docker exec mdb_gluetun wget -qO- --tries=1 --timeout=5 \
                   http://localhost:8000/v1/publicip/ip 2>/dev/null \
               | sed -n 's/.*"country":"\([^"]*\)".*/\1/p')
say "VPN connected${exit_country:+ — exit country: ${exit_country}}."
exit 0
