#!/usr/bin/env bash
#
# Magic Dingus Box - Restore source Pi after live SD clone
#
# Undoes prepare_for_cloning.sh. Restores the per-Pi identity that was
# snapshotted away, disables magic-first-boot.service so this Pi
# doesn't run first-boot logic on its next reboot, and starts the
# kiosk + Docker stack back up.
#
# Idempotent: if the marker is missing (i.e. nothing to restore),
# it's a no-op with a friendly message rather than an error. This
# matters because the Mac-side orchestrator runs this in a trap
# handler that may fire even on the happy path.
#
# Must run as root.
#

set -euo pipefail

INSTALL_DIR="/opt/magic_dingus_box"
CPP_DIR="${INSTALL_DIR}/magic_dingus_box_cpp"
DATA_DIR="${CPP_DIR}/data"
BACKUP_DIR="/var/lib/magic-dingus-box/cloning_backup"

DEVICE_INFO_PATH="${DATA_DIR}/device_info.json"
HOSTNAME_PATH="/etc/hostname"
HOSTS_PATH="/etc/hosts"
MARKER_PATH="${BACKUP_DIR}/in_progress"

log() {
    echo "$1"
    logger -t "magic-restore-clone" "$1" 2>/dev/null || true
}

log "=== restore_after_cloning.sh starting ==="

if [[ "$EUID" -ne 0 ]]; then
    log "ERROR: must be run as root"
    exit 1
fi

# ---------------------------------------------------------------------------
# Idempotency guard
# ---------------------------------------------------------------------------
if [[ ! -f "$MARKER_PATH" ]]; then
    log "No clone-in-progress marker found at ${MARKER_PATH}"
    log "Nothing to restore. (This is fine — restore was a no-op.)"
    exit 0
fi

# ---------------------------------------------------------------------------
# Step 1: Restore per-Pi identity files from backup
# ---------------------------------------------------------------------------
log "[1/4] Restoring per-Pi identity from ${BACKUP_DIR}..."

if [[ -f "$BACKUP_DIR/device_info.json" ]]; then
    mkdir -p "$(dirname "$DEVICE_INFO_PATH")"
    cp -p "$BACKUP_DIR/device_info.json" "$DEVICE_INFO_PATH"
    chown magic:magic "$DEVICE_INFO_PATH" 2>/dev/null || true
    log "[1/4] Restored device_info.json"
fi

if [[ -f "$BACKUP_DIR/hostname" ]]; then
    cp -p "$BACKUP_DIR/hostname" "$HOSTNAME_PATH"
    # hostnamectl picks it up live (without reboot)
    hostnamectl set-hostname "$(cat "$HOSTNAME_PATH")" 2>/dev/null || true
    log "[1/4] Restored /etc/hostname ($(cat "$HOSTNAME_PATH"))"
fi

if [[ -f "$BACKUP_DIR/hosts" ]]; then
    cp -p "$BACKUP_DIR/hosts" "$HOSTS_PATH"
    log "[1/4] Restored /etc/hosts"
fi

# Boot-partition cloud-init files that prepare_for_cloning.sh Step 2b removed
# so the operator's Wi-Fi PSK, the `magic` password hash and the operator's
# SSH public key would not be captured by the dd.
#
# The stash is in /dev/shm (RAM) rather than $BACKUP_DIR precisely because
# $BACKUP_DIR is on the SD card and would have been captured by the dd —
# putting the secret straight back into the image it was removed from.
#
# Consequence worth knowing: /dev/shm does not survive a reboot. If the source
# Pi is rebooted between prepare and restore, these are gone for good. That is
# not data loss in any meaningful sense — cloud-init has already applied them,
# the live Wi-Fi config lives in /etc/NetworkManager, and the box keeps
# working — but the files will not come back. prepare_for_cloning.sh's header
# already warns against rebooting between the two scripts.
BOOT_FW="/boot/firmware"
BOOT_STASH="/dev/shm/mdb-boot-stash"

if [[ -d "$BOOT_STASH" ]]; then
    restored=0
    for f in user-data network-config meta-data; do
        if [[ -f "${BOOT_STASH}/${f}" ]]; then
            cp -p "${BOOT_STASH}/${f}" "${BOOT_FW}/${f}" 2>/dev/null && restored=$((restored + 1))
        fi
    done
    sync
    # Wipe the RAM stash so the credentials do not linger in /dev/shm, which
    # is world-readable by default.
    rm -rf "$BOOT_STASH"
    log "[1/4] Restored ${restored} boot-partition file(s) and cleared the stash"
else
    log "[1/4] No boot-partition stash found (nothing to restore)"
fi

# Application secrets removed by prepare_for_cloning.sh Step 2c so the .img.gz
# artifact would not carry the ProtonVPN private key, the phone-remote HMAC
# secret or the TMDB key. The manifest records each file's original path so
# they go back exactly where they came from, with their ownership.
SECRET_STASH="/dev/shm/mdb-secret-stash"

if [[ -f "${SECRET_STASH}/manifest" ]]; then
    restored=0
    while IFS=$'\t' read -r key dest mode uid gid; do
        [[ -n "$key" && -n "$dest" ]] || continue
        [[ -f "${SECRET_STASH}/${key}" ]] || continue
        mkdir -p "$(dirname "$dest")"
        cp -p "${SECRET_STASH}/${key}" "$dest"
        if [[ -n "$mode" && -n "$uid" && -n "$gid" ]]; then
            # Exact inverse of prepare: put back the ownership and mode the file
            # actually had. The Radarr/Prowlarr/qBittorrent files are owned by
            # the container user (1000:1000, mode 644); blanket-chowning them to
            # magic and forcing 600 — as this did before the manifest carried
            # these fields — silently changed service state the clone then
            # inherited.
            chown "${uid}:${gid}" "$dest" 2>/dev/null || true
            chmod "$mode" "$dest" 2>/dev/null || true
        else
            # Stash written by an older prepare_for_cloning.sh, which recorded
            # only key and dest. Keep the previous behaviour for those.
            case "$dest" in
                /home/magic/*) chown magic:magic "$dest" 2>/dev/null || true ;;
                /opt/magic_dingus_box/*) chown magic:magic "$dest" 2>/dev/null || true ;;
            esac
            chmod 600 "$dest" 2>/dev/null || true
        fi
        restored=$((restored + 1))
    done < "${SECRET_STASH}/manifest"
    sync
    rm -rf "$SECRET_STASH"
    log "[1/4] Restored ${restored} application secret(s) and cleared the stash"

    # The stash now includes the Wi-Fi profile (*.nmconnection). The active
    # connection survived in NM's memory while the file was gone (NM does not
    # watch connection files); reload so NM's file view matches again rather
    # than waiting for the next reboot. Best-effort — Ethernet-only boxes
    # have nothing to reload.
    nmcli connection reload 2>/dev/null || true
else
    log "[1/4] No application-secret stash found (nothing to restore)"
fi

# ---------------------------------------------------------------------------
# Step 2: Disable magic-first-boot.service
# ---------------------------------------------------------------------------
# We re-enabled it during prepare so the cloned image would run it.
# On the source Pi we DO NOT want it firing (it would wipe all the
# state we just restored). Disable now.
log "[2/4] Disabling magic-first-boot.service on source Pi..."

if systemctl is-enabled magic-first-boot.service &>/dev/null; then
    systemctl disable magic-first-boot.service 2>&1 | sed 's/^/    /'
fi

# ---------------------------------------------------------------------------
# Step 3: Restart services
# ---------------------------------------------------------------------------
log "[3/4] Starting services back up..."

# Order matters: Docker stack first (kiosk's Media Browser depends on
# Radarr being reachable), then content manager, then the kiosk last.
# reset-failed first so a previously-failed unit can be restarted.
# The container-runtime daemons first. prepare_for_cloning.sh stops dockerd and
# containerd outright (not just the compose stack) so neither can rewrite its
# secret-bearing metadata after the free-space zeroing has run. Nothing below
# can start without them: magic-dingus-services shells out to `docker compose`.
systemctl start containerd.service 2>/dev/null || true
systemctl start docker.socket 2>/dev/null || true
systemctl start docker.service 2>&1 | sed 's/^/    /' || \
    log "[3/4] WARN: docker.service failed to start — the stack cannot come up without it"

systemctl reset-failed magic-dingus-services.service 2>/dev/null || true
systemctl start magic-dingus-services.service 2>&1 | sed 's/^/    /' || \
    log "[3/4] WARN: magic-dingus-services.service failed to start (check docker logs)"

systemctl start magic-dingus-web.service 2>&1 | sed 's/^/    /' || \
    log "[3/4] WARN: magic-dingus-web.service failed to start"

systemctl start magic-dingus-box-cpp.service 2>&1 | sed 's/^/    /' || \
    log "[3/4] WARN: magic-dingus-box-cpp.service failed to start"

# Re-arm the front-panel standby switch, which prepare stopped so a bump
# of the toggle could not restart services underneath the dd.
if systemctl is-enabled kiosk-standby-watcher.service &>/dev/null; then
    systemctl start kiosk-standby-watcher.service 2>&1 | sed 's/^/    /' || \
        log "[3/4] WARN: kiosk-standby-watcher.service failed to start"
fi

# ---------------------------------------------------------------------------
# Step 4: Clear the in-progress marker + cleanup
# ---------------------------------------------------------------------------
log "[4/4] Removing clone-in-progress marker..."

rm -f "$MARKER_PATH"

# Backup files no longer needed; remove them so a future prepare run
# starts from a clean slate.
rm -f "$BACKUP_DIR/device_info.json" "$BACKUP_DIR/hostname" "$BACKUP_DIR/hosts"
rmdir "$BACKUP_DIR" 2>/dev/null || true

log "=== restore_after_cloning.sh complete ==="
log ""
log "Source Pi is fully restored. Verify services with:"
log "  systemctl is-active magic-dingus-box-cpp.service"
log "  systemctl is-active magic-dingus-services.service"
log "  docker ps"
