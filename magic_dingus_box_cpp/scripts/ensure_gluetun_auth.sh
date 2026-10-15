#!/bin/bash
#
# Magic Dingus Box — install Gluetun's control-server access file.
#
# Gluetun's control server (port 8000, inside the shared VPN netns — never
# host-published) is how this stack reads the forwarded port and the exit
# IP. Up to v3.41.x its routes are public by default and every request
# logs "route ... is unprotected by default ... will become no longer
# publicly accessible"; current gluetun makes ALL routes private unless
# /gluetun/auth/config.toml says otherwise. Without this file, the first
# gluetun bump past that change would 401 every reader at once:
#   - the compose healthcheck's port clause (GET /v1/portforward) — once
#     a port has been seen, gluetun goes unhealthy for good and the
#     cascade watcher restarts the whole stack every ~10 minutes;
#   - qbit_port_sync.sh (qBit's listen port never follows a new lease);
#   - the Content Manager health summary + setup check, check_vpn_required.sh
#     and recreate_gluetun.sh (GET /v1/publicip/ip).
#
# The file opens exactly those read-only routes and nothing else. Every
# other route (PUT /v1/vpn/status — "stop the VPN" — included) becomes
# private immediately, even on v3.41.x: with a file present gluetun no
# longer applies its built-in public role. Nothing here uses them. That
# matters because Byparr's headless browser shares this netns and loads
# arbitrary indexer pages.
#
# GATES on a gluetun bump (tests/local/gluetun_auth.bats enforces the
# first two against this file):
#   - every route below must be one gluetun knows: an unknown route makes
#     gluetun REFUSE TO START ("route path not supported by the control
#     server") — check the new version's validRoutes before bumping;
#   - every localhost:8000 route the tree calls must be listed here;
#   - GET /v1/openvpn/portforwarded is the legacy alias (301 to
#     /v1/portforward). Nothing current calls it; it stays for a box
#     rolled back to an older tree whose healthcheck + port sync still
#     do. Drop it only if a future gluetun drops the route.
#
# Gluetun reads the file at start. Installing or changing it restarts
# nothing: running boxes pick it up at their next gluetun start (boot,
# cascade restart, VPN country change, image bump).
#
# Called (as root) from setup_memory_tuning.sh — the hook every delivery
# path runs: deploy, OTA, first_boot, sync_source_box — and from
# setup_services.sh before its `compose up`. Idempotent; never fatal
# (exit 0 on every path; a failure is logged).
#
# Usage: ensure_gluetun_auth.sh [--provision] [SERVICES_DIR]
#   SERVICES_DIR defaults to /opt/magic_dingus_box/services.
#   Without --provision it acts only on a Media Browser box (services/.env
#   or services/config/gluetun present), so a games-only box gets no
#   stray directories. setup_services.sh passes --provision.

set -uo pipefail

PROVISION=false
if [[ "${1:-}" == "--provision" ]]; then
    PROVISION=true
    shift
fi
SERVICES_DIR="${1:-/opt/magic_dingus_box/services}"

log() { echo "[gluetun-auth] $1"; }

AUTH_DIR="${SERVICES_DIR}/config/gluetun/auth"
AUTH_FILE="${AUTH_DIR}/config.toml"

read -r -d '' CONTENT << 'EOF'
# Installed by magic_dingus_box_cpp/scripts/ensure_gluetun_auth.sh — edits
# here are overwritten. Opens only the read-only routes the Magic Dingus
# Box reads; every other control-server route requires authentication.
[[roles]]
name = "mdb-local-readonly"
routes = ["GET /v1/portforward", "GET /v1/publicip/ip", "GET /v1/openvpn/portforwarded"]
auth = "none"
EOF
CONTENT+=$'\n'

if [[ ! -d "$SERVICES_DIR" ]]; then
    log "no ${SERVICES_DIR}; skipping"
    exit 0
fi
if [[ "$PROVISION" != "true" && ! -f "${SERVICES_DIR}/.env" \
      && ! -d "${SERVICES_DIR}/config/gluetun" ]]; then
    log "Media Browser not provisioned; skipping"
    exit 0
fi

if [[ -f "$AUTH_FILE" ]] && [[ "$(cat "$AUTH_FILE"; echo x)" == "${CONTENT}x" ]]; then
    log "control-server access file up to date"
    exit 0
fi

# Owned like the services dir it lives under (the operator's user), so a
# later non-root edit of config/ never trips over a root-owned file.
OWNER="$(stat -c '%u:%g' "$SERVICES_DIR" 2>/dev/null || stat -f '%u:%g' "$SERVICES_DIR" 2>/dev/null || echo "")"

if ! mkdir -p "$AUTH_DIR"; then
    log "WARNING: cannot create ${AUTH_DIR}; gluetun keeps its built-in defaults"
    exit 0
fi
TMP="$(mktemp "${AUTH_DIR}/.config.toml.XXXXXX" 2>/dev/null)" || {
    log "WARNING: cannot write in ${AUTH_DIR}; gluetun keeps its built-in defaults"
    exit 0
}
if printf '%s' "$CONTENT" > "$TMP" && chmod 0644 "$TMP" && mv -f "$TMP" "$AUTH_FILE"; then
    if [[ -n "$OWNER" ]]; then
        chown "$OWNER" "${SERVICES_DIR}/config" "${SERVICES_DIR}/config/gluetun" \
            "$AUTH_DIR" "$AUTH_FILE" 2>/dev/null || true
    fi
    log "control-server access file installed (takes effect at gluetun's next start)"
else
    rm -f "$TMP"
    log "WARNING: writing ${AUTH_FILE} failed; gluetun keeps its built-in defaults"
fi
exit 0
