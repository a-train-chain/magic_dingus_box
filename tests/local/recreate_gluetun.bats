#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# recreate_gluetun.sh applies the Content Manager's VPN-country change:
# recreate ONLY gluetun (compose up -d --no-deps, never --force-recreate),
# under the shared compose lock, released BEFORE waiting so the cascade
# watcher can re-link the dependents; then wait for healthy. Runs against a
# stub docker that logs every call.

setup() {
    SCRIPT="$CPP_DIR/scripts/recreate_gluetun.sh"
    export COMPOSE_DIR="$BATS_TEST_TMPDIR/services"
    export MDB_COMPOSE_LOCK="$BATS_TEST_TMPDIR/compose.lock"
    export DOCKER_LOG="$BATS_TEST_TMPDIR/docker.log"
    export POLL_S=0 RECREATE_WAIT_S=3
    mkdir -p "$COMPOSE_DIR" "$BATS_TEST_TMPDIR/bin"
    echo "services: {}" > "$COMPOSE_DIR/docker-compose.yml"
    printf 'WIREGUARD_PRIVATE_KEY=x\nVPN_COUNTRIES="United States"\n' > "$COMPOSE_DIR/.env"
    cat > "$BATS_TEST_TMPDIR/bin/docker" <<'EOF'
#!/bin/bash
echo "$*" >> "$DOCKER_LOG"
case "$1" in
    compose) exit "${COMPOSE_RC:-0}" ;;
    inspect) echo "${HEALTH:-healthy}" ;;
    exec)    echo '{"public_ip":"203.0.113.9","country":"United States"}' ;;
esac
exit 0
EOF
    chmod +x "$BATS_TEST_TMPDIR/bin/docker"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
}

@test "recreates only gluetun, without --force-recreate, and reports the exit country" {
    run bash "$SCRIPT"
    [ "$status" -eq 0 ] || { echo "$output"; false; }
    grep -q "^compose -f $COMPOSE_DIR/docker-compose.yml up -d --no-deps gluetun$" "$DOCKER_LOG"
    ! grep -q -- "--force-recreate" "$DOCKER_LOG"
    ! grep -qE "compose .*(radarr|sonarr|prowlarr|qbittorrent|byparr)" "$DOCKER_LOG"
    [[ "$output" == *"to a server in United States"* ]]
    [[ "$output" == *"exit country: United States"* ]]
}

@test "a compose failure exits 1 and never waits" {
    COMPOSE_RC=1 run bash "$SCRIPT"
    [ "$status" -eq 1 ]
    [[ "$output" == *"FAILED to recreate"* ]]
    ! grep -q "^inspect" "$DOCKER_LOG"
}

@test "not healthy in time exits 1 with a plain message" {
    HEALTH=starting run bash "$SCRIPT"
    [ "$status" -eq 1 ]
    [[ "$output" == *"has not reported healthy"* ]]
}

@test "unprovisioned box: nothing is touched" {
    rm "$COMPOSE_DIR/.env"
    run bash "$SCRIPT"
    [ "$status" -eq 1 ]
    [ ! -s "$DOCKER_LOG" ]
}

@test "the lock is released before waiting for health" {
    command -v flock >/dev/null || skip "no flock on this host"
    # While the script waits on an unhealthy gluetun, another actor (the
    # cascade watcher re-linking dependents) must be able to take the lock.
    HEALTH=starting RECREATE_WAIT_S=2 POLL_S=1 bash "$SCRIPT" >/dev/null 2>&1 &
    pid=$!
    sleep 0.7
    run flock -w 0.5 "$MDB_COMPOSE_LOCK" true
    wait "$pid" || true
    [ "$status" -eq 0 ]
}
