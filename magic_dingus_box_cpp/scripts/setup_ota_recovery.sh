#!/bin/bash
#
# Magic Dingus Box — install the boot-time OTA power-loss recovery unit.
#
# Installs magic-dingus-ota-recovery.service (from ../systemd/ next to this
# script) into /etc/systemd/system and enables it. At boot it runs before
# the kiosk and the web admin, but only when update.sh left its
# "install in progress" marker behind (ConditionPathExists=), in which case
# it restores the pre-update backup. See update.sh (OTA_MARKER,
# recover_interrupted_update).
#
# Idempotent; safe to re-run any time. Callers:
#   - update.sh, at the start of every install, BEFORE it writes the marker
#     (unit files are otherwise never refreshed by an OTA, so the update.sh
#     that relies on the unit is the one that delivers it)
#   - deploy_cpp.sh (provisioning / developer deploys)
#   - scripts/golden_image/sync_source_box.sh (so a golden image carries it)
#   - scripts/golden_image/first_boot.sh (clones cut from older images)
#
# Test seams (same conventions as setup_memory_tuning.sh):
#   MAGIC_TUNING_ROOT    - prefix for /etc (BATS fake root; skips the root check)
#   MAGIC_SKIP_SYSTEMCTL - "true" skips daemon-reload / enable

set -euo pipefail

TUNING_ROOT="${MAGIC_TUNING_ROOT:-}"
SKIP_SYSTEMCTL="${MAGIC_SKIP_SYSTEMCTL:-false}"
UNIT="magic-dingus-ota-recovery.service"

log() { echo "[ota-recovery] $1"; }

if [[ -z "$TUNING_ROOT" && "$EUID" -ne 0 ]]; then
    echo "[ota-recovery] ERROR: must run as root (sudo)" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${SCRIPT_DIR}/../systemd/${UNIT}"
DEST="${TUNING_ROOT}/etc/systemd/system/${UNIT}"

if [[ ! -f "$SRC" ]]; then
    echo "[ota-recovery] ERROR: unit source not found: $SRC" >&2
    exit 1
fi

changed=0
if [[ ! -f "$DEST" ]] || ! cmp -s "$SRC" "$DEST"; then
    mkdir -p "$(dirname "$DEST")"
    install -m 0644 "$SRC" "$DEST"
    changed=1
    log "installed $DEST"
else
    log "$UNIT already current"
fi

if [[ "$SKIP_SYSTEMCTL" == "true" ]]; then
    log "SKIP: daemon-reload / enable (test mode)"
    exit 0
fi

if [[ "$changed" -eq 1 ]]; then
    systemctl daemon-reload
fi
if ! systemctl is-enabled --quiet "$UNIT" 2>/dev/null; then
    systemctl enable "$UNIT" >/dev/null 2>&1
    log "enabled $UNIT"
fi
