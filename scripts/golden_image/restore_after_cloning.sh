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
systemctl reset-failed magic-dingus-services.service 2>/dev/null || true
systemctl start magic-dingus-services.service 2>&1 | sed 's/^/    /' || \
    log "[3/4] WARN: magic-dingus-services.service failed to start (check docker logs)"

systemctl start magic-dingus-web.service 2>&1 | sed 's/^/    /' || \
    log "[3/4] WARN: magic-dingus-web.service failed to start"

systemctl start magic-dingus-box-cpp.service 2>&1 | sed 's/^/    /' || \
    log "[3/4] WARN: magic-dingus-box-cpp.service failed to start"

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
