#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# The compose header promises every image is pinned, but gluetun rode the
# floating `qmcgaw/gluetun:v3` tag until 2026-10 — every fresh pull could
# move the box onto a new gluetun, and the whole Media Browser stack shares
# its network namespace. Pinned means a digest, or at least a
# major.minor[.patch] version tag (never `latest` or a bare major).

COMPOSE="$CPP_DIR/services/docker-compose.yml"

@test "every compose image is pinned" {
    run grep -E '^[[:space:]]+image:' "$COMPOSE"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -ge 6 ]
    for line in "${lines[@]}"; do
        img="${line#*image:}"
        img="${img// /}"
        if [[ "$img" == *@sha256:* ]]; then
            continue
        fi
        [[ "$img" =~ :v?[0-9]+\.[0-9]+ ]] \
            || { echo "unpinned image: $img" >&2; false; }
    done
}

@test "gluetun is pinned to the production build by digest" {
    # v3.41.3: v3.41.1's port-forwarding loop never re-acquired a port after
    # Proton's NAT-PMP gateway refused a renewal, and every such loss cost
    # a full-stack restart (9 in two hours on the owner's box, 2026-10-05).
    grep -q 'image: qmcgaw/gluetun:v3.41.3@sha256:fa19cc76b2af13d57a8d3dc3066f2ada061b1c761b8aecf989b3877c0486e027' "$COMPOSE"
}

@test "gluetun's healthcheck still requires the forwarded port once one was seen" {
    # The backstop if a gluetun release ever loses the port for good again.
    grep -q '/tmp/.pf_seen' "$COMPOSE"
}

@test "WireGuard keepalive is on, as a Go duration (Proton configs ask for 25)" {
    run python3 -c "
import yaml
env=yaml.safe_load(open('$COMPOSE'))['services']['gluetun']['environment']
vals=[e.split('=',1)[1] for e in env if e.startswith('WIREGUARD_PERSISTENT_KEEPALIVE_INTERVAL=')]
assert vals == ['25s'], vals
"
    [ "$status" -eq 0 ] || { echo "$output"; false; }
}

@test "VPN country comes from services/.env, and stays blank-able for custom" {
    # The Content Manager's country picker rewrites VPN_COUNTRIES; `-` (not
    # `:-`) so the empty value admin.py writes for `custom` stays empty.
    grep -q -- '- SERVER_COUNTRIES=${VPN_COUNTRIES-}$' "$COMPOSE"
}

@test "OOM ranking: Byparr dies first, then qBittorrent; no hard mem_limits" {
    # Measured 2026-10-03: Byparr peaks at 762 MB in a challenge, qBit's
    # 1 GB peak is page cache — caps would break them, ranking doesn't.
    run python3 -c "
import yaml,sys
s=yaml.safe_load(open('$CPP_DIR/services/docker-compose.yml'))['services']
assert s['byparr'].get('oom_score_adj',0) > s['qbittorrent'].get('oom_score_adj',0) > 0, 'ranking'
assert not any('mem_limit' in v for v in s.values()), 'mem_limit present'
"
    [ "$status" -eq 0 ] || { echo "$output"; false; }
}
