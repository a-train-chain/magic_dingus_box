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

@test "sync_qbit_password.sh pins the same alt-speed rates as the kiosk" {
    # The boot unit runs after the kiosk's own configure_alt_speed_limits
    # (2 MiB/s down, 8 KiB/s up), so any drift here silently wins on every
    # box — it pinned 1 MiB/s up until 2026-10. (The kiosk call lives in
    # media_browser/mb_services.cpp since main.cpp was slimmed.)
    grep -q '/\*dl_bytes_s=\*/2 \* 1024 \* 1024' "$CPP_DIR/src/media_browser/mb_services.cpp"
    grep -q '/\*up_bytes_s=\*/8 \* 1024)' "$CPP_DIR/src/media_browser/mb_services.cpp"
    grep -q '"alt_dl_limit":2097152,"alt_up_limit":8192' "$CPP_DIR/scripts/sync_qbit_password.sh"
}

@test "setup_services.sh restarts the kiosk only when its MDB_QBIT_PASS is stale" {
    # Every Reconfigure used to restart the kiosk, killing a movie or game
    # in progress even when nothing had changed.
    f="$CPP_DIR/scripts/setup_services.sh"
    run grep -n 'systemctl restart magic-dingus-box-cpp.service' "$f"
    [ "${#lines[@]}" -eq 1 ]
    restart_line="${lines[0]%%:*}"
    gate_line=$(grep -n 'if \[ "${KIOSK_RESTART}" = "1" \]' "$f" | cut -d: -f1)
    [ -n "$gate_line" ]
    [ "$gate_line" -lt "$restart_line" ]
    [ $((restart_line - gate_line)) -le 3 ]
}

@test "shellcheck clean: both qBit password scripts" {
    command -v shellcheck >/dev/null 2>&1 || skip "shellcheck not installed"
    run shellcheck -S error "$CPP_DIR/scripts/setup_services.sh" "$CPP_DIR/scripts/sync_qbit_password.sh"
    [ "$status" -eq 0 ]
}
