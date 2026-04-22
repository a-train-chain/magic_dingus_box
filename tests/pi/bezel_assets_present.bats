#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

PI_BEZELS_DIR="/opt/magic_dingus_box/magic_dingus_box_cpp/assets/bezels"

setup() { require_pi; }

@test "bezels.json exists on Pi" {
    run pi_ssh "test -f $PI_BEZELS_DIR/bezels.json"
    [ "$status" -eq 0 ]
}

@test "every PNG referenced in bezels.json exists on Pi" {
    run pi_ssh 'cd '"$PI_BEZELS_DIR"' && jq -r ".bezels[] | select(.file != null) | .file" bezels.json | while read -r f; do [ -f "$f" ] || { echo "MISSING: $f"; exit 1; }; done'
    [ "$status" -eq 0 ]
}

@test "every MDB bezel has a .cfg sidecar on Pi" {
    run pi_ssh 'cd '"$PI_BEZELS_DIR"' && jq -r ".bezels[] | select(.id | startswith(\"mdb_\")) | .file" bezels.json | while read -r f; do cfg="${f%.png}.cfg"; [ -f "$cfg" ] || { echo "MISSING SIDECAR: $cfg"; exit 1; }; done'
    [ "$status" -eq 0 ]
}
