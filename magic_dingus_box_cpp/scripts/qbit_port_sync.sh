#!/usr/bin/env bash
# qBittorrent listen-port sync — keep qBit's incoming port aligned
# with whatever NAT-PMP port Gluetun is currently holding from the
# VPN provider.
#
# Why this matters: every time Gluetun re-handshakes (VPN reconnect,
# container restart, exit-server rotation), ProtonVPN issues a new
# random NAT-PMP forwarded port. Until qBit's listen_port matches,
# incoming peer connections from the swarm fail at the Gluetun
# firewall: the swarm members see our advertised port, try to
# connect, hit "port not forwarded," and silently drop us from
# their peer list. Net effect: torrents stall at metaDL or
# stalledDL with 0 peers — exactly the Sling Blade case we hit
# tonight where popular older movies should easily find peers but
# couldn't, while newer high-seed torrents (Devil Wears Prada 2)
# limped along via outbound-initiated connections only.
#
# Strategy: poll Gluetun's HTTP control endpoint (via docker exec —
# port 8000 isn't published to the host), compare to qBit's current
# listen_port, and PUT the new value via qBit's web API only when
# it actually changed. Cheap (one HTTP roundtrip per check) and
# idempotent.
#
# qBit password comes from MDB_QBIT_PASS in services/.env, which is
# written by setup_services.sh Step 7.6 + re-applied by the boot-time
# magic-dingus-sync-qbit-password.service oneshot. The legacy version
# of this script had QBIT_PASS hard-coded to "adminadmin" which broke
# silently the moment Step 7.5 rotated qBit's password — observed
# tonight as JSONDecodeError tracebacks in journalctl and unsynced
# ports for the entire session.
#
# Run via: systemd timer 'qbit-port-sync.timer' (60s cadence).

set -uo pipefail

ENV_FILE="/opt/magic_dingus_box/services/.env"
GLUETUN_API='http://localhost:8000/v1/openvpn/portforwarded'
QBIT_API='http://localhost:8080'
QBIT_USER='admin'

if [ ! -f "${ENV_FILE}" ]; then
    echo "[qbit-port-sync] no .env at ${ENV_FILE}, skipping (services not provisioned)"
    exit 0
fi

# MDB_QBIT_PASS mirrors QBITTORRENT_ADMIN_PASSWORD; either should
# work, but MDB_QBIT_PASS is the canonical name used by the kiosk
# binary (env var read via systemd EnvironmentFile=) so we use that
# as the primary source of truth.
QBIT_PASS=$(grep '^MDB_QBIT_PASS=' "${ENV_FILE}" | cut -d= -f2-)
if [ -z "${QBIT_PASS}" ]; then
    QBIT_PASS=$(grep '^QBITTORRENT_ADMIN_PASSWORD=' "${ENV_FILE}" | cut -d= -f2-)
fi
if [ -z "${QBIT_PASS}" ]; then
    echo "[qbit-port-sync] no qBit password in .env, skipping"
    exit 0
fi

# Pull the forwarded port from Gluetun's control endpoint. We hit it
# via `docker exec` because Gluetun's 8000 isn't host-published —
# publishing it would expose the unprotected control API on the LAN,
# which we explicitly avoid.
PORT=$(docker exec mdb_gluetun wget -qO- "${GLUETUN_API}" 2>/dev/null \
        | python3 -c 'import sys,json; print(json.load(sys.stdin)["port"])' 2>/dev/null \
        || echo 0)

if [ -z "${PORT}" ] || [ "${PORT}" = "0" ]; then
    # Gluetun starting up, port-forwarding lease lapsed, or NAT-PMP
    # blocked. Don't clobber qBit's current port — leaving it alone
    # is preferable to setting it to 0 (which disables incoming
    # peer connectivity entirely).
    echo "[qbit-port-sync] no forwarded port from Gluetun; leaving qBit unchanged"
    exit 0
fi

# Authenticate with qBit. Cookie file is per-run so we never carry
# stale session state between invocations.
COOKIE=$(mktemp /tmp/qbit-cookie-XXXX)
trap 'rm -f "${COOKIE}"' EXIT

LOGIN_RESP=$(curl -sS -c "${COOKIE}" -X POST \
    -d "username=${QBIT_USER}" --data-urlencode "password=${QBIT_PASS}" \
    "${QBIT_API}/api/v2/auth/login" 2>&1)

if [ "${LOGIN_RESP}" != "Ok." ]; then
    echo "[qbit-port-sync] qBit login failed (response: ${LOGIN_RESP:0:40}); skipping"
    echo "                 likely qBit password drift — magic-dingus-sync-qbit-password should heal on next boot"
    exit 0
fi

# Fetch current listen_port. If qBit is mid-recreate / mid-restart
# this can return empty body; handle gracefully.
PREFS=$(curl -sS -b "${COOKIE}" "${QBIT_API}/api/v2/app/preferences" 2>/dev/null)
CURRENT=$(echo "${PREFS}" | python3 -c 'import sys,json; print(json.load(sys.stdin).get("listen_port", 0))' 2>/dev/null || echo 0)

if [ -z "${CURRENT}" ] || [ "${CURRENT}" = "0" ]; then
    echo "[qbit-port-sync] could not read qBit listen_port; skipping"
    exit 0
fi

if [ "${CURRENT}" = "${PORT}" ]; then
    echo "[qbit-port-sync] in sync (port=${PORT})"
    exit 0
fi

echo "[qbit-port-sync] port changed: qBit=${CURRENT} -> Gluetun=${PORT}, updating"
# upnp + random_port both false ensures qBit doesn't drift back to
# auto-assigning a different port on its own.
curl -sS -b "${COOKIE}" -X POST \
    --data-urlencode "json={\"listen_port\":${PORT},\"upnp\":false,\"random_port\":false}" \
    "${QBIT_API}/api/v2/app/setPreferences" >/dev/null
echo "[qbit-port-sync] qBit listen_port set to ${PORT}"
