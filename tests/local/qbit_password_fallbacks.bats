#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# qBittorrent 5.x removed the adminadmin default — new containers print a
# per-session temporary password in their log. Both password-touching
# scripts must know all three fallbacks (.env, adminadmin, temporary) or
# fresh provisions dead-end (hit live on the first Pi 5 bench, 2026-07-22).

@test "setup_services.sh falls back to the session temporary password" {
    grep -q 'temporary password' "$CPP_DIR/scripts/setup_services.sh"
}

@test "sync_qbit_password.sh falls back to the session temporary password" {
    grep -q 'docker logs mdb_qbittorrent' "$CPP_DIR/scripts/sync_qbit_password.sh"
}

@test "shellcheck clean: both qBit password scripts" {
    command -v shellcheck >/dev/null 2>&1 || skip "shellcheck not installed"
    run shellcheck -S error "$CPP_DIR/scripts/setup_services.sh" "$CPP_DIR/scripts/sync_qbit_password.sh"
    [ "$status" -eq 0 ]
}
