#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# configure_docker_logging.sh merges json-file rotation into daemon.json
# without clobbering the operator's other keys. setup_services.sh calls it
# and restarts dockerd only when it reports "changed".

SCRIPT="$CPP_DIR/scripts/configure_docker_logging.sh"

setup() {
    command -v jq >/dev/null 2>&1 || skip "jq not installed"
    TMPD="$(mktemp -d)"
    DJ="$TMPD/docker/daemon.json"
}

teardown() {
    [ -n "${TMPD:-}" ] && rm -rf "$TMPD"
}

@test "creates daemon.json with rotation when absent" {
    run bash "$SCRIPT" "$DJ"
    [ "$status" -eq 0 ]
    [ "$output" = "changed" ]
    [ "$(jq -r '."log-driver"' "$DJ")" = "json-file" ]
    [ "$(jq -r '."log-opts"."max-size"' "$DJ")" = "10m" ]
    [ "$(jq -r '."log-opts"."max-file"' "$DJ")" = "3" ]
}

@test "second run is a no-op" {
    bash "$SCRIPT" "$DJ" >/dev/null
    before="$(cat "$DJ")"
    run bash "$SCRIPT" "$DJ"
    [ "$status" -eq 0 ]
    [ "$output" = "unchanged" ]
    [ "$(cat "$DJ")" = "$before" ]
}

@test "preserves unrelated keys and other log-opts" {
    mkdir -p "$(dirname "$DJ")"
    echo '{"data-root":"/mnt/docker","log-opts":{"labels":"x"},"dns":["1.1.1.1"]}' > "$DJ"
    run bash "$SCRIPT" "$DJ"
    [ "$output" = "changed" ]
    [ "$(jq -r '."data-root"' "$DJ")" = "/mnt/docker" ]
    [ "$(jq -r '.dns[0]' "$DJ")" = "1.1.1.1" ]
    [ "$(jq -r '."log-opts".labels' "$DJ")" = "x" ]
    [ "$(jq -r '."log-opts"."max-size"' "$DJ")" = "10m" ]
}

@test "converges a stale max-size (present is not correct)" {
    mkdir -p "$(dirname "$DJ")"
    echo '{"log-driver":"json-file","log-opts":{"max-size":"500m","max-file":"3"}}' > "$DJ"
    run bash "$SCRIPT" "$DJ"
    [ "$output" = "changed" ]
    [ "$(jq -r '."log-opts"."max-size"' "$DJ")" = "10m" ]
}

@test "leaves a non-json-file driver alone" {
    mkdir -p "$(dirname "$DJ")"
    echo '{"log-driver":"journald"}' > "$DJ"
    run bash "$SCRIPT" "$DJ"
    [ "$status" -eq 0 ]
    [ "$output" = "unchanged" ]
    [ "$(jq -r '."log-opts"' "$DJ")" = "null" ]
}

@test "refuses to touch invalid JSON" {
    mkdir -p "$(dirname "$DJ")"
    printf '{"data-root": "/x",}\n' > "$DJ"
    before="$(cat "$DJ")"
    run bash "$SCRIPT" "$DJ"
    [ "$status" -eq 1 ]
    [ "$(cat "$DJ")" = "$before" ]
}

@test "empty file is treated as {}" {
    mkdir -p "$(dirname "$DJ")"
    : > "$DJ"
    run bash "$SCRIPT" "$DJ"
    [ "$status" -eq 0 ]
    [ "$output" = "changed" ]
    [ "$(jq -r '."log-opts"."max-file"' "$DJ")" = "3" ]
}

@test "setup_services.sh wires the rotation step in" {
    run grep -q "configure_docker_logging.sh" "$CPP_DIR/scripts/setup_services.sh"
    [ "$status" -eq 0 ]
}
