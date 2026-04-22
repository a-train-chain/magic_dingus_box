#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

setup() { require_pi; }

@test "magic-dingus-box-cpp.service is active" {
    run pi_ssh 'systemctl is-active magic-dingus-box-cpp.service'
    [ "$status" -eq 0 ]
    [ "$output" = "active" ]
}

@test "magic-dingus-web.service is active" {
    run pi_ssh 'systemctl is-active magic-dingus-web.service'
    [ "$status" -eq 0 ]
    [ "$output" = "active" ]
}

@test "magic-cpu-performance.service is active" {
    run pi_ssh 'systemctl is-active magic-cpu-performance.service'
    [ "$status" -eq 0 ]
    [ "$output" = "active" ]
}

@test "usb-gadget-network.service is active or enabled" {
    # On Pi 4 without an active USB-C connection, the service may be
    # "inactive (dead)" but enabled — both states are acceptable.
    run pi_ssh 'systemctl is-active usb-gadget-network.service || systemctl is-enabled usb-gadget-network.service'
    [ "$status" -eq 0 ]
}
