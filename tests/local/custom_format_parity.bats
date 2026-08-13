#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# Radarr and Sonarr get the SAME custom-format definitions (rejections for
# AV1/Remux/HEVC-1080p+/HDR/scam-executables/porn, x264 preference, release-
# group tiers). The scoring divergence documented in
# converge_custom_formats.sh (Sonarr is greenfield and omits the legacy
# "Trusted small-release groups" neutralizer entry) is applied SHELL-SIDE
# via SCORE_MAP, not in these
# fixtures — so the two JSON files are meant to define the same SET OF
# FORMATS. They were byte-identical when this test was written (md5
# b89d8a99…); nothing else asserted it, so a Radarr-only retune (one already
# happened 2026-07-26) could silently leave Sonarr scoring against stale
# definitions. This test makes that drift fail in CI.
#
# It compares the set of format NAMES, not bytes: a cosmetic reformat of one
# file is fine, a format present in one and absent from the other is not.

RADARR_CF="$CPP_DIR/scripts/data/radarr_custom_formats.json"
SONARR_CF="$CPP_DIR/scripts/data/sonarr_custom_formats.json"

@test "both custom-format fixtures exist" {
    [ -f "$RADARR_CF" ]
    [ -f "$SONARR_CF" ]
}

@test "both custom-format fixtures are valid JSON" {
    python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$RADARR_CF"
    python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$SONARR_CF"
}

@test "Radarr and Sonarr define the same set of custom-format names" {
    run python3 - "$RADARR_CF" "$SONARR_CF" <<'PY'
import json, sys

def names(path):
    data = json.load(open(path))
    # Fixture is a list of format objects, or an object wrapping one.
    items = data if isinstance(data, list) else data.get("customFormats", data)
    return {f["name"] for f in items}

r, s = names(sys.argv[1]), names(sys.argv[2])
if r != s:
    only_r = sorted(r - s)
    only_s = sorted(s - r)
    print("Custom-format name sets diverged.")
    if only_r: print("  Radarr-only:", only_r)
    if only_s: print("  Sonarr-only:", only_s)
    sys.exit(1)
print(f"OK: {len(r)} formats in sync")
PY
    [ "$status" -eq 0 ] || { echo "$output"; false; }
}
