load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

@test "restart readiness rejects a previous process status from the same second" {
    helper="$(sed -n '/^kiosk_restart_wait() {/,/^}/p' "$TESTS_REPO_ROOT/magic_dingus_box_cpp/scripts/hw_validate.sh")"
    run bash -c '
        eval "$1"
        UNIT=test.service; DRY_RUN=0
        # Old kiosk status was written at 100.5; restart begins at 100.75.
        date() { if [[ "$1" == +%s ]]; then echo 100; else echo 100.75; fi; }
        mut() { return 0; }
        lib() {
            [[ "$1" == wait-menu && "$2" == --after ]] || return 2
            awk -v after="$3" "BEGIN { exit !(after > 100.5) }"
        }
        kiosk_restart_wait
    ' _ "$helper"
    [ "$status" -eq 0 ]
}
