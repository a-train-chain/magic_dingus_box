#!/bin/bash
set -euo pipefail

# Magic Dingus Box: configure read-only root using overlayfs with /data as RW.
# This script targets Raspberry Pi OS (Bookworm/Trixie).
#
# Usage examples:
#   sudo ./scripts/os_ro_setup.sh --data-device /dev/mmcblk0p3
#   sudo ./scripts/os_ro_setup.sh --data-uuid 1234-ABCD
#   sudo ./scripts/os_ro_setup.sh               # assumes /data already mounted

DATA_DEVICE=""
DATA_UUID=""
NO_FORMAT=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --data-device)
      DATA_DEVICE="$2"; shift 2;;
    --data-uuid)
      DATA_UUID="$2"; shift 2;;
    --no-format)
      NO_FORMAT=1; shift;;
    *)
      echo "Unknown arg: $1" >&2; exit 1;;
  esac
done

require_root() {
  if [[ "$(id -u)" -ne 0 ]]; then
    echo "Run as root (sudo)." >&2
    exit 1
  fi
}

in_fstab() {
  local pattern="$1"
  grep -qF -- "$pattern" /etc/fstab 2>/dev/null || return 1
}

append_fstab() {
  local line="$1"
  if ! in_fstab "$line"; then
    echo "$line" >> /etc/fstab
    echo "Added to /etc/fstab: $line"
  else
    echo "fstab already contains: $line"
  fi
}

ensure_dir() {
  local d="$1"
  mkdir -p "$d"
}

require_root

echo "==> Preparing /data mount (optional)"
if [[ -n "$DATA_DEVICE" ]]; then
  if [[ $NO_FORMAT -eq 0 ]]; then
    TYPE=$(blkid -o value -s TYPE "$DATA_DEVICE" || true)
    if [[ "$TYPE" != "ext4" ]]; then
      echo "Formatting $DATA_DEVICE as ext4"
      mkfs.ext4 -F "$DATA_DEVICE"
    fi
  fi
  DATA_UUID=$(blkid -o value -s UUID "$DATA_DEVICE")
fi

if [[ -n "$DATA_UUID" ]]; then
  ensure_dir /data
  append_fstab "UUID=$DATA_UUID /data ext4 defaults,noatime 0 2"
  mount -a || true
fi

if mountpoint -q /data; then
  echo "/data is mounted"
else
  echo "WARNING: /data is not mounted. Proceeding, but playlists/logs will not persist."
fi

echo "==> Configuring tmpfs for volatile directories"
append_fstab "tmpfs /tmp tmpfs defaults,noatime,mode=1777 0 0"
append_fstab "tmpfs /var/tmp tmpfs defaults,noatime,mode=1777 0 0"
# Optional: keep journald volatile to protect media; can be changed if persistent logs needed
append_fstab "tmpfs /var/log tmpfs defaults,noatime,mode=0755 0 0"

echo "==> Enabling Raspberry Pi overlay filesystem (root overlay)"
if command -v raspi-config >/dev/null 2>&1; then
  raspi-config nonint enable_overlayfs
else
  echo "ERROR: raspi-config not found. Install raspberrypi-ui-mods or run on Raspberry Pi OS." >&2
  exit 1
fi

echo "==> Creating application data directories"
ensure_dir /data/playlists
ensure_dir /data/media
ensure_dir /data/logs

echo "==> Done. Review changes:"
echo "- /etc/fstab updated for /data and tmpfs mounts"
echo "- OverlayFS enabled via raspi-config (root becomes read-only lower with tmpfs upper)"
echo "- /data prepared for playlists/media/logs"
echo
echo "Reboot to apply overlay filesystem: sudo reboot"

