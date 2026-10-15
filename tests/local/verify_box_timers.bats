#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# verify_box.sh FAILs a box whose repeating safety-net timers have gone
# dormant. Origin (hardware, 2026-10-14): a drop-in's empty OnBootSec= reset
# EVERY trigger of magic-dingus-missing-search.timer, so it ran once per
# boot and then sat 'elapsed' — no retry of missing movies for 10 days on
# the owner's box, while verify_box called it SHIPPABLE. The classifier is
# extracted because verify_box.sh runs top-to-bottom against live hardware.

setup() {
    local def
    def=$(sed -n '/^timer_health_of() {/,/^}/p' "$CPP_DIR/scripts/verify_box.sh")
    [ -n "$def" ] || { echo "timer_health_of not found in verify_box.sh"; return 1; }
    eval "$def"
}

# What `systemctl show -p LoadState -p ActiveState -p SubState <t>` prints.
show() { printf 'LoadState=%s\nActiveState=%s\nSubState=%s\n' "$1" "$2" "$3"; }

@test "waiting and running timers are armed" {
    [ "$(timer_health_of "$(show loaded active waiting)")" = "armed" ]
    [ "$(timer_health_of "$(show loaded active running)")" = "armed" ]
}

@test "elapsed is dormant: the 2026-10-14 missing-search state" {
    [ "$(timer_health_of "$(show loaded active elapsed)")" = "dormant" ]
}

@test "stopped, failed and masked timers are stopped" {
    [ "$(timer_health_of "$(show loaded inactive dead)")" = "stopped" ]
    [ "$(timer_health_of "$(show loaded failed failed)")" = "stopped" ]
    [ "$(timer_health_of "$(show masked inactive dead)")" = "stopped" ]
}

@test "a timer that is not installed (games-only box) is skipped" {
    [ "$(timer_health_of "$(show not-found inactive dead)")" = "skip" ]
    # No systemctl at all (dev machine) prints nothing.
    [ "$(timer_health_of "")" = "skip" ]
}

@test "a transitional or unreadable state is unknown, never armed" {
    [ "$(timer_health_of "$(show loaded activating start)")" = "unknown" ]
    [ "$(timer_health_of "LoadState=loaded")" = "unknown" ]
}

@test "property order does not matter" {
    [ "$(timer_health_of "$(printf 'SubState=elapsed\nLoadState=loaded\nActiveState=active\n')")" = "dormant" ]
}

@test "every in-tree .timer is on the repeating list" {
    local list t missing=""
    list=$(sed -n 's/^REPEATING_TIMERS=(\(.*\))$/ \1 /p' "$CPP_DIR/scripts/verify_box.sh")
    [ -n "$list" ]
    while IFS= read -r t; do
        [[ "$list" == *" $(basename "$t") "* ]] || missing+=" $(basename "$t")"
    done < <(git -C "$TESTS_REPO_ROOT" ls-files '*.timer')
    [ -z "$missing" ] || { echo "not checked by verify_box:$missing"; false; }
}

@test "dormant and stopped timers are FAILs; verify_box asks systemd for all three states" {
    grep -q 'dormant) *fail "' "$CPP_DIR/scripts/verify_box.sh"
    grep -q 'stopped) *fail "' "$CPP_DIR/scripts/verify_box.sh"
    grep -qF 'systemctl show -p LoadState -p ActiveState -p SubState "$_t"' \
        "$CPP_DIR/scripts/verify_box.sh"
}
