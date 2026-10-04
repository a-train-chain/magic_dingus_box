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
    grep -q 'image: qmcgaw/gluetun:v3.41.1@sha256:1a5bf4b4820a879cdf8d93d7ef0d2d963af56670c9ebff8981860b6804ebc8ab' "$COMPOSE"
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
