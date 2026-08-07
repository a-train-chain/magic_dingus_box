#!/bin/bash
#
# Magic Dingus Box — Network Doctor.
#
# Runs a ladder of connectivity probes and, crucially, turns the failure
# PATTERN into one plain-English verdict a customer can read on their
# phone. Born from a field case where a single hostile router produced
# four unrelated-looking symptoms ("Radarr offline", no posters, update
# CHECK FAILED, VPN setup failing) and days of remote guessing — while
# the answer was one sentence: "your router blocks this device's
# internet; local network is fine."
#
# Usage:
#   network_doctor.sh          # human-readable
#   network_doctor.sh --json   # machine output for the Content Manager
#
# Read-only, no root needed, ~10-20 s worst case (every probe bounded).

set -uo pipefail   # deliberately NOT -e: a failed probe is a RESULT here

JSON=0
[[ "${1:-}" == "--json" ]] && JSON=1

declare -a NAMES OKS DETAILS

add() {  # add <name> <ok:0|1> <detail>
    NAMES+=("$1"); OKS+=("$2"); DETAILS+=("$3")
}

# --- rung 1: interface + address -------------------------------------------
EGRESS_IF=$(ip -4 route show default 2>/dev/null | awk '{print $5; exit}')
GATEWAY=$(ip -4 route show default 2>/dev/null | awk '{print $3; exit}')
IPADDR=""
if [[ -n "$EGRESS_IF" ]]; then
    IPADDR=$(ip -4 -o addr show dev "$EGRESS_IF" 2>/dev/null | awk '{print $4; exit}')
    SSID=$(iw dev "$EGRESS_IF" link 2>/dev/null | awk -F': ' '/SSID/{print $2; exit}')
    add "network_joined" 1 "${EGRESS_IF}${SSID:+ (\"$SSID\")} at ${IPADDR:-?}"
else
    add "network_joined" 0 "no default route — not on any network"
fi

# 169.254.x.x means DHCP failed: joined the Wi-Fi but got no real address.
if [[ "$IPADDR" == 169.254.* ]]; then
    add "dhcp" 0 "self-assigned address — router gave no DHCP lease"
else
    [[ -n "$IPADDR" ]] && add "dhcp" 1 "lease OK"
fi

# --- rung 2: gateway reachable ----------------------------------------------
if [[ -n "$GATEWAY" ]] && ping -c1 -W2 "$GATEWAY" >/dev/null 2>&1; then
    add "router_reachable" 1 "$GATEWAY answers"
elif [[ -n "$GATEWAY" ]]; then
    # Plenty of routers drop ICMP; try TCP to its web UI before condemning.
    if timeout 2 bash -c "exec 3<>/dev/tcp/${GATEWAY}/80" 2>/dev/null; then
        add "router_reachable" 1 "$GATEWAY answers (web port; ping filtered)"
    else
        add "router_reachable" 0 "$GATEWAY not responding"
    fi
else
    add "router_reachable" 0 "no gateway known"
fi

# --- rung 3: IPv6 posture ----------------------------------------------------
if [[ -n "$EGRESS_IF" ]]; then
    v6=$(ip -6 addr show dev "$EGRESS_IF" scope global 2>/dev/null | grep -c inet6)
    if [[ "${v6:-0}" -eq 0 ]]; then
        add "ipv6_posture" 1 "no global IPv6 (correct — v6 black-holes can't trap this box)"
    else
        add "ipv6_posture" 0 "global IPv6 active on ${EGRESS_IF} — a broken-v6 router can black-hole DNS/HTTPS; run setup_network_hardening.sh"
    fi
fi

# --- rung 4: DNS via configured resolvers ------------------------------------
if getent hosts api.themoviedb.org >/dev/null 2>&1; then
    add "dns_system" 1 "system resolvers answer"
else
    add "dns_system" 0 "system resolvers cannot resolve names"
fi

# --- rung 5: DNS direct to 1.1.1.1 (bypasses the router entirely) ------------
dns_direct=$(python3 - <<'PYEOF' 2>/dev/null
import socket, sys
name = "api.themoviedb.org"
q = b"\x12\x34\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00"
q += b"".join(bytes([len(p)]) + p.encode() for p in name.split("."))
q += b"\x00\x00\x01\x00\x01"
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(3)
try:
    s.sendto(q, ("1.1.1.1", 53))
    d = s.recv(512)
    print("ok" if len(d) > 12 and (d[3] & 0x0F) == 0 else "servfail")
except Exception:
    print("blocked")
PYEOF
)
case "$dns_direct" in
    ok)       add "dns_direct" 1 "1.1.1.1 answers directly" ;;
    servfail) add "dns_direct" 0 "1.1.1.1 reachable but query failed" ;;
    *)        add "dns_direct" 0 "cannot reach 1.1.1.1 — outbound DNS blocked for this device" ;;
esac

# --- rung 6: HTTPS over IPv4 (the path posters + updates actually use) -------
http_rc=$(curl -4 -sS -o /dev/null -w '%{http_code}' -m 8 https://api.themoviedb.org/3 2>/dev/null)
if [[ -n "$http_rc" && "$http_rc" != "000" ]]; then
    add "https_tmdb" 1 "TMDB reachable (HTTP ${http_rc})"
else
    add "https_tmdb" 0 "cannot reach TMDB over HTTPS"
fi
gh_rc=$(curl -4 -sS -o /dev/null -w '%{http_code}' -m 8 https://api.github.com 2>/dev/null)
if [[ -n "$gh_rc" && "$gh_rc" != "000" ]]; then
    add "https_github" 1 "GitHub reachable (HTTP ${gh_rc}) — updates can download"
else
    add "https_github" 0 "cannot reach GitHub — updates cannot download"
fi

# --- rung 7: VPN tunnel (only meaningful on a provisioned box) ---------------
if [[ -f /opt/magic_dingus_box/services/.env ]]; then
    ping_rc=$(curl -sS -o /dev/null -w '%{http_code}' -m 5 http://localhost:7878/ping 2>/dev/null)
    if [[ "$ping_rc" == "200" ]]; then
        add "vpn_stack" 1 "download stack up (Radarr answers through the tunnel)"
    else
        add "vpn_stack" 0 "download stack not answering (tunnel or containers down)"
    fi
else
    add "vpn_stack" 1 "not provisioned yet (nothing to check)"
fi

# --- verdict synthesis -------------------------------------------------------
get() {  # get <name> -> 0|1|missing
    local i
    for i in "${!NAMES[@]}"; do
        [[ "${NAMES[$i]}" == "$1" ]] && { echo "${OKS[$i]}"; return; }
    done
    echo "missing"
}

VERDICT="unknown"; ADVICE=""
if [[ "$(get network_joined)" == "0" ]]; then
    VERDICT="No network"
    ADVICE="The box is not on any Wi-Fi. Join one from the TV: Settings button -> Wi-Fi."
elif [[ "$(get dhcp)" == "0" ]]; then
    VERDICT="Router gave no address"
    ADVICE="The Wi-Fi password was accepted but the router issued no address. Reboot the router, then the box."
elif [[ "$(get router_reachable)" == "0" ]]; then
    VERDICT="Router unreachable"
    ADVICE="Joined the Wi-Fi but the router is not answering. Reboot the router; if it persists, the router may isolate this device."
elif [[ "$(get https_tmdb)" == "1" && "$(get https_github)" == "1" ]]; then
    if [[ "$(get vpn_stack)" == "0" ]]; then
        VERDICT="Internet OK — download stack down"
        ADVICE="The network is fine. The VPN/download side needs attention: reboot the box; if it persists, reconfigure the VPN in the Content Manager."
    else
        VERDICT="All good"
        ADVICE="Network, DNS, internet and services all check out."
    fi
elif [[ "$(get dns_system)" == "0" && "$(get dns_direct)" == "0" ]]; then
    VERDICT="Router is blocking this device's internet"
    ADVICE="Local network works but nothing reaches the internet — typical of router 'security'/parental features silently blocking an unrecognized device. In the router app, find this device (magicpi-...) and allow it; also try disabling IPv6 in the router."
elif [[ "$(get dns_system)" == "0" && "$(get dns_direct)" == "1" ]]; then
    VERDICT="Router DNS broken (working around it)"
    ADVICE="The router's own DNS is dead but public DNS works. The box's fallback should cover this; if things are still slow, set the router's DNS to 1.1.1.1."
else
    VERDICT="Partial internet blockage"
    ADVICE="DNS works but web traffic is blocked or IPv6 is interfering. Check router security features for this device, and disable IPv6 in the router."
fi

# --- output ------------------------------------------------------------------
if [[ "$JSON" -eq 1 ]]; then
    python3 - "$VERDICT" "$ADVICE" <<'PYEOF' "${NAMES[@]}" --- "${OKS[@]}" --- "${DETAILS[@]}"
import json, sys
verdict, advice = sys.argv[1], sys.argv[2]
rest = sys.argv[3:]
seps = [i for i, a in enumerate(rest) if a == "---"]
names, oks, details = rest[:seps[0]], rest[seps[0]+1:seps[1]], rest[seps[1]+1:]
checks = [{"name": n, "ok": o == "1", "detail": d}
          for n, o, d in zip(names, oks, details)]
print(json.dumps({"ok": True, "verdict": verdict, "advice": advice,
                  "checks": checks}))
PYEOF
else
    echo "=== Magic Dingus Box Network Doctor ==="
    for i in "${!NAMES[@]}"; do
        [[ "${OKS[$i]}" == "1" ]] && mark="PASS" || mark="FAIL"
        printf '  [%s] %-16s %s\n' "$mark" "${NAMES[$i]}" "${DETAILS[$i]}"
    done
    echo
    echo "VERDICT: $VERDICT"
    echo "         $ADVICE"
fi

# exit 0 when nothing failed, 1 otherwise (usable from scripts/CI)
for o in "${OKS[@]}"; do [[ "$o" == "0" ]] && exit 1; done
exit 0
