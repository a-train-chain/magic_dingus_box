#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# The reference settings.json is CHECKED IN. This used to read
# build/dev_data/settings.json — a file no checkout or CI runner has — so
# every test here skipped everywhere and the suite "passed" without ever
# running. The fixture is also loaded by the C++ suite
# (tests/app/test_settings_persistence.cpp), which asserts the kiosk's own
# save_settings() reproduces it byte for byte: if the on-disk format
# changes, that test fails and this file gets regenerated with it.
REFERENCE="$CPP_DIR/tests/app/fixtures/settings.json"

setup() {
    command -v jq >/dev/null || skip "jq not installed"
    # A missing fixture is a FAILURE, not a skip: a skip here is how this
    # suite went silent the first time.
    [ -f "$REFERENCE" ] || { echo "missing fixture: $REFERENCE"; return 1; }
}

@test "reference settings.json is valid JSON" {
    run jq empty "$REFERENCE"
    [ "$status" -eq 0 ]
}

@test "settings.json has the display/audio/playback object shape" {
    run jq -e '
        (.display | type == "object") and
        (.audio | type == "object") and
        (.playback | type == "object") and
        (.display | has("mode"))
    ' "$REFERENCE"
    [ "$status" -eq 0 ]
}

@test "display.mode is one of the values the kiosk reads" {
    run jq -r '.display.mode' "$REFERENCE"
    [ "$status" -eq 0 ]
    case "$output" in
        crt_native|modern_tv) : ;;
        *) echo "unexpected display.mode: $output"; false ;;
    esac
}

@test "audio.output is one of the values the kiosk reads" {
    run jq -r '.audio.output' "$REFERENCE"
    [ "$status" -eq 0 ]
    case "$output" in
        auto|hdmi|headphone) : ;;
        *) echo "unexpected audio.output: $output"; false ;;
    esac
}

@test "numeric and boolean fields carry the types load_settings expects" {
    # A wrong type here is exactly what crash-looped a box (7413d96): the
    # kiosk now quarantines such a file, but the reference must never be one.
    run jq -e '
        (.display.bezel_index | type == "number") and
        (.display.scanline_intensity | type == "number") and
        (.display.enhanced_crt_enabled | type == "boolean") and
        (.playback.master_volume | type == "number") and
        (.playback.master_volume >= 0 and .playback.master_volume <= 100) and
        (.playback.playlist_loop | type == "boolean") and
        (.playback.shuffle | type == "boolean") and
        (.audio.retroarch_volume_offset_db | type == "number")
    ' "$REFERENCE"
    [ "$status" -eq 0 ]
}
