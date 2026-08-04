#!/usr/bin/env bash
# Sync qBittorrent's WebUI password to the value in services/.env.
#
# Background: qBittorrent stores its WebUI password as a PBKDF2 hash
# inside the container's persisted qBittorrent.conf. When the docker
# container is RECREATED (vs just restarted) — which `docker compose
# up -d` does whenever the gluetun service config changes — the
# password change is in-memory until qBit's next graceful shutdown.
# If the recreate sequence kills the container before that flush
# (SIGKILL after timeout), the new container starts up with the OLD
# password from disk. Operator-visible symptom: Radarr can't talk to
# qBit (queue items go to status=warning with "401 Unauthorized"),
# and smoke test reports "qBit login with .env password FAILED".
#
# This script applies Step 7.5's logic (setup_services.sh:397-468)
# as a standalone, boot-time-runnable unit. Idempotent: if qBit
# already accepts the .env password, exits cleanly with no changes.
#
# Invoked from magic-dingus-sync-qbit-password.service after the
# docker stack is up.

set -uo pipefail

ENV_FILE="/opt/magic_dingus_box/services/.env"
QBIT_URL="http://localhost:8080"
LOG_PREFIX="[sync_qbit_password]"

log() { echo "${LOG_PREFIX} $*"; }

# ---------------------------------------------------------------------------
# Alternative-speed-limit crash recovery — $1 is an authenticated cookie jar
# ---------------------------------------------------------------------------
# During movie playback the kiosk engages qBittorrent's alternative speed
# limits so downloads trickle instead of fighting the video for disk, and
# clears them when playback ends — if it gets the chance. A kiosk that
# dies mid-movie (crash, power cut, or now the standby switch) leaves the
# cap ON, and from then on every download on that box crawls at ~2 MB/s
# with nothing on screen to explain why.
#
# The kiosk does attempt this clear at its own startup, and that attempt
# has never once succeeded: it runs seconds after boot while qBittorrent
# is still inside a container that has not finished starting. Measured on
# the production box 2026-08-03, on a completely healthy boot —
# "configure_alt_speed_limits failed: curl: Could not connect to server".
# The standby-wake path makes it strictly worse, because the kiosk now
# deliberately starts BEFORE the Docker stack.
#
# So recovery belongs here: this unit is ordered After=
# magic-dingus-services, i.e. once the containers are actually up. The
# kiosk's own call stays as a best-effort fast path for whenever qBit
# does happen to be ready.
#
# Defined as a function and called from BOTH exit paths. The first
# version of this lived only at the end of the script and never ran at
# all, because the overwhelmingly common case — the password already
# being correct — returns early several stages before that.
reset_alt_speed_limits() {
    local jar="$1" mode after

    mode=$(curl -fsS -b "${jar}" \
        "${QBIT_URL}/api/v2/transfer/speedLimitsMode" 2>/dev/null || echo "")

    if [ "${mode}" = "1" ]; then
        # qBittorrent 5.x exposes no explicit setter for the mode, only a
        # toggle, so read-then-toggle-then-verify is the entire contract.
        curl -fsS -b "${jar}" -X POST \
            "${QBIT_URL}/api/v2/transfer/toggleSpeedLimitsMode" >/dev/null 2>&1 || true
        after=$(curl -fsS -b "${jar}" \
            "${QBIT_URL}/api/v2/transfer/speedLimitsMode" 2>/dev/null || echo "")
        if [ "${after}" = "0" ]; then
            log "alt-speed limits were STUCK ON (crash recovery) — cleared, full speed restored"
        else
            log "WARN: tried to clear stuck alt-speed limits, mode still '${after}'"
        fi
    elif [ "${mode}" = "0" ]; then
        log "alt-speed limits already off (normal)"
    else
        log "WARN: could not read alt-speed mode (got '${mode}')"
    fi

    # Re-assert the trickle RATES regardless, so boxes provisioned before
    # the rates were retuned converge with nobody touching them. These are
    # BYTES per second despite the qBittorrent docs claiming KiB —
    # verified against the live API. 2 MiB/s down, 1 MiB/s up.
    if curl -fsS -b "${jar}" -X POST "${QBIT_URL}/api/v2/app/setPreferences" \
        --data-urlencode 'json={"alt_dl_limit":2097152,"alt_up_limit":1048576}' \
        >/dev/null 2>&1; then
        log "alt-speed rates pinned (2 MiB/s down, 1 MiB/s up)"
    else
        log "WARN: failed to pin alt-speed rates"
    fi
}

if [ ! -f "${ENV_FILE}" ]; then
    log "no .env at ${ENV_FILE}, skipping (services not provisioned)"
    exit 0
fi

QBIT_PW=$(grep '^QBITTORRENT_ADMIN_PASSWORD=' "${ENV_FILE}" | cut -d= -f2-)
if [ -z "${QBIT_PW}" ]; then
    log "WARN: QBITTORRENT_ADMIN_PASSWORD missing from .env, skipping"
    exit 0
fi

# Wait for qBit to be reachable (up to 120s — same cold-start budget
# as the cooldown-clear oneshot).
#
# Reachability probe: hit the WebUI root path (GET /) instead of
# /api/v2/app/version. The version endpoint requires auth → returns
# 403 even when qBit is up and we treat that as a failure. Root
# serves the login HTML without auth, returns HTTP 200 — perfect
# signal that the WebUI listener is alive.
log "waiting for qBit WebUI..."
DEADLINE=$(($(date +%s) + 120))
while [ "$(date +%s)" -lt "${DEADLINE}" ]; do
    if curl -sS -o /dev/null -w "%{http_code}" --max-time 3 "${QBIT_URL}/" 2>/dev/null \
        | grep -qE '^(200|301|302)$'; then
        log "qBit responding"
        break
    fi
    sleep 5
done

# Final check before continuing
CODE=$(curl -sS -o /dev/null -w "%{http_code}" --max-time 3 "${QBIT_URL}/" 2>/dev/null || echo 000)
if ! echo "${CODE}" | grep -qE '^(200|301|302)$'; then
    log "qBit did not respond within 120s (last HTTP ${CODE}); skipping (best-effort)"
    exit 0
fi

COOKIE=$(mktemp)
trap 'rm -f "${COOKIE}"' EXIT

# Try .env password first — happy path, no changes needed.
if curl -fsS -c "${COOKIE}" -X POST "${QBIT_URL}/api/v2/auth/login" \
    -d "username=admin" --data-urlencode "password=${QBIT_PW}" 2>/dev/null \
    | grep -q "Ok\."; then
    log "qBit already accepts .env password (no resync needed)"
    # Still the right place to do the alt-speed recovery: this is the
    # ordinary boot, and a stuck cap is exactly as likely here as on the
    # rare boot where the password also drifted.
    reset_alt_speed_limits "${COOKIE}"
    exit 0
fi

# .env password didn't work — qBit drifted. Fall back to the docker
# default. linuxserver/qbittorrent ships with admin/adminadmin until
# Step 7.5 (or this script) overrides it.
log "qBit rejected .env password — trying docker default 'adminadmin'..."
if curl -fsS -c "${COOKIE}" -X POST "${QBIT_URL}/api/v2/auth/login" \
    -d "username=admin&password=adminadmin" 2>/dev/null \
    | grep -q "Ok\."; then
    log "qBit on default credentials — applying .env password..."
else
    # qBittorrent 5.x removed the adminadmin default: new containers
    # print a per-session temporary password in their log instead.
    # (Hit live on the first Pi 5 provisioning, 2026-07-22.)
    TMP_PW=$(docker logs mdb_qbittorrent 2>&1 | grep "temporary password" | tail -1 | awk '{print $NF}')
    if [ -n "${TMP_PW}" ] && curl -fsS -c "${COOKIE}" -X POST "${QBIT_URL}/api/v2/auth/login" \
        -d "username=admin" --data-urlencode "password=${TMP_PW}" 2>/dev/null \
        | grep -q "Ok\."; then
        log "qBit on session temporary password — applying .env password..."
    else
        log "WARN: qBit rejects .env password, adminadmin, AND the session temporary password; manual recovery needed"
        log "      to recover: stop mdb_qbittorrent, edit qBittorrent.conf to remove WebUI\\Password_PBKDF2 line, start container, grep its logs for temporary password, then re-run this script"
        exit 0
    fi
fi

# Apply the .env password + ensure localhost-bypass stays off.
# (Step 7.5 in setup_services.sh does the same thing; mirroring it
# here means we re-apply on every boot, catching the drift.)
PREFS_JSON=$(python3 -c "
import json,sys
print(json.dumps({
    'web_ui_username': 'admin',
    'web_ui_password': sys.argv[1],
    'bypass_local_auth': False,
    'bypass_auth_subnet_whitelist_enabled': False,
}))" "${QBIT_PW}")

if curl -fsS -b "${COOKIE}" -X POST \
    "${QBIT_URL}/api/v2/app/setPreferences" \
    --data-urlencode "json=${PREFS_JSON}" >/dev/null 2>&1; then
    log "qBit password resynced + localhost-auth-bypass disabled"
else
    log "WARN: failed to apply preferences (setPreferences returned error)"
    exit 0
fi

# Verify the resync worked.
NEW_COOKIE=$(mktemp)
if curl -fsS -c "${NEW_COOKIE}" -X POST "${QBIT_URL}/api/v2/auth/login" \
    -d "username=admin" --data-urlencode "password=${QBIT_PW}" 2>/dev/null \
    | grep -q "Ok\."; then
    log "verified: qBit now accepts .env password"
else
    log "WARN: resync claimed success but qBit still rejects .env password"
fi

reset_alt_speed_limits "${NEW_COOKIE}"

rm -f "${NEW_COOKIE}"
