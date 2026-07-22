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

@test "GPIO 3 power switch is handled (standby watcher or gpio-shutdown overlay)" {
    # The production Pi replaced the gpio-shutdown overlay with
    # kiosk-standby-watcher.service (overlay line commented out in
    # config.txt, 2026). Accept either mechanism — exactly one of
    # them must be active for the physical switch to work.
    run pi_ssh "grep -q '^dtoverlay=gpio-shutdown' $CONFIG || systemctl is-enabled --quiet kiosk-standby-watcher.service"
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

@test "config.txt sets gpu_mem to the headroom-round value (76)" {
    # 2026-07-16 performance-headroom round: KMS/V3D allocates from
    # CMA, not firmware memory, so gpu_mem was cut from 128 to the
    # firmware-recommended floor of 76 (see CLONING.md "Boot config").
    # On a Pi 5 this key is ignored entirely and should be absent.
    run pi_ssh "grep -E '^gpu_mem=' $CONFIG"
    [ "$status" -eq 0 ]
    val=$(echo "$output" | sed 's/^gpu_mem=//' | head -1)
    [ "$val" -eq 76 ] || { echo "gpu_mem=$val (want 76 per headroom round)"; false; }
}

@test "config.txt does NOT set force_turbo (removed in headroom round)" {
    # force_turbo=1 was removed 2026-07-16 so the SoC can downclock at
    # idle (~6 °C cooler); the performance CPU governor still pins the
    # clock whenever the kiosk is running (see CLONING.md "Boot config").
    run pi_ssh "grep -q '^force_turbo=' $CONFIG"
    [ "$status" -ne 0 ]
}

@test "config.txt has KMS video driver overlay (vc4-kms-v3d)" {
    run pi_ssh "grep -q '^dtoverlay=vc4-kms-v3d' $CONFIG"
    [ "$status" -eq 0 ]
}
