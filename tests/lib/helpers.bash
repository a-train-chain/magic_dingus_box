#!/usr/bin/env bash
# Shared helpers for the Magic Dingus Box test suite.
# Source this from any .bats file with:
#   load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# Repo root (one level up from tests/).
# Set TESTS_REPO_ROOT in the environment to override.
: "${TESTS_REPO_ROOT:=$(cd "$BATS_TEST_DIRNAME/../.." && pwd)}"
export TESTS_REPO_ROOT

# Pi connection target. Override with: PI_HOST=user@host
: "${PI_HOST:=magic@10.55.0.1}"
export PI_HOST

# Path to the magic_dingus_box_cpp tree.
: "${CPP_DIR:=$TESTS_REPO_ROOT/magic_dingus_box_cpp}"
export CPP_DIR

# Run a command on the Pi via SSH. Stdout/stderr/exit code propagate.
# Fails the test if SSH itself fails (e.g., Pi unreachable).
pi_ssh() {
    ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new \
        -o BatchMode=yes "$PI_HOST" "$@"
}

# Verify the Pi is reachable before running a Pi-tier test.
# Use this in `setup_file` (Bats) or first `setup` to skip cleanly.
require_pi() {
    if ! pi_ssh true 2>/dev/null; then
        skip "Pi not reachable at $PI_HOST"
    fi
}

# Check the Pi has a joystick connected. Skip if not.
require_joystick() {
    require_pi
    if ! pi_ssh 'ls /dev/input/js* >/dev/null 2>&1'; then
        skip "no joystick at /dev/input/js* on $PI_HOST"
    fi
}

# Read VID/PID of the first joystick on the Pi.
# Sets BATS variables: PI_JOY_VID, PI_JOY_PID
read_pi_joystick_id() {
    require_joystick
    PI_JOY_VID="$(pi_ssh 'cat /sys/class/input/js0/device/id/vendor 2>/dev/null')"
    PI_JOY_PID="$(pi_ssh 'cat /sys/class/input/js0/device/id/product 2>/dev/null')"
}
