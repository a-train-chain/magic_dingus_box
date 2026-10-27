#!/bin/bash
#
# Magic Dingus Box — the USB-C cable's DHCP + DNS (http://dingus.box).
#
# The current design, end to end:
#   usb-gadget-network.service      puts 10.55.0.1/24 on usb0 (deploy_cpp.sh
#                                   Step 1.66 + the unit converge install it)
#   dnsmasq + /etc/dnsmasq.d/usb0.conf
#                                   DHCP 10.55.0.10-100 on usb0 and the
#                                   catch-all DNS that makes ANY name a
#                                   cabled laptop types — dingus.box — answer
#                                   10.55.0.1 (scripts/data/dnsmasq-usb0.conf)
#   content-manager-redirect.service
#                                   port 80 -> :5000, so the bare name lands
#                                   on the Content Manager
#
# Modes:
#   (no args)         install the dnsmasq conf + the redirect and (re)start
#                     both, then retire the legacy networkd config below.
#                     Called by install_deps.sh and deploy_cpp.sh --usb-gadget.
#   --retire-legacy   ONLY retire the legacy networkd config. Called by
#                     setup_memory_tuning.sh, the root hook every delivery
#                     path runs (deploy, OTA — including the OLD update.sh's
#                     first hop — first boot, source-box sync), so field boxes
#                     heal on their next update. Never provisions anything and
#                     never fails.
#
# The legacy config: until 2026-10, deploy_cpp.sh --usb-gadget wrote
# /etc/systemd/network/10-usb-gadget.network (Address=192.168.7.1/24,
# DHCPServer=yes, EmitDNS=no) and enabled systemd-networkd. A box that ever
# ran it keeps a SECOND DHCP server on usb0 that hands laptops a 192.168.7.x
# lease with no DNS server, so dingus.box never resolves — found live on a
# Pi 4B on 2026-10-27, where removing that one file fixed it. It is moved
# (never deleted) to /var/lib/magic-dingus/retired/. systemd-networkd itself
# is left enabled: with no .network file matching usb0 it does nothing there,
# and an operator may use it for something else.
#
# Test seams (mirroring setup_memory_tuning.sh):
#   MAGIC_USB_ROOT        - prefix for /etc and /var paths (BATS fake root;
#                           also skips the EUID root check)
#   MAGIC_SKIP_SYSTEMCTL  - "true" skips systemctl / networkctl / ip

set -uo pipefail

USB_ROOT="${MAGIC_USB_ROOT:-}"
SKIP_SYSTEMCTL="${MAGIC_SKIP_SYSTEMCTL:-false}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

log() { echo "[usb-gadget-dns] $1"; }

MODE=install
case "${1:-}" in
    "")               ;;
    --retire-legacy)  MODE=retire ;;
    *) echo "usage: $0 [--retire-legacy]" >&2; exit 2 ;;
esac

if [[ -z "$USB_ROOT" && "$EUID" -ne 0 ]]; then
    echo "[usb-gadget-dns] ERROR: must run as root (sudo)" >&2
    exit 1
fi

LEGACY_NETWORK="${USB_ROOT}/etc/systemd/network/10-usb-gadget.network"
RETIRED_DIR="${USB_ROOT}/var/lib/magic-dingus/retired"
LEGACY_ADDR="192.168.7.1/24"
DNSMASQ_SRC="${SCRIPT_DIR}/data/dnsmasq-usb0.conf"
DNSMASQ_DEST="${USB_ROOT}/etc/dnsmasq.d/usb0.conf"

# Move the legacy networkd file aside and, on a live box, stop what it
# started. Returns 0 whether or not there was anything to retire.
retire_legacy_network() {
    if [[ ! -f "$LEGACY_NETWORK" ]]; then
        return 0
    fi
    # Only the file that manages usb0 is ours to move: the name is the one
    # the old deploy wrote, the match keeps an unrelated file by the same
    # name (hand-written for another interface) where it is.
    if ! grep -qE '^[[:space:]]*Name=usb0[[:space:]]*$' "$LEGACY_NETWORK"; then
        log "$LEGACY_NETWORK does not match usb0; leaving it alone"
        return 0
    fi
    local backup
    backup="${RETIRED_DIR}/10-usb-gadget.network.$(date +%Y%m%d%H%M%S)"
    if ! install -d -m 0755 "$RETIRED_DIR" || ! mv "$LEGACY_NETWORK" "$backup"; then
        log "WARNING: could not move $LEGACY_NETWORK aside (laptops on the cable may still get 192.168.7.x and no dingus.box)"
        return 0
    fi
    log "retired legacy usb0 DHCP server: $LEGACY_NETWORK -> $backup"

    if [[ "$SKIP_SYSTEMCTL" == "true" ]]; then
        log "SKIP: networkd reload + usb0 cleanup (test mode)"
        return 0
    fi
    # networkd stops serving DHCP on a link it no longer has a .network for
    # once it reloads. --no-block on every systemctl: this also runs at boot
    # (first_boot.sh -> setup_memory_tuning.sh).
    if systemctl is-active --quiet systemd-networkd.service 2>/dev/null; then
        networkctl reload 2>/dev/null \
            || log "WARNING: networkctl reload failed (the old DHCP server stops at the next reboot)"
    fi
    if [[ -e /sys/class/net/usb0 ]]; then
        ip addr del "$LEGACY_ADDR" dev usb0 2>/dev/null || true
        if ! ip -4 addr show dev usb0 2>/dev/null | grep -q 'inet 10\.55\.0\.1/'; then
            systemctl --no-block restart usb-gadget-network.service 2>/dev/null || true
        fi
    fi
    # dnsmasq may have lost usb0's DHCP port to networkd; rebind it.
    systemctl --no-block try-restart dnsmasq.service 2>/dev/null || true
    return 0
}

if [[ "$MODE" == retire ]]; then
    retire_legacy_network
    exit 0
fi

# --- install mode -----------------------------------------------------------
RC=0

if [[ ! -f "$DNSMASQ_SRC" ]]; then
    log "ERROR: ${DNSMASQ_SRC} not found — cannot configure usb0 DHCP/DNS"
    RC=1
elif install -d -m 0755 "${DNSMASQ_DEST%/*}" \
    && install -m 0644 "$DNSMASQ_SRC" "$DNSMASQ_DEST"; then
    log "installed ${DNSMASQ_DEST}"
else
    log "ERROR: could not write ${DNSMASQ_DEST}"
    RC=1
fi

# The old networkd DHCP server must be gone BEFORE dnsmasq restarts, or the
# two race for usb0's DHCP port again.
retire_legacy_network

if [[ "$SKIP_SYSTEMCTL" == "true" ]]; then
    log "SKIP: dnsmasq + content-manager-redirect enable/restart (test mode)"
else
    if ! systemctl cat dnsmasq.service >/dev/null 2>&1; then
        log "ERROR: dnsmasq is not installed (run magic_dingus_box_cpp/scripts/install_deps.sh)"
        RC=1
    elif systemctl enable dnsmasq.service 2>/dev/null \
        && systemctl restart dnsmasq.service; then
        log "dnsmasq serving DHCP + dingus.box DNS on usb0"
    else
        log "ERROR: dnsmasq did not start (journalctl -u dnsmasq)"
        RC=1
    fi

    # The unit goes through converge_system_units.sh --only, the one writer
    # of unit files (it daemon-reloads when it wrote).
    if bash "${SCRIPT_DIR}/converge_system_units.sh" --only content-manager-redirect.service \
        && systemctl enable content-manager-redirect.service 2>/dev/null \
        && systemctl restart content-manager-redirect.service; then
        log "port-80 -> :5000 redirect running"
    else
        log "ERROR: content-manager-redirect.service did not start"
        RC=1
    fi
fi

exit "$RC"
