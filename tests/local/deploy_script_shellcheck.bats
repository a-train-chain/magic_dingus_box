#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

SCRIPTS_DIR="$CPP_DIR/scripts"

setup() {
    command -v shellcheck >/dev/null 2>&1 || skip "shellcheck not installed"
}

@test "shellcheck clean: deploy_cpp.sh" {
    run shellcheck -S warning "$SCRIPTS_DIR/deploy_cpp.sh"
    [ "$status" -eq 0 ]
}

@test "shellcheck clean: init_audio.sh" {
    run shellcheck -S warning "$SCRIPTS_DIR/init_audio.sh"
    [ "$status" -eq 0 ]
}

@test "shellcheck clean: update.sh" {
    run shellcheck -S warning "$SCRIPTS_DIR/update.sh"
    [ "$status" -eq 0 ]
}

@test "shellcheck clean: install_cores.sh" {
    [ -f "$SCRIPTS_DIR/install_cores.sh" ] || skip "install_cores.sh not present"
    run shellcheck -S warning "$SCRIPTS_DIR/install_cores.sh"
    [ "$status" -eq 0 ]
}

@test "shellcheck clean: install_deps.sh" {
    [ -f "$SCRIPTS_DIR/install_deps.sh" ] || skip "install_deps.sh not present"
    run shellcheck -S warning "$SCRIPTS_DIR/install_deps.sh"
    [ "$status" -eq 0 ]
}
