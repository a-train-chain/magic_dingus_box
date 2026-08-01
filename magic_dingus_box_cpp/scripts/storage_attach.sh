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
# drive in, the HOST mount succeeds (fstab has x-systemd.automount) but the
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
# are genuinely stale. It compares what the host sees in the library against
# what Radarr sees through its bind; only a host-has-content /
# container-sees-nothing split triggers a restart. That makes it safe to run
# repeatedly and safe to run at boot.
set -uo pipefail

BASE="${MAGIC_BASE_DIR:-/opt/magic_dingus_box}"
COMPOSE_DIR="${BASE}/services"
STORAGE_ROOT="${STORAGE_ROOT:-/mnt/ssd}"
LOG_TAG="mdb-storage-attach"

log() { logger -t "$LOG_TAG" -- "$1"; echo "[$LOG_TAG] $1"; }

# Unprovisioned box: no Media Browser stack to re-link.
if [[ ! -f "${COMPOSE_DIR}/.env" ]]; then
    log "services/.env absent — Media Browser unprovisioned, nothing to do"
    exit 0
fi

if ! mountpoint -q "$STORAGE_ROOT"; then
    log "${STORAGE_ROOT} is not a mountpoint — nothing to re-link"
    exit 0
fi

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

host_count=$(find "${STORAGE_ROOT}/library" -maxdepth 1 -mindepth 1 2>/dev/null | wc -l | tr -d ' ')
cont_count=$(docker exec mdb_radarr sh -c 'find /library -maxdepth 1 -mindepth 1 2>/dev/null | wc -l' 2>/dev/null | tr -d ' \r')
cont_count=${cont_count:-0}

log "library entries: host=${host_count} container=${cont_count}"

# The stale signature: the drive has content but the container's bind still
# points at the empty pre-mount directory. Equal counts (including 0 == 0 on
# a genuinely empty drive) mean the bind is live and we must not churn the
# stack.
if (( host_count > 0 && cont_count == 0 )); then
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
        new_count=$(docker exec mdb_radarr sh -c 'find /library -maxdepth 1 -mindepth 1 2>/dev/null | wc -l' 2>/dev/null | tr -d ' \r')
        new_count=${new_count:-0}
        if (( new_count > 0 )); then
            log "re-link OK — container now sees ${new_count} library entries"
        else
            log "WARNING: container still sees 0 entries after recreate"
        fi
    else
        log "ERROR: docker compose failed (rc=${rc})"
        while IFS= read -r line; do
            [[ -n "$line" ]] && log "  compose: ${line}"
        done <<< "$(tail -8 <<<"$out")"
        exit 1
    fi
else
    log "bind is live — no action needed"
fi

exit 0
