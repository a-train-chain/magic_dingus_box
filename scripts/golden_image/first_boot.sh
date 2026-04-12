#!/usr/bin/env bash
set -euo pipefail

#
# Magic Dingus Box - First Boot Setup
#
# Runs ONCE on each newly cloned Pi at first boot. Handles:
#   - SSH host key regeneration
#   - Root filesystem expansion to fill SD card
#   - Unique device identity generation
#   - Required directory creation
#   - Ownership/permission fixup
#   - Self-disabling so it never runs again
#
# This script is enabled by prepare_golden_image.sh and triggered by
# magic-first-boot.service (oneshot, before the kiosk app starts).
#
# Must run as root (systemd runs it as root by default).
#

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
INSTALL_DIR="/opt/magic_dingus_box"
CPP_DIR="${INSTALL_DIR}/magic_dingus_box_cpp"
DATA_DIR="${CPP_DIR}/data"
CONFIG_DIR="${INSTALL_DIR}/config"
MAGIC_USER="magic"
MAGIC_HOME="/home/${MAGIC_USER}"

# ---------------------------------------------------------------------------
# Logging helper - stdout + syslog
# ---------------------------------------------------------------------------
log() {
    echo "$1"
    logger -t "magic-first-boot" "$1"
}

log "=== Magic Dingus Box first-boot setup starting ==="

# ---------------------------------------------------------------------------
# Step 1: Regenerate SSH host keys
# ---------------------------------------------------------------------------
log "[1/6] Regenerating SSH host keys..."

if ls /etc/ssh/ssh_host_* &>/dev/null; then
    log "[1/6] SSH host keys already exist, skipping regeneration"
else
    ssh-keygen -A
    log "[1/6] SSH host keys regenerated"

    # Restart sshd to pick up new keys
    if systemctl is-active sshd.service &>/dev/null; then
        systemctl restart sshd.service
        log "[1/6] sshd restarted with new host keys"
    elif systemctl is-active ssh.service &>/dev/null; then
        systemctl restart ssh.service
        log "[1/6] ssh restarted with new host keys"
    else
        log "[1/6] Warning: sshd not running, keys will be used on next start"
    fi
fi

# ---------------------------------------------------------------------------
# Step 2: Expand root filesystem to fill SD card
# ---------------------------------------------------------------------------
log "[2/6] Expanding root filesystem..."

# Wait for udev to settle before querying block devices
udevadm settle

ROOT_PART=$(findmnt -n -o SOURCE /)
ROOT_DISK_NAME=$(lsblk -no pkname "$ROOT_PART" 2>/dev/null || true)

if [[ -z "$ROOT_DISK_NAME" ]]; then
    log "[2/6] WARNING: Could not determine parent disk for ${ROOT_PART}, skipping expansion"
else
    ROOT_DISK="/dev/${ROOT_DISK_NAME}"

    if [[ ! -b "$ROOT_DISK" ]]; then
        log "[2/6] WARNING: ${ROOT_DISK} is not a block device, skipping expansion"
    else
        PART_NUM=$(echo "$ROOT_PART" | grep -o '[0-9]*$')

        # Check if there is unallocated space after the root partition
        # Use parted to get the partition end and disk size in bytes
        PART_END=$(parted -s "$ROOT_DISK" unit B print 2>/dev/null | awk "/^ ${PART_NUM} /{gsub(/B/,\"\"); print \$3}" || echo "0")
        DISK_END=$(parted -s "$ROOT_DISK" unit B print 2>/dev/null | grep "Disk ${ROOT_DISK}" | awk '{gsub(/B/,""); print $3}' || echo "0")

        # If we couldn't parse parted output, just run the expansion (safe to re-run)
        if [[ "$PART_END" == "0" || "$DISK_END" == "0" ]]; then
            NEEDS_EXPAND=true
        else
            # Skip if partition end is within 100MB of disk end
            THRESHOLD=$((100 * 1024 * 1024))
            GAP=$((DISK_END - PART_END))
            if [[ "$GAP" -lt "$THRESHOLD" ]]; then
                NEEDS_EXPAND=false
            else
                NEEDS_EXPAND=true
            fi
        fi

        if [[ "$NEEDS_EXPAND" == false ]]; then
            log "[2/6] Filesystem already fills the SD card, skipping expansion"
        else
            log "[2/6] Expanding partition ${PART_NUM} on ${ROOT_DISK}..."
            parted -s "$ROOT_DISK" resizepart "$PART_NUM" 100%

            # Notify kernel of the updated partition table
            partprobe "$ROOT_DISK" 2>/dev/null || true
            udevadm settle

            log "[2/6] Partition expanded, resizing filesystem on ${ROOT_PART}..."
            resize2fs "$ROOT_PART"
            log "[2/6] Filesystem expanded successfully"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# Step 3: Generate unique device identity
# ---------------------------------------------------------------------------
log "[3/6] Generating unique device identity..."

DEVICE_INFO_PATH="${DATA_DIR}/device_info.json"

if [[ -f "$DEVICE_INFO_PATH" ]]; then
    log "[3/6] device_info.json already exists, skipping"
else
    DEVICE_UUID=$(cat /proc/sys/kernel/random/uuid)
    CREATED_AT=$(date +%s)

    cat > "$DEVICE_INFO_PATH" <<DEVEOF
{
    "device_id": "${DEVICE_UUID}",
    "device_name": "Magic Dingus Box",
    "created_at": ${CREATED_AT}
}
DEVEOF

    chown "${MAGIC_USER}:${MAGIC_USER}" "$DEVICE_INFO_PATH"

    # Set unique hostname derived from device UUID (e.g., magicpi-a3f2)
    SHORT_ID=$(echo "$DEVICE_UUID" | cut -c1-4)
    NEW_HOSTNAME="magicpi-${SHORT_ID}"
    hostnamectl set-hostname "$NEW_HOSTNAME"
    log "[3/6] Device identity created: ${DEVICE_UUID} (hostname: ${NEW_HOSTNAME})"
fi

# ---------------------------------------------------------------------------
# Step 4: Create required directories
# ---------------------------------------------------------------------------
log "[4/6] Creating required directories..."

mkdir -p "${DATA_DIR}/saves"
mkdir -p "${DATA_DIR}/states"
mkdir -p "${DATA_DIR}/media"
mkdir -p "${CONFIG_DIR}"

log "[4/6] Directories verified: saves, states, media, config"

# ---------------------------------------------------------------------------
# Step 5: Fix permissions
# ---------------------------------------------------------------------------
log "[5/6] Fixing file ownership..."

# Only fix ownership on directories/files that first-boot created or modified.
# The golden image already has correct ownership on ROMs, cores, binary, etc.
# A full recursive chown would take 30-120s on a ROM-laden 2GB Pi.
chown "${MAGIC_USER}:${MAGIC_USER}" "${DATA_DIR}/saves" "${DATA_DIR}/states" "${DATA_DIR}/media"
chown "${MAGIC_USER}:${MAGIC_USER}" "${CONFIG_DIR}"
if [[ -f "${DATA_DIR}/device_info.json" ]]; then
    chown "${MAGIC_USER}:${MAGIC_USER}" "${DATA_DIR}/device_info.json"
fi
log "[5/6] Set ownership on first-boot created directories and files"

# ---------------------------------------------------------------------------
# Step 6: Disable this service (run once only)
# ---------------------------------------------------------------------------
log "[6/6] Disabling first-boot service..."

systemctl disable magic-first-boot.service
log "[6/6] magic-first-boot.service disabled (will not run again)"

log "=== Magic Dingus Box first-boot setup complete ==="
