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

# Authenticate with qBit FIRST — before the Gluetun port query — so the
# drive-absent guard below runs on every tick that qBit is reachable at
# all. It originally sat at the END of this script, which put it behind
# five early `exit 0`s including the steady-state "in sync" path: the one
# minute-by-minute case the guard exists for (drive yanked mid-seeding,
# port long since synced) was exactly the case that never reached it.
# Cookie file is per-run so we never carry stale session state between
# invocations.
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

# ---------------------------------------------------------------------------
# Drive-absent download guard (runs every 60 s with the timer).
# ---------------------------------------------------------------------------
# If the MOVIES drive is unplugged, /mnt/ssd silently degrades to a plain
# directory on the SD card — and active torrents then write to the OS
# partition until it is FULL, which kills the box. Pause everything while
# the mount is absent; resume automatically when it returns. The marker
# records that WE paused (an operator's own manual pauses are never
# resumed by this guard). Runs here because this script already holds an
# authenticated qBit session on a 60 s cadence — and it runs BEFORE the
# Gluetun port fetch on purpose, so a lapsed NAT-PMP lease (its own
# early exit) can never postpone the pause.
qbit_torrents_op() {
    # $1 = modern endpoint, $2 = legacy endpoint. The pinned image is
    # qBittorrent 5.0.3, whose WebAPI (2.11) RENAMED pause/resume to
    # stop/start — the old names return 404 (verified live on the box;
    # the first version of this guard called only pause/resume and
    # therefore never paused anything). Try the modern name first and
    # fall back to the legacy one so an older 4.x image still works.
    _rc=$(curl -sS -o /dev/null -w '%{http_code}' -b "${COOKIE}" -X POST \
        --data "hashes=all" "${QBIT_API}/api/v2/torrents/$1" 2>/dev/null)
    if [ "${_rc}" != "200" ]; then
        curl -sS -b "${COOKIE}" -X POST --data "hashes=all" \
            "${QBIT_API}/api/v2/torrents/$2" >/dev/null 2>&1 || true
    fi
}
GUARD_MARKER=/tmp/mdb_drive_guard_paused
if ! mountpoint -q /mnt/ssd; then
    if [ ! -f "$GUARD_MARKER" ]; then
        qbit_torrents_op stop pause
        touch "$GUARD_MARKER"
        echo "[qbit-port-sync] DRIVE GUARD: /mnt/ssd not mounted — all torrents paused"
    fi
elif [ -f "$GUARD_MARKER" ]; then
    qbit_torrents_op start resume
    rm -f "$GUARD_MARKER"
    echo "[qbit-port-sync] DRIVE GUARD: drive back — torrents resumed"
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
