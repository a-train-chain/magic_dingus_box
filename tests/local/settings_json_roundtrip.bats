#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

REFERENCE="$CPP_DIR/build/dev_data/settings.json"

setup() {
    [ -f "$REFERENCE" ] || skip "no reference settings.json at $REFERENCE (run a Pi build first)"
}

@test "reference settings.json is valid JSON" {
    run jq empty "$REFERENCE"
    [ "$status" -eq 0 ]
}

@test "settings.json has expected top-level structure" {
    # Either old shape (top-level: display_mode, modern_resolution, ...)
    # or new shape (display:{}, audio:{}, playback:{})
    run jq -e '
        (
            (has("display_mode") and has("show_bezel"))
            or
            (has("display") and (.display | has("mode")))
        )
    ' "$REFERENCE"
    [ "$status" -eq 0 ]
}

@test "if display.mode is present, it is one of the allowed values" {
    run jq -r 'select(.display.mode != null) | .display.mode' "$REFERENCE"
    if [ -n "$output" ]; then
        case "$output" in
            crt_native|modern_tv|modern_bezel) : ;;
            *) echo "unexpected display.mode: $output"; false ;;
        esac
    fi
}
