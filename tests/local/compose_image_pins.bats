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

@test "Gluetun runs an init process to reap orphaned VPN probes" {
    python3 - "$COMPOSE" <<'PYTHON'
import sys, yaml
with open(sys.argv[1]) as compose:
    assert yaml.safe_load(compose)['services']['gluetun'].get('init') is True
PYTHON
}

@test "native Proton startup uses traffic and unsupported modes keep forwarding latch" {
    run python3 -c "
import yaml
check=yaml.safe_load(open('$COMPOSE'))['services']['gluetun']['healthcheck']['test'][1]
assert 'VPN_SERVICE_PROVIDER' in check and '= protonvpn' in check
assert 'VPN_TYPE' in check and '= wireguard' in check
assert 'VPN_PORT_FORWARDING' in check and '= on' in check
assert 'https://one.one.one.one/cdn-cgi/trace' in check
assert 'https://www.google.com/generate_204' in check
assert '/tmp/.pf_seen' in check
"
    [ "$status" -eq 0 ] || { echo "$output"; false; }
}

@test "Gluetun's own health restart is off exactly where the native controller owns recovery" {
    # Gluetun v3.41.3 treats a stop that cancels its in-flight startup check
    # as a health failure and restarts again. With its own restart on, the
    # controller's server switch (stop + start) landing while Gluetun
    # restart-loops on a dead server started two restart sequences that
    # cancelled each other: 236 sub-second restarts, no port forwarding, a
    # wedged VPN loop (magicpi5 2026-11-15, 4 of 4 fallback runs). Run the
    # real entrypoint wrapper with a stub Gluetun that prints its setting.
    run python3 -c "
import os, subprocess, tempfile, yaml
svc = yaml.safe_load(open('$COMPOSE'))['services']['gluetun']
entry = svc['entrypoint']
assert entry[:2] == ['/bin/sh', '-c'], entry
script = entry[2].replace('\$\$', '\$')
assert 'exec /gluetun-entrypoint' in script, script
# The same three-clause condition as the healthcheck's native branch.
check = svc['healthcheck']['test'][1].replace('\$\$', '\$')
native = '[ \"\$VPN_SERVICE_PROVIDER\" = protonvpn ] && [ \"\$VPN_TYPE\" = wireguard ] && [ \"\$VPN_PORT_FORWARDING\" = on ]'
assert native in check and native in script, (check, script)
stub = tempfile.mkdtemp()
probe = os.path.join(stub, 'gluetun')
open(probe, 'w').write('#!/bin/sh\necho \"restart=\${HEALTH_RESTART_VPN-unset} args=\$*\"\n')
os.chmod(probe, 0o755)
script = script.replace('/gluetun-entrypoint', probe)
cases = {('protonvpn', 'wireguard', 'on'): 'restart=off',
         ('custom', 'wireguard', 'on'): 'restart=unset',
         ('protonvpn', 'openvpn', 'on'): 'restart=unset',
         ('protonvpn', 'wireguard', 'off'): 'restart=unset'}
for (provider, kind, forwarding), want in cases.items():
    env = {'PATH': '/usr/bin:/bin', 'VPN_SERVICE_PROVIDER': provider, 'VPN_TYPE': kind,
           'VPN_PORT_FORWARDING': forwarding}
    out = subprocess.run(['/bin/sh', '-c', script] + entry[3:], env=env,
                         capture_output=True, text=True, check=True).stdout.strip()
    assert out == want + ' args=', (provider, kind, forwarding, out)
# An operator's explicit HEALTH_RESTART_VPN is never overridden off-native.
env = {'PATH': '/usr/bin:/bin', 'VPN_SERVICE_PROVIDER': 'custom', 'VPN_TYPE': 'wireguard',
       'VPN_PORT_FORWARDING': 'off', 'HEALTH_RESTART_VPN': 'on'}
out = subprocess.run(['/bin/sh', '-c', script] + entry[3:], env=env, capture_output=True, text=True, check=True).stdout
assert out.startswith('restart=on'), out
assert not any(e.startswith('HEALTH_RESTART_VPN=') for e in svc['environment']), 'set it only in the wrapper'
"
    [ "$status" -eq 0 ] || { echo "$output"; false; }
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
