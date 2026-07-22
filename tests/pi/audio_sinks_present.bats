#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

setup() { require_pi; }

@test "PulseAudio is running" {
    run pi_ssh 'pactl info 2>/dev/null | grep -q "Server Name"'
    [ "$status" -eq 0 ]
}

@test "HDMI sink is present" {
    run pi_ssh 'pactl list sinks short 2>/dev/null | grep -i hdmi'
    [ "$status" -eq 0 ]
}

@test "Headphone (mailbox) sink is present (Pi 4 only — Pi 5 has no analog jack)" {
    run pi_ssh 'grep -q "Raspberry Pi 5" /proc/device-tree/model'
    [ "$status" -ne 0 ] || skip "Pi 5 has no 3.5mm jack / mailbox sink"
    # bcm2835 / mailbox sink corresponds to the 3.5mm jack
    run pi_ssh 'pactl list sinks short 2>/dev/null | grep -iE "mailbox|bcm2835|headphones"'
    [ "$status" -eq 0 ]
}
