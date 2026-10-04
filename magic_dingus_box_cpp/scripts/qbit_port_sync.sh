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
#
# Sourceable for tests (tests/local/drive_guard_decision.bats): every
# path below is env-overridable and main only runs when executed.

ENV_FILE="${ENV_FILE:-/opt/magic_dingus_box/services/.env}"
GLUETUN_API='http://localhost:8000/v1/openvpn/portforwarded'
QBIT_API="${QBIT_API:-http://localhost:8080}"
QBIT_USER='admin'

# ---------------------------------------------------------------------------
# Drive-absent download guard (runs every 60 s with the timer).
# ---------------------------------------------------------------------------
# If the MOVIES drive is unplugged, /mnt/ssd silently degrades to a plain
# directory on the SD card — and active torrents then write to the OS
# partition until it is FULL, which kills the box. Runs here because this
# script already holds an authenticated qBit session on a 60 s cadence —
# and it runs BEFORE the Gluetun port fetch on purpose, so a lapsed NAT-PMP
# lease (its own early exit) can never postpone the stop.
#
# Contract (2026-10 rewrite — the first version stopped torrents ONCE,
# behind a tmpfs marker, and then trusted them to stay stopped):
#   * ENFORCE on EVERY tick while the drive is absent. Anything that
#     starts torrents behind the guard's back — the kiosk's resume_all at
#     game/movie exit and its boot-time recovery, a download newly added
#     from the Movies screen — is stopped again within one tick instead of
#     writing into the SD card until it fills. Idempotent: only torrents
#     that are actually active are touched.
#   * RECORD what it stopped (hashes) in a PERSISTENT state dir, not
#     tmpfs. The old /tmp marker vanished on reboot, so torrents stopped by
#     the guard stayed stopped forever when the drive came back across a
#     power cycle. Release starts exactly the recorded set, so torrents the
#     operator had stopped themselves are never started by the guard.
#   * NEW torrents arrive stopped while the drive is absent: qBit's "add
#     stopped" preference is switched on (previous value saved) and
#     restored on release.
#   * RELEASE only once qBittorrent can actually SEE the drive through its
#     /downloads bind. A drive plugged back in mounts on the host first; a
#     qBit container started without it keeps looking at the SD-card
#     placeholder until storage_attach.sh re-creates it. Resuming in that
#     window would write into the SD card exactly like the unplugged case.
GUARD_STATE_DIR="${GUARD_STATE_DIR:-/var/lib/magic-dingus/drive_guard}"
GUARD_HASHES="${GUARD_STATE_DIR}/stopped_hashes"
GUARD_PREFS="${GUARD_STATE_DIR}/add_stopped_prev.json"
LEGACY_GUARD_MARKER="${LEGACY_GUARD_MARKER:-/tmp/mdb_drive_guard_paused}"
STORAGE_ROOT="${STORAGE_ROOT:-/mnt/ssd}"

# Pure decision: what should the guard do this tick?
#   $1 1 if STORAGE_ROOT is a mountpoint, else 0
#   $2 1 if the guard is engaged (it has stopped torrents / owns the pref)
#   $3 qBit's view of the drive: live | stale | unknown
# Prints enforce | release | none.
guard_decision() {
    local mounted="$1" engaged="$2" view="$3"
    if [ "$mounted" != "1" ]; then
        echo enforce
    elif [ "$engaged" != "1" ]; then
        echo none
    elif [ "$view" = "live" ]; then
        echo release
    else
        # Drive is back on the host, but qBit is still on the stale
        # placeholder bind (or we could not tell). Keep everything stopped
        # until storage_attach.sh has re-linked it.
        echo enforce
    fi
}

# Hashes of torrents that are not already stopped/paused, one per line.
# Reads /api/v2/torrents/info JSON on stdin; exits non-zero when the input
# is not a torrent list. qBit 5.x names the idle states stoppedDL/stoppedUP,
# 4.x pausedDL/pausedUP; everything else (downloading, seeding, queued,
# stalled, checking, error, missingFiles...) counts as active — stopping an
# errored torrent is harmless, and recording it means release puts it back
# the way the guard found it.
active_hashes() {
    python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(1)
if not isinstance(data, list):
    sys.exit(1)
for t in data:
    if not isinstance(t, dict):
        continue
    h = t.get("hash") or ""
    st = t.get("state") or ""
    if h and not (st.startswith("stopped") or st.startswith("paused")):
        print(h)
'
}

# Union of the hash lists in the given files ("-" = stdin), sorted,
# de-duplicated, no blank lines. Missing files count as empty.
merge_hashes() {
    local f
    for f in "$@"; do
        if [ "$f" = "-" ]; then cat; elif [ -f "$f" ]; then cat "$f"; fi
    done | grep -v '^[[:space:]]*$' | sort -u
}

# "a\nb\nc" -> "a|b|c" (qBit's hashes= list syntax); ALL anywhere -> "all".
hashes_param() {
    local list
    list=$(merge_hashes -)
    if printf '%s\n' "$list" | grep -qx 'ALL'; then
        echo all
    else
        printf '%s\n' "$list" | paste -sd'|' -
    fi
}

# POST hashes=<$3> to the modern (5.x stop/start) endpoint, falling back to
# the legacy (4.x pause/resume) one. qBittorrent 5.0.3's WebAPI 2.11
# RENAMED pause/resume to stop/start — the old names return 404 (verified
# live; the first version of this guard called only pause/resume and so
# never paused anything). Returns 0 iff either call answered 200.
qbit_torrents_op() {
    local modern="$1" legacy="$2" hashes="$3" rc
    rc=$(curl -sS --max-time 15 -o /dev/null -w '%{http_code}' -b "${COOKIE}" -X POST \
        --data-urlencode "hashes=${hashes}" "${QBIT_API}/api/v2/torrents/${modern}" 2>/dev/null)
    [ "$rc" = "200" ] && return 0
    rc=$(curl -sS --max-time 15 -o /dev/null -w '%{http_code}' -b "${COOKIE}" -X POST \
        --data-urlencode "hashes=${hashes}" "${QBIT_API}/api/v2/torrents/${legacy}" 2>/dev/null)
    [ "$rc" = "200" ]
}

# Can qBittorrent see the drive through its /downloads bind? Same identity
# probe storage_attach.sh uses: a token written on the DRIVE must read back
# inside the container. Prints live | stale | unknown.
qbit_drive_view() {
    local probe="${STORAGE_ROOT}/downloads/.mdb-guard-probe" tok seen
    tok="$(date +%s)-$$-${RANDOM}"
    if ! printf '%s' "$tok" > "$probe" 2>/dev/null; then
        echo unknown
        return
    fi
    seen=$(docker exec mdb_qbittorrent cat /downloads/.mdb-guard-probe 2>/dev/null | tr -d '\r\n')
    rm -f "$probe" 2>/dev/null
    if [ "$seen" = "$tok" ]; then echo live; else echo stale; fi
}

guard_enforce() {
    mkdir -p "$GUARD_STATE_DIR" 2>/dev/null
    local info new count

    # One-time per engagement: make torrents added while the drive is gone
    # arrive stopped. qBit 5.x calls the pref add_stopped_enabled, 4.x
    # start_paused_enabled; only keys this qBit actually reports are saved
    # and set, and the saved original is what release restores.
    if [ ! -f "$GUARD_PREFS" ]; then
        local saved want
        saved=$(curl -sS --max-time 15 -b "${COOKIE}" "${QBIT_API}/api/v2/app/preferences" 2>/dev/null \
            | python3 -c '
import json, sys
p = json.load(sys.stdin)
print(json.dumps({k: p[k] for k in ("add_stopped_enabled", "start_paused_enabled") if k in p}))
' 2>/dev/null)
        if [ -n "$saved" ] && [ "$saved" != "{}" ]; then
            want=$(printf '%s' "$saved" | python3 -c '
import json, sys
print(json.dumps({k: True for k in json.load(sys.stdin)}))')
            if curl -fsS --max-time 15 -b "${COOKIE}" -X POST \
                    --data-urlencode "json=${want}" \
                    "${QBIT_API}/api/v2/app/setPreferences" >/dev/null 2>&1; then
                printf '%s\n' "$saved" > "$GUARD_PREFS"
                echo "[qbit-port-sync] DRIVE GUARD: new downloads will be added stopped until the drive returns"
            fi
        fi
    fi

    info=$(curl -sS --max-time 15 -b "${COOKIE}" "${QBIT_API}/api/v2/torrents/info" 2>/dev/null)
    if ! new=$(printf '%s' "$info" | active_hashes); then
        # Could not list torrents: stop EVERYTHING (disk safety wins) and
        # record the ALL sentinel so release starts everything again — the
        # old behavior, used only when the precise path is blind.
        new="ALL"
    fi

    if [ -n "$new" ]; then
        if qbit_torrents_op stop pause "$(printf '%s\n' "$new" | hashes_param)"; then
            printf '%s\n' "$new" | merge_hashes "$GUARD_HASHES" - > "${GUARD_HASHES}.tmp" \
                && mv -f "${GUARD_HASHES}.tmp" "$GUARD_HASHES"
            count=$(printf '%s\n' "$new" | grep -c .)
            echo "[qbit-port-sync] DRIVE GUARD: drive not reachable by qBit — stopped ${count} active torrent(s)"
        else
            echo "[qbit-port-sync] DRIVE GUARD: WARN stop call failed — retrying next tick"
        fi
    fi
    # Engaged even with nothing to stop, so release restores the pref.
    [ -f "$GUARD_HASHES" ] || : > "$GUARD_HASHES"
}

guard_release() {
    local ok=1 hashes count
    hashes=$(merge_hashes "$GUARD_HASHES")
    if [ -n "$hashes" ]; then
        qbit_torrents_op start resume "$(printf '%s\n' "$hashes" | hashes_param)" || ok=0
    fi
    if [ "$ok" = "1" ] && [ -f "$GUARD_PREFS" ]; then
        if curl -fsS --max-time 15 -b "${COOKIE}" -X POST \
                --data-urlencode "json=$(cat "$GUARD_PREFS")" \
                "${QBIT_API}/api/v2/app/setPreferences" >/dev/null 2>&1; then
            rm -f "$GUARD_PREFS"
        else
            ok=0
        fi
    fi
    if [ "$ok" = "1" ]; then
        rm -f "$GUARD_HASHES"
        count=$(printf '%s' "$hashes" | grep -c .)
        echo "[qbit-port-sync] DRIVE GUARD: drive back — restarted ${count} torrent(s) the guard had stopped"
    else
        echo "[qbit-port-sync] DRIVE GUARD: WARN release incomplete — retrying next tick"
    fi
}

drive_guard_tick() {
    local mounted=0 engaged=0 view=unknown
    mountpoint -q "$STORAGE_ROOT" 2>/dev/null && mounted=1
    # Marker left by the pre-2026-10 guard (tmpfs, "stopped ALL"): fold it
    # into the persistent state so the release still restarts everything.
    if [ -f "$LEGACY_GUARD_MARKER" ]; then
        mkdir -p "$GUARD_STATE_DIR" 2>/dev/null
        printf 'ALL\n' | merge_hashes "$GUARD_HASHES" - > "${GUARD_HASHES}.tmp" \
            && mv -f "${GUARD_HASHES}.tmp" "$GUARD_HASHES" \
            && rm -f "$LEGACY_GUARD_MARKER"
    fi
    [ -f "$GUARD_HASHES" ] && engaged=1
    if [ "$mounted" = "1" ] && [ "$engaged" = "1" ]; then
        view=$(qbit_drive_view)
    fi
    case "$(guard_decision "$mounted" "$engaged" "$view")" in
        enforce) guard_enforce ;;
        release) guard_release ;;
        *)       : ;;
    esac
}

main() {
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
    # drive-absent guard runs on every tick that qBit is reachable at all.
    # It originally sat at the END of this script, which put it behind
    # five early `exit 0`s including the steady-state "in sync" path: the
    # one minute-by-minute case the guard exists for (drive yanked
    # mid-seeding, port long since synced) was exactly the case that never
    # reached it. Cookie file is per-run so we never carry stale session
    # state between invocations.
    COOKIE=$(mktemp /tmp/qbit-cookie-XXXX)
    trap 'rm -f "${COOKIE}"' EXIT

    LOGIN_RESP=$(curl -sS --max-time 15 -c "${COOKIE}" -X POST \
        -d "username=${QBIT_USER}" --data-urlencode "password=${QBIT_PASS}" \
        "${QBIT_API}/api/v2/auth/login" 2>&1)

    if [ "${LOGIN_RESP}" != "Ok." ]; then
        echo "[qbit-port-sync] qBit login failed (response: ${LOGIN_RESP:0:40}); skipping"
        echo "                 likely qBit password drift — magic-dingus-sync-qbit-password should heal on next boot"
        exit 0
    fi

    drive_guard_tick

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
    PREFS=$(curl -sS --max-time 15 -b "${COOKIE}" "${QBIT_API}/api/v2/app/preferences" 2>/dev/null)
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
    curl -sS --max-time 15 -b "${COOKIE}" -X POST \
        --data-urlencode "json={\"listen_port\":${PORT},\"upnp\":false,\"random_port\":false}" \
        "${QBIT_API}/api/v2/app/setPreferences" >/dev/null
    echo "[qbit-port-sync] qBit listen_port set to ${PORT}"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    set -uo pipefail
    main "$@"
fi
