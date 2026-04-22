#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

setup() { require_pi; }

@test "idle temperature is below 70°C" {
    run pi_ssh 'vcgencmd measure_temp'
    [ "$status" -eq 0 ]
    # Output: temp=54.5'C
    temp=$(echo "$output" | sed "s/temp=//; s/'C//; s/\..*//")
    [ "$temp" -lt 70 ] || { echo "TEMP TOO HIGH: ${temp}°C (max 70 idle)"; false; }
    if [ "$temp" -ge 65 ]; then
        echo "WARN: idle temp is ${temp}°C (above 65°C — check airflow before sustained load)"
    fi
}
