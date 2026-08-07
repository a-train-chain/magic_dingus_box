#!/bin/bash
#
# Magic Dingus Box — install the any-Wi-Fi network posture.
#
# One idempotent entry point, called from deploy_cpp.sh (base platform —
# a games-only box needs this exactly as much as a Movies box) and safe to
# re-run any time. Installs:
#   1. /etc/NetworkManager/conf.d/90-magic-ipv6-disable.conf
#        -> new connections default to ipv6.method=disabled
#   2. /etc/NetworkManager/dispatcher.d/90-magic-network-hardening
#        -> every connection that comes up is repaired in place
#           (ipv6 off + public-DNS-first), covering profiles that
#           predate this posture or set ipv6.method explicitly
# and then applies the same repair to every EXISTING wifi/ethernet
# profile immediately, without dropping the active connection (runtime
# changes go through `nmcli device modify`, which reapplies in place).
#
# Field history: the first customer install (2026-08-05) sat on a router
# whose broken IPv6 black-holed all host DNS/HTTPS — no posters, no
# update checks, VPN re-setup failing — while the v4-pinned WireGuard
# tunnel worked, scattering four misleading symptoms across days of
# remote debugging. This file is why that never happens again.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="${SCRIPT_DIR}/data"

log() { echo "[net-hardening] $1"; }

if [[ "$EUID" -ne 0 ]]; then
    echo "[net-hardening] ERROR: must run as root (sudo)" >&2
    exit 1
fi

# --- 1. conf.d default ------------------------------------------------------
install -m 0644 "${DATA_DIR}/90-magic-ipv6-disable.conf" \
    /etc/NetworkManager/conf.d/90-magic-ipv6-disable.conf
log "conf.d default installed (new connections: ipv6 disabled)"

# --- 2. dispatcher ----------------------------------------------------------
install -m 0755 "${DATA_DIR}/90-magic-network-hardening" \
    /etc/NetworkManager/dispatcher.d/90-magic-network-hardening
log "dispatcher installed (self-heals every connection on up)"

# conf.d changes need NM to re-read config; reload is non-disruptive
# (it does not touch active connections).
nmcli general reload conf 2>/dev/null || true

# --- 3. repair existing profiles NOW ----------------------------------------
# Same logic as the dispatcher, applied to every wifi/ethernet profile so
# the posture is correct before the next reconnect, and to the ACTIVE
# device state so it is correct right now. `nmcli device modify` is a
# reapply-in-place: the link (and any SSH session over it) stays up.
WANT_DNS="1.1.1.1 8.8.8.8"
fixed=0
while IFS=: read -r name uuid ctype device; do
    case "$ctype" in
        802-11-wireless|802-3-ethernet) ;;
        *) continue ;;
    esac
    cur_v6=$(nmcli -g ipv6.method connection show "$uuid" 2>/dev/null || true)
    cur_dns=$(nmcli -g ipv4.dns connection show "$uuid" 2>/dev/null || true)
    changed=0
    if [[ "$cur_v6" != "disabled" ]]; then
        nmcli connection modify "$uuid" ipv6.method disabled && changed=1
    fi
    if [[ "$cur_dns" != "1.1.1.1,8.8.8.8" ]]; then
        nmcli connection modify "$uuid" \
            ipv4.dns "$WANT_DNS" ipv4.ignore-auto-dns no && changed=1
    fi
    if [[ "$changed" -eq 1 ]]; then
        fixed=$((fixed + 1))
        log "repaired profile: ${name}"
        # If this profile is live on a device right now, fix the runtime too.
        if [[ -n "$device" && "$device" != "--" ]]; then
            nmcli device modify "$device" ipv6.method disabled 2>/dev/null || true
            nmcli device modify "$device" ipv4.dns "$WANT_DNS" 2>/dev/null || true
            log "applied live to ${device} (reapply in place, link stays up)"
        fi
    fi
done < <(nmcli -t -f NAME,UUID,TYPE,DEVICE connection show 2>/dev/null)

log "done: ${fixed} existing profile(s) repaired"

# --- 4. verify the outcome, not the intent ----------------------------------
# Egress interface = whatever holds the default route. A global v6 address
# there means the posture did NOT take (the exact false-green the old
# sysctl check hid); report loudly but exit 0 — the dispatcher will repair
# on the next reconnect, and callers (deploy) must not die here.
EGRESS_IF=$(ip -4 route show default 2>/dev/null | awk '{print $5; exit}')
if [[ -n "$EGRESS_IF" ]]; then
    v6_count=$(ip -6 addr show dev "$EGRESS_IF" scope global 2>/dev/null | grep -c "inet6" || true)
    if [[ "${v6_count:-0}" -eq 0 ]]; then
        log "verified: no global IPv6 on egress interface ${EGRESS_IF}"
    else
        log "WARNING: ${EGRESS_IF} still holds ${v6_count} global IPv6 address(es);"
        log "         the dispatcher will clear it on the next reconnect/reboot"
    fi
    if grep -qE "^nameserver 1\.1\.1\.1" /etc/resolv.conf 2>/dev/null; then
        log "verified: public DNS first in resolv.conf"
    else
        log "NOTE: resolv.conf not yet showing 1.1.1.1 first (applies on reconnect)"
    fi
fi
