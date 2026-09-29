#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# storage_attach.sh re-creates the storage-bound containers when a movie
# drive is plugged in after the stack started. Its old guard compared entry
# COUNTS (host > 0, container == 0) and could never fire: setup always
# creates library/tv/.mdb-keep, so the SD-card placeholder was never empty.
# The guard now asks whether each container can read a token written onto
# the drive. These tests pin the pure decision functions.

setup() {
    # shellcheck disable=SC1091
    source "$CPP_DIR/scripts/storage_attach.sh"
}

@test "token read back through the bind -> live" {
    [ "$(bind_verdict tok123 tok123 running)" = "live" ]
}

@test "placeholder with content (tv/.mdb-keep) but no token -> stale" {
    [ "$(bind_verdict tok123 "" running)" = "stale" ]
}

@test "container reads a different token -> stale" {
    [ "$(bind_verdict tok123 old999 running)" = "stale" ]
}

@test "container not running -> unknown, never stale" {
    [ "$(bind_verdict tok123 "" absent)" = "unknown" ]
}

@test "token could not be written on the drive -> unknown" {
    [ "$(bind_verdict "" "" running)" = "unknown" ]
}

@test "relink when any container is stale" {
    relink_needed live stale
    relink_needed stale unknown
}

@test "no relink when all live or unknown" {
    run relink_needed live live
    [ "$status" -ne 0 ]
    run relink_needed live unknown
    [ "$status" -ne 0 ]
    run relink_needed unknown unknown
    [ "$status" -ne 0 ]
}

@test "sourcing the script runs nothing (main is guarded)" {
    run bash -c "source '$CPP_DIR/scripts/storage_attach.sh' && echo sourced-ok"
    [ "$status" -eq 0 ]
    [ "$output" = "sourced-ok" ]
}

@test "old count-based guard is gone" {
    run grep -q 'cont_count == 0' "$CPP_DIR/scripts/storage_attach.sh"
    [ "$status" -ne 0 ]
}
