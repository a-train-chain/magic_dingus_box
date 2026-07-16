#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

SCRIPTS_DIR="$CPP_DIR/scripts"

setup() {
    command -v shellcheck >/dev/null 2>&1 || skip "shellcheck not installed"
}

@test "shellcheck clean: deploy_cpp.sh" {
    run shellcheck -S error "$SCRIPTS_DIR/deploy_cpp.sh"
    [ "$status" -eq 0 ]
}

@test "deploy excludes host build and Python cache artifacts" {
    for pattern in 'build-*' '__pycache__' '*.pyc'; do
        grep -Fq -- "--exclude '$pattern'" "$SCRIPTS_DIR/deploy_cpp.sh" || {
            echo "deploy_cpp.sh is missing rsync exclude: $pattern"
            false
        }
    done
}

@test "shellcheck clean: init_audio.sh" {
    run shellcheck -S error "$SCRIPTS_DIR/init_audio.sh"
    [ "$status" -eq 0 ]
}

@test "shellcheck clean: update.sh" {
    run shellcheck -S error "$SCRIPTS_DIR/update.sh"
    [ "$status" -eq 0 ]
}

@test "shellcheck clean: install_cores.sh" {
    [ -f "$SCRIPTS_DIR/install_cores.sh" ] || skip "install_cores.sh not present"
    run shellcheck -S error "$SCRIPTS_DIR/install_cores.sh"
    [ "$status" -eq 0 ]
}

@test "shellcheck clean: install_deps.sh" {
    [ -f "$SCRIPTS_DIR/install_deps.sh" ] || skip "install_deps.sh not present"
    run shellcheck -S error "$SCRIPTS_DIR/install_deps.sh"
    [ "$status" -eq 0 ]
}
