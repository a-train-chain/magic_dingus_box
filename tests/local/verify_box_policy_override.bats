#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# verify_box.sh must FAIL a box carrying the TEST-ONLY
# MDB_PLATFORM_POLICY_OVERRIDE (a Pi 5 rehearsing Pi 4B software policy
# must never be cloned or shipped). These tests pin the pure parser it
# runs over `systemctl show -p Environment --value`, services/.env and
# /proc/<pid>/environ. verify_box.sh runs top-to-bottom against live
# hardware, so the function is extracted from it rather than sourced.

setup() {
    local def
    def=$(sed -n '/^policy_override_in_env() {/,/^}/p' "$CPP_DIR/scripts/verify_box.sh")
    [ -n "$def" ] || { echo "policy_override_in_env not found in verify_box.sh"; return 1; }
    eval "$def"
}

@test "systemctl Environment with the override set -> value" {
    [ "$(policy_override_in_env 'HOME=/home/magic DISPLAY= MDB_PLATFORM_POLICY_OVERRIDE=pi4 XDG_RUNTIME_DIR=/run/user/1000')" = "pi4" ]
}

@test "systemctl Environment without the override -> nothing" {
    [ -z "$(policy_override_in_env 'HOME=/home/magic DISPLAY= XDG_RUNTIME_DIR=/run/user/1000')" ]
}

@test "empty override value counts as unset (binaries treat it so)" {
    [ -z "$(policy_override_in_env 'HOME=/home/magic MDB_PLATFORM_POLICY_OVERRIDE=')" ]
}

@test "an invalid value still FAILS the box (any non-empty value)" {
    [ "$(policy_override_in_env 'MDB_PLATFORM_POLICY_OVERRIDE=bogus')" = "bogus" ]
}

@test "quoted systemctl token -> value" {
    [ "$(policy_override_in_env '"MDB_PLATFORM_POLICY_OVERRIDE=pi4" HOME=/home/magic')" = "pi4" ]
}

@test "env-file text, with export and quotes -> value" {
    text=$'RADARR_API_KEY=abc\nexport MDB_PLATFORM_POLICY_OVERRIDE="pi4"\n'
    [ "$(policy_override_in_env "$text")" = "pi4" ]
}

@test "commented-out env-file line -> nothing" {
    text=$'RADARR_API_KEY=abc\n#MDB_PLATFORM_POLICY_OVERRIDE=pi4\n'
    [ -z "$(policy_override_in_env "$text")" ]
}

@test "a similarly named variable is not the override" {
    [ -z "$(policy_override_in_env 'XMDB_PLATFORM_POLICY_OVERRIDE=pi4 MDB_PI_MODEL_OVERRIDE=Raspberry')" ]
}

@test "no input -> nothing" {
    [ -z "$(policy_override_in_env '')" ]
}

@test "verify_box FAILs (not WARNs) on the override" {
    run grep -nE '^\s*fail "TEST-ONLY platform policy override is ON' "$CPP_DIR/scripts/verify_box.sh"
    [ "$status" -eq 0 ]
}
