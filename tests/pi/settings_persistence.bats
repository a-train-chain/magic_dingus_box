#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

SETTINGS="/opt/magic_dingus_box/config/settings.json"

setup() {
    require_pi
    # Stash original value of audio.retroarch_volume_offset_db
    ORIGINAL=$(pi_ssh 'jq -r .audio.retroarch_volume_offset_db '"$SETTINGS")
    [ -n "$ORIGINAL" ] || skip "settings.json missing audio.retroarch_volume_offset_db"
}

teardown() {
    # Restore original
    pi_ssh 'tmp=$(mktemp) && jq '"'"'.audio.retroarch_volume_offset_db = '"'"'"'"$ORIGINAL"'"'"''"'"' '"$SETTINGS"' > $tmp && sudo mv $tmp '"$SETTINGS"' && sudo chown magic:magic '"$SETTINGS"' && sudo systemctl restart magic-dingus-box-cpp.service' >/dev/null 2>&1
    sleep 3
}

@test "setting written via JSON edit survives a service restart" {
    # Write a new value (-7.0 dB, distinct from likely default)
    NEW_VALUE="-7.0"
    pi_ssh 'tmp=$(mktemp) && jq '"'"'.audio.retroarch_volume_offset_db = -7.0'"'"' '"$SETTINGS"' > $tmp && sudo mv $tmp '"$SETTINGS"' && sudo chown magic:magic '"$SETTINGS"

    # Restart the service so it loads the new value
    pi_ssh 'sudo systemctl restart magic-dingus-box-cpp.service'
    sleep 5

    # Service should be active again
    run pi_ssh 'systemctl is-active magic-dingus-box-cpp.service'
    [ "$output" = "active" ]

    # And the value should still be there
    run pi_ssh 'jq -r .audio.retroarch_volume_offset_db '"$SETTINGS"
    [ "$output" = "$NEW_VALUE" ]
}
