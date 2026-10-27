#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

@test "clone swap cleanup protects active and off-root swap while scrubbing stale memory" {
    run python3 "$TESTS_REPO_ROOT/tests/local/test_clone_swap_cleanup.py"
    [ "$status" -eq 0 ]
}
