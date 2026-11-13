#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

BEZELS_JSON="$CPP_DIR/assets/bezels/bezels.json"
BEZELS_DIR="$CPP_DIR/assets/bezels"

@test "bezels.json exists and is valid JSON" {
    [ -f "$BEZELS_JSON" ]
    run jq empty "$BEZELS_JSON"
    [ "$status" -eq 0 ]
}

@test "bezels.json has a 'bezels' array" {
    run jq -e '.bezels | type == "array"' "$BEZELS_JSON"
    [ "$status" -eq 0 ]
    [ "$output" = "true" ]
}

@test "every bezel entry has required fields" {
    run jq '.bezels[] | select((.id | type != "string") or (.name | type != "string") or (.description | type != "string"))' "$BEZELS_JSON"
    # If any bezel is missing required fields, this will produce output
    [ -z "$output" ]
}

@test "every non-procedural bezel has a 'file' that exists on disk" {
    while IFS= read -r file; do
        [ -n "$file" ]
        [ -f "$BEZELS_DIR/$file" ] || { echo "missing: $BEZELS_DIR/$file"; false; }
    done < <(jq -r '.bezels[] | select(.file != null) | .file' "$BEZELS_JSON")
}

# Fit-mode video is drawn inside this window (app/bezel_catalog.h); an entry
# the kiosk cannot use silently falls back to the whole 4:3 area, under the
# frame. test_app_unit "[bezel_catalog]" holds the values to the PNGs.
@test "every bezel with a file declares its opening: four positive integers inside 1920x1080" {
    run jq -r '.bezels[] | select(.file != null)
        | select(if (.opening | type) != "object" then true
                 else (.opening as $o
                       | ([$o.x, $o.y, $o.w, $o.h]
                          | all(type == "number" and . > 0 and . == floor))
                         and $o.x + $o.w <= 1920 and $o.y + $o.h <= 1080
                       | not)
                 end)
        | .id' "$BEZELS_JSON"
    [ "$status" -eq 0 ]
    [ -z "$output" ] || { echo "missing or bad opening: $output"; false; }
}

@test "every MDB bezel has a matching .cfg sidecar" {
    while IFS= read -r file; do
        cfg="${file%.png}.cfg"
        [ -f "$BEZELS_DIR/$cfg" ] || { echo "missing sidecar: $BEZELS_DIR/$cfg"; false; }
    done < <(jq -r '.bezels[] | select(.id | startswith("mdb_")) | .file' "$BEZELS_JSON")
}

@test "no duplicate bezel ids" {
    count_total=$(jq '.bezels | length' "$BEZELS_JSON")
    count_unique=$(jq '[.bezels[].id] | unique | length' "$BEZELS_JSON")
    [ "$count_total" = "$count_unique" ]
}
