#!/usr/bin/env bash
# Smoke-check: every torrent-ecosystem container exits via the VPN.
#
# Run on the Pi. Compares each container's reported public IP against
# Gluetun's reported public IP. Any container with a different exit IP
# is leaking. Also verifies host-level: IPv6 disabled and DNS bypassing
# the ISP resolver (1.1.1.1).
#
# Exit codes:
#   0 — all checks pass
#   1 — one or more containers leak
#   2 — host-level networking misconfigured
#   3 — required tools missing on the Pi

set -uo pipefail

EXPECTED_VPN_CONTAINERS=(mdb_gluetun mdb_qbittorrent mdb_radarr mdb_sonarr mdb_prowlarr mdb_byparr)

# 1. Tooling checks
for tool in docker dig curl jq; do
    if ! command -v "${tool}" &>/dev/null; then
        echo "ERROR: ${tool} not installed on this Pi."
        exit 3
    fi
done

# 2. Get gluetun's reported VPN exit IP
GLUETUN_IP=$(docker exec mdb_gluetun wget -qO- --timeout=5 \
    http://localhost:8000/v1/publicip/ip 2>/dev/null \
    | jq -r .public_ip 2>/dev/null || echo "")
if [ -z "${GLUETUN_IP}" ]; then
    echo "ERROR: cannot read gluetun's exit IP. Is the tunnel up?"
    echo "       Try: docker logs mdb_gluetun"
    exit 1
fi
echo "Gluetun reports VPN exit IP: ${GLUETUN_IP}"

# 3. Per-container exit IP check
LEAKS=0
for container in "${EXPECTED_VPN_CONTAINERS[@]}"; do
    if ! docker inspect "${container}" >/dev/null 2>&1; then
        echo "  ${container}: NOT RUNNING (skipped)"
        continue
    fi
    # ifconfig.me/ip returns a plain IP string. We curl from inside
    # the container; if the container shares gluetun's netns, this
    # exits via the VPN too.
    CONTAINER_IP=$(docker exec "${container}" sh -c \
        'curl -fsS --max-time 8 ifconfig.me/ip 2>/dev/null || \
         wget -qO- --timeout=8 ifconfig.me/ip 2>/dev/null' \
        | tr -d '[:space:]')
    if [ "${CONTAINER_IP}" = "${GLUETUN_IP}" ]; then
        echo "  ${container}: exits via VPN ✓"
    else
        echo "  ${container}: LEAK — exits as ${CONTAINER_IP:-unknown}"
        LEAKS=$((LEAKS + 1))
    fi
done

# 4. Host-level: IPv6 disabled — checked on the ACTIVE EGRESS INTERFACE,
# not the sysctl. The old test read conf/all/disable_ipv6, which
# setup once set to 1 and never changes — but NetworkManager re-enables
# v6 per-interface for any ipv6.method=auto profile, so wlan0 carried a
# live global v6 address while this check printed a green tick: a false
# green no unit could ever fail. The address on the interface is the
# outcome; assert that. (Posture is enforced by setup_network_hardening.sh
# + the NM dispatcher.)
EGRESS_IF=$(ip -4 route show default 2>/dev/null | awk '{print $5; exit}')
if [ -n "$EGRESS_IF" ]; then
    V6_COUNT=$(ip -6 addr show dev "$EGRESS_IF" scope global 2>/dev/null | grep -c inet6)
    if [ "${V6_COUNT:-0}" = "0" ]; then
        echo "  host: no global IPv6 on egress (${EGRESS_IF}) ✓"
    else
        echo "  host: WARNING — ${EGRESS_IF} holds ${V6_COUNT} global IPv6 address(es)."
        echo "         Run: sudo scripts/setup_network_hardening.sh (then reconnect)"
        LEAKS=$((LEAKS + 1))
    fi
else
    echo "  host: WARNING — no default route; cannot assess IPv6 posture"
    LEAKS=$((LEAKS + 1))
fi

# 5. Host-level: DNS bypasses the ISP resolver (resolv.conf points at 1.1.1.1)
if grep -qE "^nameserver 1\.(1\.1\.1|0\.0\.1)\b" /etc/resolv.conf 2>/dev/null; then
    echo "  host: DNS bypasses ISP resolver (1.1.1.1) ✓"
else
    echo "  host: WARNING — /etc/resolv.conf does not point at 1.1.1.1."
    echo "         Current nameserver(s):"
    grep "^nameserver" /etc/resolv.conf 2>/dev/null | sed 's/^/         /'
    LEAKS=$((LEAKS + 1))
fi

if [ "${LEAKS}" -gt 0 ]; then
    echo "FAIL: ${LEAKS} leak(s) or misconfig(s) detected."
    exit 1
fi
echo "OK: all torrent-ecosystem containers exit via VPN; host correctly configured."
exit 0
