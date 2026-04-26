#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# Verify /boot/firmware/config.txt has the expected lines for this kiosk.
# Catches: an apt upgrade or rpi-update reverts config.txt → clones boot
# at stock 1.5GHz / no USB gadget / no rotary / no power button.

CONFIG="/boot/firmware/config.txt"

setup() { require_pi; }

@test "config.txt exists" {
    run pi_ssh "test -f $CONFIG"
    [ "$status" -eq 0 ]
}

@test "config.txt enables 64-bit ARM" {
    run pi_ssh "grep -q '^arm_64bit=1' $CONFIG"
    [ "$status" -eq 0 ]
}

@test "config.txt has USB gadget overlay (dtoverlay=dwc2)" {
    run pi_ssh "grep -q '^dtoverlay=dwc2' $CONFIG"
    [ "$status" -eq 0 ]
}

@test "config.txt has rotary encoder overlay" {
    run pi_ssh "grep -q '^dtoverlay=rotary-encoder' $CONFIG"
    [ "$status" -eq 0 ]
}

@test "config.txt has GPIO shutdown button overlay" {
    run pi_ssh "grep -q '^dtoverlay=gpio-shutdown' $CONFIG"
    [ "$status" -eq 0 ]
}

@test "config.txt has CPU overclock (arm_freq=2000)" {
    run pi_ssh "grep -q '^arm_freq=2000' $CONFIG"
    [ "$status" -eq 0 ]
}

@test "config.txt has GPU overclock (gpu_freq=600 and v3d_freq=600)" {
    run pi_ssh "grep -q '^gpu_freq=600' $CONFIG"
    [ "$status" -eq 0 ]
    run pi_ssh "grep -q '^v3d_freq=600' $CONFIG"
    [ "$status" -eq 0 ]
}

@test "config.txt sets gpu_mem to at least 256" {
    run pi_ssh "grep -E '^gpu_mem=' $CONFIG"
    [ "$status" -eq 0 ]
    val=$(echo "$output" | sed 's/^gpu_mem=//' | head -1)
    [ "$val" -ge 256 ] || { echo "gpu_mem=$val (want >= 256)"; false; }
}

@test "config.txt has force_turbo=1" {
    run pi_ssh "grep -q '^force_turbo=1' $CONFIG"
    [ "$status" -eq 0 ]
}

@test "config.txt has KMS video driver overlay (vc4-kms-v3d)" {
    run pi_ssh "grep -q '^dtoverlay=vc4-kms-v3d' $CONFIG"
    [ "$status" -eq 0 ]
}
