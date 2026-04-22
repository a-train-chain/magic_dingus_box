#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

setup() { require_pi; }

@test "ARM clock is at or above 1.95 GHz" {
    run pi_ssh 'vcgencmd measure_clock arm'
    [ "$status" -eq 0 ]
    # Output: frequency(48)=2000478464
    hz=$(echo "$output" | sed 's/.*=//')
    [ "$hz" -ge 1950000000 ] || { echo "ARM clock too low: $hz Hz (want >= 1.95 GHz)"; false; }
}

@test "V3D clock is at or above 590 MHz" {
    run pi_ssh 'vcgencmd measure_clock v3d'
    [ "$status" -eq 0 ]
    hz=$(echo "$output" | sed 's/.*=//')
    [ "$hz" -ge 590000000 ] || { echo "V3D clock too low: $hz Hz (want >= 590 MHz)"; false; }
}

@test "GPU memory split is at least 256 MB" {
    run pi_ssh 'vcgencmd get_mem gpu'
    [ "$status" -eq 0 ]
    # Output: gpu=256M
    mb=$(echo "$output" | sed 's/gpu=//; s/M//')
    [ "$mb" -ge 256 ] || { echo "gpu_mem too low: ${mb}M (want >= 256M)"; false; }
}

@test "CPU governor is 'performance' on cpu0" {
    run pi_ssh 'cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor'
    [ "$status" -eq 0 ]
    [ "$output" = "performance" ] || { echo "governor: $output (want performance)"; false; }
}

@test "throttle flags are 0x0 (no thermal/voltage issues)" {
    run pi_ssh 'vcgencmd get_throttled'
    [ "$status" -eq 0 ]
    [ "$output" = "throttled=0x0" ] || { echo "throttled: $output (want 0x0)"; false; }
}
