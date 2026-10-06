#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# gluetun_cascade_restart.sh's convergence pass brings back dependents
# that are missing or not running and restarts a *arr/Byparr container
# whose own healthcheck stays unhealthy past a confirm window. Before it,
# the healthy branch only logged: a dependent removed, left "Created" by a
# boot whose `compose up` hit the unit timeout, or wedged unhealthy stayed
# down until a reboot. These tests pin the pure per-container decision.
# (The end-to-end pass against a stub docker lives in
# magic_dingus_box_cpp/scripts/tests/test_gluetun_cascade.py.)

setup() {
    # shellcheck disable=SC1091
    source "$CPP_DIR/scripts/gluetun_cascade_restart.sh"
}

@test "missing or stopped dependent -> up" {
    for st in absent created exited dead; do
        [ "$(dependent_action "$st" "" 0 "" 1000 300)" = "up" ]
    done
}

@test "paused for playback -> never touched, whatever its state" {
    [ "$(dependent_action exited "" 1 "" 1000 300)" = "ok" ]
    [ "$(dependent_action running unhealthy 1 1 1000 300)" = "ok" ]
}

@test "running and healthy (or no healthcheck) -> ok" {
    [ "$(dependent_action running healthy 0 "" 1000 300)" = "ok" ]
    [ "$(dependent_action running "" 0 "" 1000 300)" = "ok" ]
    [ "$(dependent_action running starting 0 "" 1000 300)" = "ok" ]
}

@test "docker or an operator owns transitional states -> ok" {
    [ "$(dependent_action restarting "" 0 "" 1000 300)" = "ok" ]
    [ "$(dependent_action paused "" 0 "" 1000 300)" = "ok" ]
}

@test "newly unhealthy -> mark (start the confirm clock, do not restart yet)" {
    [ "$(dependent_action running unhealthy 0 "" 1000 300)" = "mark" ]
}

@test "unhealthy inside the confirm window -> wait" {
    [ "$(dependent_action running unhealthy 0 900 1000 300)" = "wait" ]
}

@test "unhealthy for the full confirm window -> restart" {
    [ "$(dependent_action running unhealthy 0 700 1000 300)" = "restart" ]
    [ "$(dependent_action running unhealthy 0 0 1000 300)" = "restart" ]
}

@test "sourcing the script runs nothing (main is guarded)" {
    run bash -c "source '$CPP_DIR/scripts/gluetun_cascade_restart.sh' && echo sourced-ok"
    [ "$status" -eq 0 ]
    [ "$output" = "sourced-ok" ]
}

@test "every compose-touching script takes the same shared lock" {
    for f in gluetun_cascade_restart.sh playback_services_pause.sh \
             storage_attach.sh migrate_hardlink_layout.sh recreate_gluetun.sh; do
        grep -q '/run/lock/mdb-compose.lock' "$CPP_DIR/scripts/$f" \
            || { echo "$f does not take the compose lock" >&2; false; }
    done
    grep -q '/run/lock/mdb-compose.lock' "$CPP_DIR/scripts/clear_radarr_cooldowns.py"
}
