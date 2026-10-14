#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# verify_box.sh FAILs a box whose installed systemd units differ from its
# tree (converge_system_units.sh --check). Every delivery path converges
# them, so drift means the converge is failing on the box or /etc was
# edited by hand — the class that silenced missing-search for 10 days in
# 2026-10 while verify_box said SHIPPABLE. The verdict function and the
# section are extracted because verify_box.sh runs top-to-bottom on a Pi.

setup() {
    local def
    def=$(sed -n '/^unit_finding_verdict() {/,/^}/p' "$CPP_DIR/scripts/verify_box.sh")
    [ -n "$def" ] || { echo "unit_finding_verdict not found in verify_box.sh"; return 1; }
    eval "$def"
    BLOCK=$(sed -n '/^CONVERGE_UNITS=/,/^fi  # System units$/p' "$CPP_DIR/scripts/verify_box.sh")
    [ -n "$BLOCK" ] || { echo "System units block not found in verify_box.sh"; return 1; }
    BASE="$BATS_TEST_TMPDIR/box"
    mkdir -p "$BASE/magic_dingus_box_cpp/scripts"
}

verdict() { unit_finding_verdict "$1" | cut -f1; }

@test "DRIFT, MISSING and STALE fail; DROPIN warns; anything else warns" {
    [ "$(verdict 'DRIFT magic-dingus-missing-search.timer')" = fail ]
    [ "$(verdict 'MISSING magic-dingus-ota-recovery.service')" = fail ]
    [ "$(verdict 'STALE old-feature.timer')" = fail ]
    [ "$(verdict 'DROPIN /etc/systemd/system/x.service.d/mdb-timeout.conf')" = warn ]
    [ "$(verdict 'what is this')" = warn ]
}

@test "the message names the unit" {
    [[ "$(unit_finding_verdict 'DRIFT magic-dingus-missing-search.timer' | cut -f2)" == *magic-dingus-missing-search.timer* ]]
}

# run_section <converge stub body> — the section against a stub converge
run_section() {
    printf '#!/bin/bash\n%s\n' "$1" > "$BASE/magic_dingus_box_cpp/scripts/converge_system_units.sh"
    run bash -c '
        pass() { echo "[PASS] $1"; }
        fail() { echo "[FAIL] $1"; }
        warn() { echo "[WARN] $1"; }
        BASE="$2"
        eval "$(printf "%s\n" "$3")"
        eval "$1"
    ' _ "$BLOCK" "$BASE" "$(sed -n '/^unit_finding_verdict() {/,/^}/p' "$CPP_DIR/scripts/verify_box.sh")"
}

@test "a converged box passes" {
    run_section 'exit 0'
    [ "$output" = "[PASS] installed systemd units match the tree" ]
}

@test "each finding is its own FAIL/WARN line with the fix command" {
    run_section 'printf "DRIFT a.timer\nDROPIN /x/y.conf\n"; exit 1'
    [ "${#lines[@]}" -eq 2 ]
    [[ "${lines[0]}" == "[FAIL] a.timer"*"converge_system_units.sh"* ]]
    [[ "${lines[1]}" == "[WARN] /x/y.conf"* ]]
}

@test "a converge that cannot check (exit 2) is a WARN, not a pass" {
    run_section 'echo "[units] ERROR: not a Magic Dingus Box tree" >&2; exit 2'
    [[ "$output" == "[WARN] could not compare installed units"* ]]
}

@test "a tree without the script warns" {
    run bash -c 'warn() { echo "[WARN] $1"; }; pass() { :; }; fail() { :; }; BASE="$2"; eval "$1"' _ "$BLOCK" "$BASE"
    [[ "$output" == "[WARN] converge_system_units.sh is not in this tree"* ]]
}
