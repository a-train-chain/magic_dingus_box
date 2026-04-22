#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

setup() {
    require_pi
    # Skip the whole file if no joystick connected — we want this to be
    # informational, not a hard fail. Plug a controller in to enable.
    require_joystick
    read_pi_joystick_id
}

@test "joystick has a non-empty vendor ID in sysfs" {
    [ -n "$PI_JOY_VID" ] || { echo "empty VID at /sys/class/input/js0/device/id/vendor"; false; }
}

@test "joystick has a non-empty product ID in sysfs" {
    [ -n "$PI_JOY_PID" ] || { echo "empty PID at /sys/class/input/js0/device/id/product"; false; }
}

@test "joystick is one of the supported controllers" {
    case "${PI_JOY_VID}:${PI_JOY_PID}" in
        0e6d:111d)
            echo "  Detected: N64 USB adapter (SWITCH CO.,LTD.)"
            ;;
        0079:0006)
            echo "  Detected: PS-style USB pad (DragonRise/Microntek)"
            ;;
        *)
            echo "  Unrecognized controller VID:PID = ${PI_JOY_VID}:${PI_JOY_PID}"
            echo "  Falls back to N64 mapping. Add support in controller_detector if needed."
            # Don't fail — UNKNOWN-controller behavior is intentional fallback.
            skip "informational: unrecognized controller, see message above"
            ;;
    esac
}
