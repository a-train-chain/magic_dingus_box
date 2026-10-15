#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# verify_box.sh's "VPN tunnel" section: the Box Health card's view of how
# often the ProtonVPN tunnel dropped in the last 24 h. The owner's box
# dropped ~170 times over 2026-10-03/04 with nothing on any screen saying
# so. verify_box.sh runs top-to-bottom against live hardware, so the block
# is extracted (VPN_EVENTS= through its closing fi) and run against stub
# header/pass/warn/fail and a stub docker. The wording and thresholds are
# pinned in magic_dingus_box_cpp/scripts/tests/test_vpn_events_summary.py.

setup() {
    BLOCK=$(sed -n '/^VPN_EVENTS=/,/^fi$/p' "$CPP_DIR/scripts/verify_box.sh")
    [ -n "$BLOCK" ] || { echo "VPN tunnel block not found in verify_box.sh"; return 1; }
    BASE="$BATS_TEST_TMPDIR/base"
    APP="$CPP_DIR"
    mkdir -p "$BASE/services" "$BATS_TEST_TMPDIR/bin"
    printf '#!/bin/sh\necho "${STUB_HEALTH:-healthy}"\n' > "$BATS_TEST_TMPDIR/bin/docker"
    chmod +x "$BATS_TEST_TMPDIR/bin/docker"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
    export MDB_VPN_EVENTS_FILE="$BATS_TEST_TMPDIR/vpn_events.log"
    NOW=$(date +%s)
}

run_block() {
    run bash -c '
        header() { echo "== $1 =="; }
        pass() { echo "[PASS] $1"; }
        warn() { echo "[WARN] $1"; }
        fail() { echo "[FAIL] $1"; }
        BASE="$1"; APP="$2"
        eval "$3"
    ' _ "$BASE" "$APP" "$BLOCK"
}

@test "no services/.env -> no VPN section at all" {
    run_block
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "configured, no history yet -> PASS" {
    touch "$BASE/services/.env"
    run_block
    [ "${lines[0]}" = "== VPN tunnel ==" ]
    [[ "${lines[1]}" == "[PASS] VPN tunnel: no history recorded yet"* ]]
}

@test "quiet tunnel -> PASS with the numbers" {
    touch "$BASE/services/.env"
    printf '%s watch\n%s unhealthy tunnel\n%s healthy\n' \
        $((NOW - 100000)) $((NOW - 3600)) $((NOW - 3300)) > "$MDB_VPN_EVENTS_FILE"
    run_block
    [ "${lines[1]}" = "[PASS] VPN tunnel dropped once in the last 24 h (down 5 min total, longest 5 min, last 1 h ago)" ]
}

@test "a burst of drops -> WARN naming the fix, never FAIL" {
    touch "$BASE/services/.env"
    : > "$MDB_VPN_EVENTS_FILE"
    for i in $(seq 1 8); do
        printf '%s unhealthy tunnel\n%s healthy\n' \
            $((NOW - i * 660)) $((NOW - i * 660 + 240)) >> "$MDB_VPN_EVENTS_FILE"
    done
    run_block
    [[ "${lines[1]}" == "[WARN] VPN tunnel dropped 8 times"* ]]
    [[ "$output" == *"try another VPN country"* ]]
    [[ "$output" != *"[FAIL]"* ]]
}

@test "current health is passed through (unrecorded recovery is not 'down now')" {
    touch "$BASE/services/.env"
    printf '%s watch\n%s unhealthy tunnel\n' $((NOW - 100000)) $((NOW - 7200)) > "$MDB_VPN_EVENTS_FILE"
    STUB_HEALTH=unhealthy run_block
    [[ "${lines[1]}" == "[WARN] VPN tunnel is DOWN right now"* ]]
    STUB_HEALTH=healthy run_block
    [[ "${lines[1]}" != *"DOWN right now"* ]]
}

@test "summarizer missing -> WARN, the run continues" {
    touch "$BASE/services/.env"
    APP="$BATS_TEST_TMPDIR/no_such_app"
    run_block
    [ "$status" -eq 0 ]
    [[ "${lines[1]}" == "[WARN] VPN tunnel history could not be summarized"* ]]
}

@test "the VPN section never FAILs a box" {
    ! grep -q 'fail ' <<<"$BLOCK"
}
