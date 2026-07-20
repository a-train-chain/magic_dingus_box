#!/usr/bin/env bash
#
# Magic Dingus Box - Prepare for live SD clone
#
# Quiesces the running Pi just enough to take a consistent SD-card
# snapshot via `dd` over SSH WITHOUT permanently destroying any user
# data. The companion script restore_after_cloning.sh undoes everything
# this does, leaving the source Pi in the same state it was before.
#
# What this does (safe to re-run; idempotent):
#   1. Stop kiosk + Content Manager + Docker stack so the FS quiets down
#   2. Snapshot per-Pi identity that needs to differ on each clone
#      (device_info.json + hostname) into a backup dir, then remove
#      them from disk so the cloned image will trigger first_boot.sh
#      to regenerate fresh ones.
#   3. Re-enable magic-first-boot.service (it self-disabled on this
#      Pi long ago; we want it to fire on the cloned Pi's first boot)
#   4. sync; sync; sync (flush dirty pages to SD)
#
# After this script completes, the SD card is in a "ready to be cloned"
# state. The Mac-side orchestrator dd's it and then SSHes back to run
# restore_after_cloning.sh which puts everything back.
#
# CRITICAL: do not reboot the source Pi between this script and
# restore_after_cloning.sh. If you do, first_boot.sh will fire on the
# source and regenerate its identity (which is fine, but then the
# `restore` script's identity-restore will conflict).
#
# Must run as root (operator's Mac-side script runs this via sudo over
# SSH).
#

set -euo pipefail

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
INSTALL_DIR="/opt/magic_dingus_box"
CPP_DIR="${INSTALL_DIR}/magic_dingus_box_cpp"
DATA_DIR="${CPP_DIR}/data"
SERVICES_DIR="${INSTALL_DIR}/services"
BACKUP_DIR="/var/lib/magic-dingus-box/cloning_backup"

DEVICE_INFO_PATH="${DATA_DIR}/device_info.json"
HOSTNAME_PATH="/etc/hostname"
HOSTS_PATH="/etc/hosts"
MARKER_PATH="${BACKUP_DIR}/in_progress"

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
log() {
    echo "$1"
    logger -t "magic-prepare-clone" "$1" 2>/dev/null || true
}

log "=== prepare_for_cloning.sh starting ==="

# ---------------------------------------------------------------------------
# Preflight: must run as root
# ---------------------------------------------------------------------------
if [[ "$EUID" -ne 0 ]]; then
    log "ERROR: must be run as root (operator script invokes via sudo)"
    exit 1
fi

# ---------------------------------------------------------------------------
# Preflight: refuse to run twice without a restore in between
# ---------------------------------------------------------------------------
if [[ -f "$MARKER_PATH" ]]; then
    log "ERROR: prepare-for-cloning marker already present at $MARKER_PATH"
    log "       This means a previous clone attempt did not complete its restore."
    log "       Run /opt/magic_dingus_box/scripts/golden_image/restore_after_cloning.sh"
    log "       first to put the Pi back into normal state, then retry."
    exit 1
fi

# ---------------------------------------------------------------------------
# Step 1: Stop services
# ---------------------------------------------------------------------------
log "[1/5] Stopping kiosk + Content Manager + Docker stack..."

# Order matters: stop the kiosk first (it holds DRM master / EGL context),
# then content manager, then the Docker stack last (it has the most state
# to flush). All `|| true` because each may already be stopped.
systemctl stop magic-dingus-box-cpp.service 2>/dev/null || true
systemctl stop magic-dingus-web.service 2>/dev/null || true

# Stop docker-compose stack via the systemd unit so its ExecStop runs
# (which calls `docker compose down` cleanly). reset-failed first so a
# unit currently in failed state (which can happen if the docker stack
# was previously restarted manually outside systemd) doesn't block the
# stop.
systemctl reset-failed magic-dingus-services.service 2>/dev/null || true
systemctl stop magic-dingus-services.service 2>/dev/null || true

# Belt-and-suspenders: if the systemd unit didn't catch the docker stack,
# do it directly. This handles the case where containers were started
# manually outside the systemd unit.
if [[ -f "$SERVICES_DIR/docker-compose.yml" ]] && command -v docker &>/dev/null; then
    (cd "$SERVICES_DIR" && docker compose down 2>&1 | sed 's/^/    /' || true)
fi

log "[1/5] Services stopped"

# ---------------------------------------------------------------------------
# Step 2: Backup per-Pi identity, then remove from disk
# ---------------------------------------------------------------------------
log "[2/5] Backing up per-Pi identity to ${BACKUP_DIR}..."

mkdir -p "$BACKUP_DIR"
chmod 700 "$BACKUP_DIR"

# Snapshot device_info.json (used by first_boot.sh to gate identity gen)
if [[ -f "$DEVICE_INFO_PATH" ]]; then
    cp -p "$DEVICE_INFO_PATH" "$BACKUP_DIR/device_info.json"
    rm -f "$DEVICE_INFO_PATH"
    log "[2/5] Snapshotted + removed device_info.json"
else
    log "[2/5] device_info.json not present — skipping (first_boot will create one)"
fi

# Snapshot /etc/hostname so we can restore it. The cloned Pi's
# first_boot.sh will set its own hostname from a fresh device_id.
if [[ -f "$HOSTNAME_PATH" ]]; then
    cp -p "$HOSTNAME_PATH" "$BACKUP_DIR/hostname"
    log "[2/5] Snapshotted /etc/hostname (current=$(cat "$HOSTNAME_PATH"))"
fi

# /etc/hosts also has the hostname embedded; snapshot it for restore.
if [[ -f "$HOSTS_PATH" ]]; then
    cp -p "$HOSTS_PATH" "$BACKUP_DIR/hosts"
fi

# We deliberately do NOT remove /etc/ssh/ssh_host_* — operator's active
# SSH session needs them to stay valid until restore. The downside is
# every cloned Pi inherits the same SSH host keys. For a kiosk on a
# trusted LAN that's acceptable; if you care, run ssh-keygen -A on
# each cloned Pi after first boot to rotate them.

# ---------------------------------------------------------------------------
# Step 3: Re-enable magic-first-boot.service
# ---------------------------------------------------------------------------
# This Pi already ran first_boot.sh long ago, which self-disabled the
# unit. We want it ENABLED in the cloned image so it fires on the
# cloned Pi's first boot. restore_after_cloning.sh disables it again
# afterwards so the source Pi doesn't run first-boot logic if it
# reboots later.
log "[3/5] Re-enabling magic-first-boot.service (so cloned Pi runs it)..."

# Self-heal: the unit file may never have been installed on this Pi
# (only prepare_golden_image.sh historically installed it; deploy_cpp.sh
# Step 1.65 now does too, but older deployments predate that). Without
# the unit file, `systemctl enable` fails and set -euo pipefail aborts
# the clone. Install from the repo copy synced to ${INSTALL_DIR}/systemd/.
UNIT_INSTALLED="/etc/systemd/system/magic-first-boot.service"
UNIT_SRC="${INSTALL_DIR}/systemd/magic-first-boot.service"
if [[ ! -f "$UNIT_INSTALLED" ]]; then
    if [[ -f "$UNIT_SRC" ]]; then
        cp "$UNIT_SRC" "$UNIT_INSTALLED"
        systemctl daemon-reload
        log "[3/5] Installed missing magic-first-boot.service unit from ${UNIT_SRC}"
    else
        log "ERROR: magic-first-boot.service unit not installed and no copy at ${UNIT_SRC}"
        log "       Run deploy_cpp.sh from the dev machine first (it syncs systemd/ units),"
        log "       then retry the clone. Without this unit the cloned image would never"
        log "       run first_boot.sh (no identity reset, no WiFi/VPN credential wipe)."
        exit 1
    fi
fi

if systemctl is-enabled magic-first-boot.service &>/dev/null; then
    log "[3/5] magic-first-boot.service was already enabled (unusual but harmless)"
else
    systemctl enable magic-first-boot.service 2>&1 | sed 's/^/    /'
fi

# ---------------------------------------------------------------------------
# Step 4: Drop the in-progress marker so a Pi reboot mid-clone leaves
# evidence for restore_after_cloning.sh to find.
# ---------------------------------------------------------------------------
log "[4/5] Marking clone in progress at ${MARKER_PATH}..."

cat > "$MARKER_PATH" <<EOF
# Magic Dingus Box - clone in progress
# Created: $(date -u +%Y-%m-%dT%H:%M:%SZ)
# Hostname at prep time: $(hostname)
#
# This file exists ONLY between prepare_for_cloning.sh and
# restore_after_cloning.sh. If you see it after a reboot, the previous
# clone attempt did not complete — run restore_after_cloning.sh to
# put the Pi back into normal state.
EOF
chmod 600 "$MARKER_PATH"

# ---------------------------------------------------------------------------
# Step 5: Flush dirty pages to SD so dd reads consistent state
# ---------------------------------------------------------------------------
log "[5/5] Flushing dirty pages to SD card..."

sync
sync
# Drop pagecache to force dd to read directly from the SD (vs cached pages)
echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
sync

log "=== prepare_for_cloning.sh complete ==="
log ""
log "The Pi is now ready to be dd'd. The Mac-side script will:"
log "  1. Stream /dev/mmcblk0 over this SSH session"
log "  2. SSH back to run restore_after_cloning.sh when done"
log ""
log "DO NOT reboot the source Pi until restore completes."
