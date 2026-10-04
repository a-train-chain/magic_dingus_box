#!/usr/bin/env bash
#
# Phone Remote — /dev/uinput udev rule + `input` group membership.
#
# Grants the magic-dingus-web service user permission to open uinput so the
# phone can drive a virtual gamepad. Without this, UInput() raises EACCES.
#
# Factored out of setup_services.sh (section 4.4) so the OTA can deliver
# exactly this and nothing else: update.sh used to run the WHOLE of
# setup_services.sh to get here, unprivileged, and it died at Step 0 (it
# writes /etc) on every box — the rule never arrived and the bootstrap
# re-ran on every update. setup_services.sh must not run from an OTA anyway
# (it restarts the web service mid-update and starts Docker on games-only
# boxes).
#
# Does NOT restart magic-dingus-web: a newly added group only takes effect
# for a new process. setup_services.sh restarts the web service itself
# afterwards; update.sh restarts it at the end of every successful install.
#
# Usage: sudo setup_phone_remote_uinput.sh [service_user]
#   service_user defaults to magic-dingus-web.service's User=, else "magic".
# Idempotent. Must run as root.

set -euo pipefail

if [[ "$EUID" -ne 0 ]]; then
    echo "[phone-remote] ERROR: must run as root (sudo)" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UINPUT_RULES_SRC="${SCRIPT_DIR}/data/90-magicdingus-uinput.rules"

if [[ ! -f "$UINPUT_RULES_SRC" ]]; then
    echo "  ⚠ ${UINPUT_RULES_SRC} not found — skipping uinput rule install"
    exit 0
fi

echo "Installing /dev/uinput udev rule for Phone Remote..."
# Ensure the uinput kernel module is loaded NOW and on every boot.
# On vanilla Debian Bookworm it isn't auto-loaded; on Raspberry Pi OS
# it usually is, but persist it explicitly so udev triggers below have
# a device node to match against.
modprobe uinput || true
echo "uinput" > /etc/modules-load.d/uinput.conf
install -m 0644 "$UINPUT_RULES_SRC" /etc/udev/rules.d/90-magicdingus-uinput.rules
udevadm control --reload-rules
udevadm trigger --name-match=uinput || true

# Add the magic-dingus-web service user to the input group.
SERVICE_USER="${1:-}"
if [[ -z "$SERVICE_USER" ]]; then
    SERVICE_USER="$(systemctl show -p User --value magic-dingus-web.service 2>/dev/null || true)"
fi
SERVICE_USER="${SERVICE_USER:-magic}"  # fallback to "magic" if service not installed yet
if id -nG "$SERVICE_USER" 2>/dev/null | grep -qw input; then
    echo "  ✓ user $SERVICE_USER already in input group"
else
    usermod -a -G input "$SERVICE_USER"
    echo "  ✓ added $SERVICE_USER to input group (takes effect when magic-dingus-web restarts)"
fi
