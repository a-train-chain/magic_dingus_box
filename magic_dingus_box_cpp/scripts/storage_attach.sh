#!/usr/bin/env bash
#
# storage_attach.sh — re-link the media containers to the movie drive when
# it is attached AFTER the Docker stack has already started.
#
# THE PROBLEM
# Radarr and qBittorrent bind subdirectories of the drive:
#     ${STORAGE_ROOT}/library:/library
#     ${STORAGE_ROOT}/downloads:/downloads
# Docker resolves a bind source once, at container start. If the stack came
# up while /mnt/ssd was unmounted, those binds point at the empty
# placeholder directories on the SD card. When the operator later plugs the
# drive in, the HOST mount succeeds (udev/99-magic-movies-mount.rules) but the
# running containers keep looking at the old, empty directories — Radarr
# reports an empty library and imports fail, with nothing in any log to
# explain why.
#
# Mount propagation cannot fix this. The mount event happens at the PARENT
# (/mnt/ssd) while the binds are on its CHILDREN; propagation carries events
# DOWN into a bind, never up from above it. Re-creating the containers is
# the only reliable re-link.
#
# THE GUARD
# This runs on every activation of mnt-ssd.mount, including the ordinary
# boot-with-drive-present case, so it must be a no-op unless the containers
# are genuinely stale. It writes a one-off random token onto the DRIVE and
# asks each storage-bound container whether it can read that token through
# its bind (Radarr via /library, qBittorrent via /downloads). A container
# that cannot see the token is looking at something other than the drive —
# the stale placeholder — and only that triggers a re-create.
#
# This replaced an entry-COUNT comparison (host has entries, container sees
# none) that could never fire in practice: setup_services.sh always creates
# ${STORAGE_ROOT}/library/tv/.mdb-keep, so with no drive attached the SD-card
# placeholder already holds `tv`, the container "saw" one entry, and a drive
# plugged in later left Radarr on an empty library and qBittorrent writing
# downloads onto the SD card. Identity, not emptiness, is what matters.

BASE="${MAGIC_BASE_DIR:-/opt/magic_dingus_box}"
COMPOSE_DIR="${BASE}/services"
STORAGE_ROOT="${STORAGE_ROOT:-/mnt/ssd}"
LOG_TAG="mdb-storage-attach"

log() { logger -t "$LOG_TAG" -- "$1" 2>/dev/null; echo "[$LOG_TAG] $1"; }

# Pure decision: is one container's bind live, stale, or unknown?
#   $1 token written on the drive ("" if the write failed)
#   $2 what the container read back through its bind
#   $3 "running" | anything else (container absent/stopped)
bind_verdict() {
    local expected="$1" seen="$2" state="$3"
    if [[ -z "$expected" || "$state" != "running" ]]; then
        echo unknown
    elif [[ "$seen" == "$expected" ]]; then
        echo live
    else
        echo stale
    fi
}

# Re-link when ANY probed container is stale; unknown never triggers churn.
#   $@ verdicts from bind_verdict
relink_needed() {
    local v
    for v in "$@"; do
        [[ "$v" == "stale" ]] && return 0
    done
    return 1
}

container_state() {
    if docker ps --format '{{.Names}}' 2>/dev/null | grep -qx "$1"; then
        echo running
    else
        echo absent
    fi
}

# Write a fresh token under host dir $1; print it ("" on failure).
write_token() {
    local dir="$1" tok
    tok="$(date +%s%N)-$$-${RANDOM}"
    if printf '%s' "$tok" > "${dir}/.mdb-bind-probe" 2>/dev/null; then
        echo "$tok"
    else
        echo ""
    fi
}

# Print what container $1 reads at $2/.mdb-bind-probe ("" if unreadable).
read_token() {
    docker exec "$1" cat "$2/.mdb-bind-probe" 2>/dev/null | tr -d '\r\n'
}

# Probe both storage-bound containers; prints "<radarr> <qbit>" verdicts.
probe_binds() {
    local lib_tok dl_tok r_state q_state r_seen="" q_seen=""
    lib_tok="$(write_token "${STORAGE_ROOT}/library")"
    dl_tok="$(write_token "${STORAGE_ROOT}/downloads")"
    r_state="$(container_state mdb_radarr)"
    q_state="$(container_state mdb_qbittorrent)"
    [[ "$r_state" == running ]] && r_seen="$(read_token mdb_radarr /library)"
    [[ "$q_state" == running ]] && q_seen="$(read_token mdb_qbittorrent /downloads)"
    rm -f "${STORAGE_ROOT}/library/.mdb-bind-probe" "${STORAGE_ROOT}/downloads/.mdb-bind-probe" 2>/dev/null
    echo "$(bind_verdict "$lib_tok" "$r_seen" "$r_state") $(bind_verdict "$dl_tok" "$q_seen" "$q_state")"
}

main() {
    # Unprovisioned box: no Media Browser stack to re-link.
    if [[ ! -f "${COMPOSE_DIR}/.env" ]]; then
        log "services/.env absent — Media Browser unprovisioned, nothing to do"
        exit 0
    fi

    if ! mountpoint -q "$STORAGE_ROOT"; then
        log "${STORAGE_ROOT} is not a mountpoint — nothing to re-link"
        exit 0
    fi

    # Sonarr's root folder must exist before any import fires. It has been
    # destroyed twice by an unidentified empty-dir sweep (2026-08-02); the
    # keep-file makes it permanently non-empty so sweeps cannot match it.
    # This ensure needs only the mount (guarded above), NOT docker — it must
    # run before the docker-readiness early-exits below, because the
    # early-boot mount-activation state they bail out of is exactly when the
    # directory has to be (re)created. Idempotent: install -d and the
    # keep-file check are both no-ops when the state is already correct.
    TV_ROOT="${STORAGE_ROOT}/library/tv"
    install -d -o magic -g magic "$TV_ROOT"
    [[ -f "${TV_ROOT}/.mdb-keep" ]] || { touch "${TV_ROOT}/.mdb-keep" && chown magic:magic "${TV_ROOT}/.mdb-keep"; }

    # Docker itself may not be up yet if the drive attached very early in boot.
    # In that case magic-dingus-services.service will start the stack with the
    # drive already mounted, which is the correct state anyway.
    if ! docker info >/dev/null 2>&1; then
        log "docker not ready — stack will start with the drive already mounted"
        exit 0
    fi

    if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -qx mdb_radarr; then
        log "mdb_radarr not running — stack start will pick up the mount"
        exit 0
    fi

    # downloads/ must exist on the drive for the qBit probe (setup creates it;
    # a freshly formatted drive may not have it yet).
    install -d -o magic -g magic "${STORAGE_ROOT}/downloads" 2>/dev/null || true

    read -r radarr_v qbit_v <<< "$(probe_binds)"
    log "bind probe: radarr(/library)=${radarr_v} qbittorrent(/downloads)=${qbit_v}"

    if relink_needed "$radarr_v" "$qbit_v"; then
        log "STALE BIND detected — recreating storage-bound containers"
        cd "$COMPOSE_DIR" || exit 1

        # The container must be REMOVED and re-created, not restarted: a
        # restart reuses the existing container along with its already-resolved
        # bind mounts, which is exactly the stale state we need to discard.
        #
        # Explicit `rm -s -f` then `up -d`, NOT `up -d --force-recreate`.
        # --force-recreate works by renaming the old container to a
        # hash-prefixed name (1ab636a1a325_mdb_radarr) before removing it; if
        # any earlier recreate left one of those renamed containers behind, the
        # next run dies with "Conflict. The container name ... is already in
        # use" and the re-link silently never happens. Observed on hardware.
        # Removing first is deterministic and has no rename step to collide.
        #
        # Sweep orphaned renames first so a box that already hit the conflict
        # can self-heal instead of failing forever.
        for orphan in $(docker ps -aq --filter 'name=_mdb_radarr' --filter 'name=_mdb_sonarr' --filter 'name=_mdb_qbittorrent' 2>/dev/null); do
            log "removing orphaned renamed container ${orphan}"
            docker rm -f "$orphan" >/dev/null 2>&1
        done

        # radarr/qbittorrent declare depends_on gluetun with
        # condition: service_healthy, so `up` BLOCKS until gluetun's
        # healthcheck passes (it exercises DNS+TCP+TLS through the tunnel and
        # can take tens of seconds after a reconnect). Give it room, and never
        # discard the error — swallowing stderr here once turned a one-line
        # diagnosis into a debugging session.
        out=$(cd "$COMPOSE_DIR" && {
                timeout 120 docker compose rm -s -f radarr sonarr qbittorrent 2>&1
                timeout 300 docker compose up -d radarr sonarr qbittorrent 2>&1
              })
        rc=$?
        if (( rc == 0 )); then
            sleep 5
            read -r radarr_v qbit_v <<< "$(probe_binds)"
            if relink_needed "$radarr_v" "$qbit_v"; then
                log "WARNING: still stale after recreate (radarr=${radarr_v} qbittorrent=${qbit_v})"
            else
                log "re-link OK (radarr=${radarr_v} qbittorrent=${qbit_v})"
            fi
        else
            log "ERROR: docker compose failed (rc=${rc})"
            while IFS= read -r line; do
                [[ -n "$line" ]] && log "  compose: ${line}"
            done <<< "$(tail -8 <<<"$out")"
            exit 1
        fi
    else
        log "binds are live (or unprobeable) — no action needed"
    fi

    exit 0
}

# Sourceable for tests (tests/local/storage_attach_decision.bats).
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    set -uo pipefail
    main "$@"
fi
