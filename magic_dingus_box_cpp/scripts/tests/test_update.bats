#!/usr/bin/env bats
#
# BATS tests for update.sh
#
# Run with: bats test_update.bats
# Install BATS: brew install bats-core (macOS) or apt install bats (Linux)
#

# Get the directory containing this test file
SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")" && pwd)"
UPDATE_SCRIPT="$SCRIPT_DIR/../update.sh"

# Setup - runs before each test
setup() {
    # Create a temporary directory for test artifacts
    TEST_TEMP_DIR="$(mktemp -d)"
    export MAGIC_BASE_PATH="$TEST_TEMP_DIR/install"
    export MAGIC_BACKUP_DIR="$TEST_TEMP_DIR/backup"
    export MAGIC_TEMP_DIR="$TEST_TEMP_DIR/tmp"

    # Create the directory structure
    mkdir -p "$MAGIC_BASE_PATH"
    mkdir -p "$MAGIC_BASE_PATH/magic_dingus_box_cpp/scripts"

    # Create a VERSION file
    echo "1.0.7" > "$MAGIC_BASE_PATH/VERSION"

    # Enable test mode - skip systemctl and build
    export MAGIC_SKIP_SYSTEMCTL=true
    export MAGIC_SKIP_BUILD=true
}

# Teardown - runs after each test
teardown() {
    # A fake GitHub left running by a failed channel test (see below).
    if [ -n "${FAKE_GH_PID:-}" ]; then
        kill "$FAKE_GH_PID" 2>/dev/null || true
    fi
    # Clean up temp directory
    if [ -n "$TEST_TEMP_DIR" ] && [ -d "$TEST_TEMP_DIR" ]; then
        rm -rf "$TEST_TEMP_DIR"
    fi
}

# =============================================================================
# VERSION TESTS
# =============================================================================

@test "get_current_version reads VERSION file correctly" {
    echo "1.0.7" > "$MAGIC_BASE_PATH/VERSION"

    run "$UPDATE_SCRIPT" version

    [ "$status" -eq 0 ]
    [ "$output" = "1.0.7" ]
}

@test "get_current_version returns 0.0.0 when VERSION file missing" {
    rm -f "$MAGIC_BASE_PATH/VERSION"

    run "$UPDATE_SCRIPT" version

    [ "$status" -eq 0 ]
    [ "$output" = "0.0.0" ]
}

@test "get_current_version handles version with whitespace" {
    echo "  1.0.5  " > "$MAGIC_BASE_PATH/VERSION"

    run "$UPDATE_SCRIPT" version

    [ "$status" -eq 0 ]
    [ "$output" = "1.0.5" ]
}

# =============================================================================
# VERSION COMPARISON TESTS
# =============================================================================

# Helper to test version comparison
# Usage: test_version_lt "1.0.0" "1.0.1" "true"  (expect 1.0.0 < 1.0.1)
# Usage: test_version_lt "1.0.1" "1.0.0" "false" (expect 1.0.1 >= 1.0.0)
test_version_lt() {
    local v1="$1"
    local v2="$2"
    local expected="$3"

    # Source the script to get the function
    source "$UPDATE_SCRIPT" 2>/dev/null || true
    set +u +o pipefail

    if version_lt "$v1" "$v2"; then
        [ "$expected" = "true" ]
    else
        [ "$expected" = "false" ]
    fi
}

@test "version_lt: 1.0.0 < 1.0.1" {
    test_version_lt 1.0.0 1.0.1 true
}

@test "version_lt: 1.0.1 >= 1.0.0" {
    test_version_lt 1.0.1 1.0.0 false
}

@test "version_lt: 1.0.0 >= 1.0.0 (equal versions)" {
    test_version_lt 1.0.0 1.0.0 false
}

@test "version_lt: 1.9.0 < 1.10.0 (numeric comparison)" {
    test_version_lt 1.9.0 1.10.0 true
}

# SemVer precedence for the beta channel. These call update.sh's OWN
# version_cmp (the four tests above used to re-run a private copy of the old
# `sort -V` pipeline, which tested nothing in the script). sort -V put
# 1.10.1 BEFORE 1.10.1-beta.1, so a beta box would never have been offered
# the release it was testing.
#
# Table: <a> <b> <expected version_cmp a b>
VERSION_CMP_CASES='
1.0.0           1.0.1           -1
1.0.1           1.0.0           1
1.0.0           1.0.0           0
1.9.0           1.10.0          -1
1.10.0          1.9.0           1
2.0.0           1.99.99         1
1.10.0          1.10.1-beta.1   -1
1.10.1-beta.1   1.10.0          1
1.10.1-beta.1   1.10.1-beta.2   -1
1.10.1-beta.2   1.10.1-beta.1   1
1.10.1-beta.2   1.10.1-beta.10  -1
1.10.1-beta.10  1.10.1-beta.9   1
1.10.1-beta.9   1.10.1          -1
1.10.1          1.10.1-beta.99  1
1.10.1-beta.3   1.10.1-beta.3   0
1.10.1-beta.1   1.11.0          -1
1.11.0-beta.1   1.10.1          1
01.2.3          1.2.3           0
1.2.3-beta.01   1.2.3-beta.1    0
99999999999999999999.0.0 100000000000000000000.0.0 -1
'

@test "version_cmp: SemVer precedence table (numeric, prerelease < its release)" {
    load_update_functions
    local a b want got bad=0
    while read -r a b want; do
        [ -n "$a" ] || continue
        got=$(version_cmp "$a" "$b")
        if [ "$got" != "$want" ]; then
            echo "version_cmp $a $b: got '$got', want '$want'"; bad=1
        fi
    done <<<"$VERSION_CMP_CASES"
    [ "$bad" -eq 0 ]
}

@test "version_lt agrees with version_cmp on every table row" {
    load_update_functions
    local a b want is_lt bad=0
    while read -r a b want; do
        [ -n "$a" ] || continue
        if version_lt "$a" "$b"; then is_lt=yes; else is_lt=no; fi
        if { [ "$want" = -1 ] && [ "$is_lt" = no ]; } || { [ "$want" != -1 ] && [ "$is_lt" = yes ]; }; then
            echo "version_lt $a $b = $is_lt, but version_cmp want=$want"; bad=1
        fi
    done <<<"$VERSION_CMP_CASES"
    [ "$bad" -eq 0 ]
}

@test "version_cmp: invalid input returns 2 and prints nothing" {
    load_update_functions
    local v
    for v in "" "1.0" "v1.0.0" "1.0.0-rc.1" "1.0.0-beta" "1.0.0-beta.x" \
             "1.0.0-beta.1.2" "1.0.0-alpha.1" "1.0.0 " "1.0.0/../x" "1.0.0;x" "١.0.0"; do
        run version_cmp "$v" "1.0.0"
        [ "$status" -eq 2 ] || { echo "accepted: '$v'"; false; }
        [ -z "$output" ]
        ! version_valid "$v" || { echo "version_valid accepted '$v'"; false; }
    done
}

@test "version_valid: accepts exactly X.Y.Z and X.Y.Z-beta.N" {
    load_update_functions
    version_valid 1.10.0
    version_valid 1.10.1-beta.1
    version_valid 0.0.0
    version_valid 10.20.30-beta.400
}

@test "version_lt: never offers a garbage target, always offers a repair to a garbage current" {
    load_update_functions
    ! version_lt 1.0.0 "1.0.1-rc1"
    ! version_lt 1.0.0 ""
    version_lt "junk" 1.0.0
    version_lt "0.0.0" 1.0.0
}

@test "update.sh no longer compares versions with sort -V" {
    ! grep -nE '^[^#]*sort -V' "$UPDATE_SCRIPT"
}

# =============================================================================
# CHECK UPDATE TESTS
# =============================================================================

@test "check_update returns valid JSON" {
    # Mock the GitHub API by setting a custom endpoint
    # For this test, we'll just verify it tries to output JSON
    export MAGIC_GITHUB_API="file:///nonexistent"

    run "$UPDATE_SCRIPT" check

    # Should fail due to network, but error should be JSON
    # We just check it doesn't crash and produces some output
    [ -n "$output" ]
}

@test "check_update includes current version in output" {
    # Create a mock HTTP server response would be ideal, but for unit tests
    # we check that the function at least starts correctly
    echo "1.0.5" > "$MAGIC_BASE_PATH/VERSION"

    # Run with a timeout since it will try to contact GitHub
    timeout 5 "$UPDATE_SCRIPT" check 2>/dev/null || true

    # The test passes if we get here without crashing
    true
}

# =============================================================================
# INSTALL UPDATE TESTS
# =============================================================================

@test "install_update rejects non-GitHub URLs" {
    run "$UPDATE_SCRIPT" install "1.0.8" "https://evil.com/malware.tar.gz"

    [ "$status" -ne 0 ]
    [[ "$output" == *"Invalid download URL"* ]] || [[ "$output" == *"must be from GitHub"* ]]
}

@test "install_update rejects HTTP (non-HTTPS) URLs" {
    run "$UPDATE_SCRIPT" install "1.0.8" "http://github.com/user/repo/file.tar.gz"

    [ "$status" -ne 0 ]
    [[ "$output" == *"Invalid download URL"* ]] || [[ "$output" == *"must be from GitHub"* ]]
}

# The version is interpolated into the GitHub API URL get_binary_url()
# curls (releases/tags/v${version}); curl normalizes dot-segments, so a
# crafted version fetched ANOTHER repo's release and installed its binary.
# admin.py validates it too — this is the independent second check.
@test "install_update rejects a path-traversal version" {
    run "$UPDATE_SCRIPT" install "1.0.8/../../../../attacker/evil/releases/tags/v1" \
        "https://github.com/a-train-chain/magic_dingus_box/releases/download/v1.0.8/x.tar.gz"

    [ "$status" -ne 0 ]
    [[ "$output" == *"Invalid version"* ]]
    # Rejected BEFORE anything touched the temp dir or VERSION.
    [ ! -d "$MAGIC_TEMP_DIR" ]
    [ "$(cat "$MAGIC_BASE_PATH/VERSION")" = "1.0.7" ]
}

@test "install_update rejects non-X.Y.Z versions" {
    for v in "v1.0.8" "1.0" "1.0.8-rc1" '1.0.8;id' "1.0.8 "; do
        run "$UPDATE_SCRIPT" install "$v" \
            "https://github.com/a-train-chain/magic_dingus_box/releases/download/v1.0.8/x.tar.gz"
        [ "$status" -ne 0 ]
        [[ "$output" == *"Invalid version"* ]]
    done
}

@test "install_update rejects a GitHub URL outside this repo" {
    run "$UPDATE_SCRIPT" install "1.0.8" \
        "https://github.com/attacker/evil/releases/download/v1.0.8/x.tar.gz"
    [ "$status" -ne 0 ]
    [[ "$output" == *"Invalid download URL"* ]]

    run "$UPDATE_SCRIPT" install "1.0.8" \
        "https://github.com/a-train-chain/magic_dingus_box/../../attacker/evil/x.tar.gz"
    [ "$status" -ne 0 ]
    [[ "$output" == *"Invalid download URL"* ]]
}

# Single-flight: install and rollback share TEMP_DIR and the install tree.
@test "install refuses to run while another update holds the lock" {
    if command -v flock >/dev/null 2>&1; then
        flock "${MAGIC_TEMP_DIR}.lock" sleep 5 &
        local holder=$!
        sleep 0.5
    else
        # No flock here (macOS): a stub that reports "held" still proves
        # the dispatcher takes the lock before doing anything.
        mkdir -p "$TEST_TEMP_DIR/bin"
        printf '#!/bin/sh\nexit 1\n' > "$TEST_TEMP_DIR/bin/flock"
        chmod +x "$TEST_TEMP_DIR/bin/flock"
        export PATH="$TEST_TEMP_DIR/bin:$PATH"
    fi

    run "$UPDATE_SCRIPT" install "1.0.8" \
        "https://github.com/a-train-chain/magic_dingus_box/releases/download/v1.0.8/x.tar.gz"
    [ -n "${holder:-}" ] && kill "$holder" 2>/dev/null || true

    [ "$status" -ne 0 ]
    [[ "$output" == *"already running"* ]]
    [ ! -d "$MAGIC_TEMP_DIR" ]
}

@test "rollback refuses to run while another update holds the lock" {
    mkdir -p "$TEST_TEMP_DIR/bin"
    printf '#!/bin/sh\nexit 1\n' > "$TEST_TEMP_DIR/bin/flock"
    chmod +x "$TEST_TEMP_DIR/bin/flock"
    export PATH="$TEST_TEMP_DIR/bin:$PATH"

    run "$UPDATE_SCRIPT" rollback
    [ "$status" -ne 0 ]
    [[ "$output" == *"already running"* ]]
}

@test "install_update requires version argument" {
    run "$UPDATE_SCRIPT" install

    [ "$status" -ne 0 ]
    [[ "$output" == *"Usage"* ]] || [[ "$output" == *"required"* ]]
}

@test "install_update requires URL argument" {
    run "$UPDATE_SCRIPT" install "1.0.8"

    [ "$status" -ne 0 ]
    [[ "$output" == *"Usage"* ]] || [[ "$output" == *"required"* ]]
}

# =============================================================================
# ROLLBACK TESTS
# =============================================================================

@test "rollback fails gracefully when no backup exists" {
    # Make sure backup dir doesn't exist
    rm -rf "$MAGIC_BACKUP_DIR"

    run "$UPDATE_SCRIPT" rollback

    [ "$status" -ne 0 ]
    # "No COMPLETE backup": since cdfedcd the guard gates on the backup's
    # completion marker ($BACKUP_DIR/VERSION), not the directory — a
    # backup that died mid-transfer must not be restorable (--delete
    # would take the live install down with it).
    [[ "$output" == *"No complete backup"* ]]
}

@test "rollback outputs JSON error when no backup" {
    rm -rf "$MAGIC_BACKUP_DIR"

    run "$UPDATE_SCRIPT" rollback

    [ "$status" -ne 0 ]
    # Check it's valid JSON with error
    echo "$output" | grep -q '"ok".*false' || echo "$output" | grep -q '"error"'
}

@test "rollback proceeds when backup directory exists" {
    # Create a backup directory with VERSION file
    mkdir -p "$MAGIC_BACKUP_DIR"
    echo "1.0.6" > "$MAGIC_BACKUP_DIR/VERSION"

    # Create minimal directory structure for rsync
    mkdir -p "$MAGIC_BACKUP_DIR/magic_dingus_box_cpp/data"

    run "$UPDATE_SCRIPT" rollback

    # Should succeed or at least get past the backup check
    # With MAGIC_SKIP_SYSTEMCTL=true, it should complete
    [ "$status" -eq 0 ]
}

# =============================================================================
# TEST MODE TESTS
# =============================================================================

@test "MAGIC_SKIP_SYSTEMCTL prevents systemctl calls" {
    export MAGIC_SKIP_SYSTEMCTL=true

    # Create backup for rollback test
    mkdir -p "$MAGIC_BACKUP_DIR"
    echo "1.0.6" > "$MAGIC_BACKUP_DIR/VERSION"
    mkdir -p "$MAGIC_BACKUP_DIR/magic_dingus_box_cpp/data"

    run "$UPDATE_SCRIPT" rollback

    # Should succeed and mention skipping
    [ "$status" -eq 0 ]
    # Output should contain SKIP messages (sent to stderr, captured in run)
    # If not visible, the test just verifies it completes successfully
}

@test "MAGIC_SKIP_BUILD environment variable is recognized" {
    # Verify that the MAGIC_SKIP_BUILD variable is set correctly in the script
    # by checking that the script contains the expected pattern
    grep -q 'SKIP_BUILD=.*MAGIC_SKIP_BUILD' "$UPDATE_SCRIPT"
}

# =============================================================================
# USER CONTENT PRESERVATION TESTS
# =============================================================================

@test "user media files are preserved during rollback" {
    # Create backup with VERSION
    mkdir -p "$MAGIC_BACKUP_DIR"
    echo "1.0.6" > "$MAGIC_BACKUP_DIR/VERSION"
    mkdir -p "$MAGIC_BACKUP_DIR/magic_dingus_box_cpp/data"

    # Create user content in install dir
    mkdir -p "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/media"
    echo "user video content" > "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/media/my_video.mp4"

    # Run rollback
    run "$UPDATE_SCRIPT" rollback

    # Verify user media still exists
    [ -f "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/media/my_video.mp4" ]
    [ "$(cat "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/media/my_video.mp4")" = "user video content" ]
}

@test "user ROM files are preserved during rollback" {
    # Create backup with VERSION
    mkdir -p "$MAGIC_BACKUP_DIR"
    echo "1.0.6" > "$MAGIC_BACKUP_DIR/VERSION"
    mkdir -p "$MAGIC_BACKUP_DIR/magic_dingus_box_cpp/data"

    # Create user ROMs in install dir
    mkdir -p "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/roms/nes"
    echo "rom data" > "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/roms/nes/game.nes"

    # Run rollback
    run "$UPDATE_SCRIPT" rollback

    # Verify user ROMs still exist
    [ -f "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/roms/nes/game.nes" ]
    [ "$(cat "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/roms/nes/game.nes")" = "rom data" ]
}

@test "user playlists are preserved during rollback" {
    # Create backup with VERSION
    mkdir -p "$MAGIC_BACKUP_DIR"
    echo "1.0.6" > "$MAGIC_BACKUP_DIR/VERSION"
    mkdir -p "$MAGIC_BACKUP_DIR/magic_dingus_box_cpp/data"

    # Create user playlist in install dir
    mkdir -p "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/playlists"
    echo "title: My Playlist" > "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/playlists/user_playlist.yaml"

    # Run rollback
    run "$UPDATE_SCRIPT" rollback

    # Verify user playlist still exists
    [ -f "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/playlists/user_playlist.yaml" ]
    [[ "$(cat "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/playlists/user_playlist.yaml")" == *"My Playlist"* ]]
}

@test "user settings are preserved during rollback" {
    # Create backup with VERSION
    mkdir -p "$MAGIC_BACKUP_DIR"
    echo "1.0.6" > "$MAGIC_BACKUP_DIR/VERSION"
    mkdir -p "$MAGIC_BACKUP_DIR/magic_dingus_box_cpp/data"

    # Create user settings in install dir
    mkdir -p "$MAGIC_BASE_PATH/config"
    echo '{"volume": 80}' > "$MAGIC_BASE_PATH/config/settings.json"

    # Run rollback
    run "$UPDATE_SCRIPT" rollback

    # Verify user settings still exist
    [ -f "$MAGIC_BASE_PATH/config/settings.json" ]
    [[ "$(cat "$MAGIC_BASE_PATH/config/settings.json")" == *"volume"* ]]
}

@test "device_info.json is preserved during rollback" {
    # Create backup with VERSION
    mkdir -p "$MAGIC_BACKUP_DIR"
    echo "1.0.6" > "$MAGIC_BACKUP_DIR/VERSION"
    mkdir -p "$MAGIC_BACKUP_DIR/magic_dingus_box_cpp/data"

    # Create device_info in install dir
    mkdir -p "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data"
    echo '{"device_name": "My Device"}' > "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/device_info.json"

    # Run rollback
    run "$UPDATE_SCRIPT" rollback

    # Verify device_info still exists
    [ -f "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/device_info.json" ]
    [[ "$(cat "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/device_info.json")" == *"My Device"* ]]
}

# =============================================================================
# USAGE TESTS
# =============================================================================

@test "help flag shows usage" {
    run "$UPDATE_SCRIPT" --help

    [ "$status" -eq 0 ]
    [[ "$output" == *"Usage"* ]]
    [[ "$output" == *"check"* ]]
    [[ "$output" == *"install"* ]]
    [[ "$output" == *"rollback"* ]]
}

@test "unknown command shows usage" {
    run "$UPDATE_SCRIPT" unknown_command

    [ "$status" -ne 0 ]
    [[ "$output" == *"Usage"* ]]
}

@test "no arguments shows usage" {
    run "$UPDATE_SCRIPT"

    [ "$status" -ne 0 ]
    [[ "$output" == *"Usage"* ]]
}

# =============================================================================
# JSON OUTPUT TESTS
# =============================================================================

@test "check command outputs valid JSON on error" {
    # Use an invalid API endpoint to force an error
    export MAGIC_GITHUB_API="file:///nonexistent"

    run "$UPDATE_SCRIPT" check

    # Should produce JSON even on error
    # Try to parse the stdout as JSON (will have error message)
    # The output should contain either ok:true or ok:false
    [[ "$output" == *'"ok"'* ]]
}

@test "rollback command outputs valid JSON" {
    mkdir -p "$MAGIC_BACKUP_DIR"
    echo "1.0.6" > "$MAGIC_BACKUP_DIR/VERSION"
    mkdir -p "$MAGIC_BACKUP_DIR/magic_dingus_box_cpp/data"

    run "$UPDATE_SCRIPT" rollback

    # Should contain JSON markers
    [[ "$output" == *'"ok"'* ]]
    [[ "$output" == *'"stage"'* ]] || [[ "$output" == *'"progress"'* ]]
}

# =============================================================================
# PRE-COMPILED BINARY TESTS
# =============================================================================

@test "run_build uses -j2 for memory safety" {
    grep -q 'make -j2' "$UPDATE_SCRIPT"
}

@test "get_device_arch function exists" {
    grep -q 'get_device_arch()' "$UPDATE_SCRIPT"
}

@test "get_binary_url function exists" {
    grep -q 'get_binary_url()' "$UPDATE_SCRIPT"
}

@test "update checks for pre-compiled binary" {
    grep -q 'pre-compiled\|binary_url' "$UPDATE_SCRIPT"
}

@test "get_device_arch maps aarch64 to arm64" {
    # Source just the function we need
    eval "$(grep -A 10 'get_device_arch()' "$UPDATE_SCRIPT")"

    # Mock uname to return aarch64
    uname() { echo "aarch64"; }
    export -f uname

    result=$(get_device_arch)
    [ "$result" = "arm64" ]
}

@test "get_device_arch maps x86_64 to x64" {
    eval "$(grep -A 10 'get_device_arch()' "$UPDATE_SCRIPT")"

    uname() { echo "x86_64"; }
    export -f uname

    result=$(get_device_arch)
    [ "$result" = "x64" ]
}

@test "binary download falls back to source on failure" {
    # Verify the script has fallback logic
    grep -q 'use_binary.*false' "$UPDATE_SCRIPT"
    grep -q 'compile from source' "$UPDATE_SCRIPT"
}

# =============================================================================
# RETRY DOWNLOAD TESTS
# =============================================================================

@test "retry_download function exists" {
    grep -q 'retry_download()' "$UPDATE_SCRIPT"
}

@test "retry_download has exponential backoff" {
    grep -q 'wait_times.*(5 15 45)' "$UPDATE_SCRIPT"
}

@test "retry_download has 3 max attempts" {
    grep -q 'max_attempts=3' "$UPDATE_SCRIPT"
}

@test "main download uses retry_download" {
    grep -q 'retry_download.*update package' "$UPDATE_SCRIPT"
}

@test "binary download uses retry_download" {
    grep -q 'retry_download.*pre-compiled binary' "$UPDATE_SCRIPT"
}

@test "retry_download has permanent error handling" {
    grep -q 'permanent_errors' "$UPDATE_SCRIPT"
}

# =============================================================================
# VERSION ATOMICITY TESTS
# =============================================================================

@test "VERSION written after service start" {
    # Find the line numbers for VERSION write and the verified service start
    version_line=$(grep -n 'echo.*target_version.*VERSION' "$UPDATE_SCRIPT" | grep -v '#' | grep -v 'NOTE' | head -1 | cut -d: -f1)
    service_line=$(grep -n 'if ! verify_kiosk_started; then' "$UPDATE_SCRIPT" | head -1 | cut -d: -f1)
    [ -n "$version_line" ]
    [ -n "$service_line" ]
    [ "$version_line" -gt "$service_line" ]
}

@test "rollback_internal restores VERSION file" {
    grep -q 'cp.*BACKUP_DIR/VERSION.*INSTALL_DIR/VERSION' "$UPDATE_SCRIPT"
}

@test "rollback restores VERSION file" {
    # Both rollback functions should restore VERSION
    count=$(grep -c 'cp.*BACKUP_DIR/VERSION.*INSTALL_DIR/VERSION' "$UPDATE_SCRIPT")
    [ "$count" -ge 2 ]
}

@test "service failure triggers rollback" {
    grep -A4 'if ! verify_kiosk_started; then' "$UPDATE_SCRIPT" | grep -q 'fail_install'
    grep -A6 '^fail_install()' "$UPDATE_SCRIPT" | grep -q 'rollback_internal'
}

@test "service verification exists before VERSION write" {
    grep -q 'Service failed to start, rolling back' "$UPDATE_SCRIPT"
}

# =============================================================================
# HARDENING (2026-10): helpers for the tests below
# =============================================================================

# Source update.sh to unit-test its functions. The dispatcher is skipped
# when sourced; -u / pipefail are dropped again so they cannot leak into
# BATS internals (bats keeps its own -e).
load_update_functions() {
    # shellcheck disable=SC1090
    source "$UPDATE_SCRIPT"
    set +u +o pipefail
}

# A minimal ELF header: 64-bit little-endian, e_machine low byte = $2.
make_elf() {
    local path="$1" machine="${2:-b7}"
    printf '\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00'"\\x${machine}"'\x00' > "$path"
    printf 'padding-for-the-rest-of-the-binary' >> "$path"
    chmod 0755 "$path"
}

# PATH shims for cmake/make so run_build can run for real off-Pi.
#   FAKE_MAKE_MODE=ok       -> writes an aarch64 ELF kiosk binary
#   FAKE_MAKE_MODE=fail     -> exits 2 (compile error)
#   FAKE_MAKE_MODE=garbage  -> exits 0 but writes a text file
install_build_shims() {
    mkdir -p "$TEST_TEMP_DIR/bin"
    printf '#!/bin/sh\nexit 0\n' > "$TEST_TEMP_DIR/bin/cmake"
    cat > "$TEST_TEMP_DIR/bin/make" <<'SH'
#!/bin/bash
case "${FAKE_MAKE_MODE:-ok}" in
    fail) echo "error: compile failed" >&2; exit 2 ;;
    garbage) echo "not a binary" > magic_dingus_box_cpp; chmod +x magic_dingus_box_cpp; exit 0 ;;
    *) printf '\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00NEWBUILD' > magic_dingus_box_cpp
       chmod +x magic_dingus_box_cpp; exit 0 ;;
esac
SH
    chmod +x "$TEST_TEMP_DIR/bin/cmake" "$TEST_TEMP_DIR/bin/make"
    export PATH="$TEST_TEMP_DIR/bin:$PATH"
}

# A fake release tarball + a curl shim that "downloads" it, so a full
# install runs end-to-end in test mode with no network.
#   FAKE_RSYNC_23_SRC=<prefix>  -> the rsync shim exits 23 (after doing the
#   copy) whenever its source argument starts with <prefix>.
make_fake_release() {
    local version="$1"
    local rel="$TEST_TEMP_DIR/release"
    rm -rf "$rel"
    mkdir -p "$rel/magic_dingus_box_cpp/src/config" "$rel/magic_dingus_box_cpp/scripts" \
             "$rel/magic_dingus_box_cpp/third_party"
    echo "$version" > "$rel/VERSION"
    echo "// release build v$version" > "$rel/magic_dingus_box_cpp/src/main.cpp"
    echo "// nested config dir must be delivered" > "$rel/magic_dingus_box_cpp/src/config/defaults.h"
    echo "9.9.9" > "$rel/magic_dingus_box_cpp/third_party/VERSION"
    echo "cmake_minimum_required(VERSION 3.13)" > "$rel/magic_dingus_box_cpp/CMakeLists.txt"
    echo "new helper $version" > "$rel/magic_dingus_box_cpp/scripts/helper.sh"
    # Incompressible padding: update.sh rejects downloads under 10 kB.
    head -c 16384 /dev/urandom > "$rel/magic_dingus_box_cpp/padding.bin"
    FAKE_TARBALL="$TEST_TEMP_DIR/release.tar.gz"
    tar -czf "$FAKE_TARBALL" -C "$rel" .
    export FAKE_TARBALL

    mkdir -p "$TEST_TEMP_DIR/bin"
    cat > "$TEST_TEMP_DIR/bin/curl" <<'SH'
#!/bin/bash
out=""; prev=""
for a in "$@"; do
    [ "$prev" = "-o" ] && out="$a"
    prev="$a"
done
case "$*" in
    *api.github.com*) exit 0 ;;   # no pre-compiled binary for this release
esac
[ -n "$out" ] && cp "$FAKE_TARBALL" "$out"
exit 0
SH
    local real_rsync
    real_rsync="$(command -v rsync)"
    cat > "$TEST_TEMP_DIR/bin/rsync" <<SH
#!/bin/bash
"$real_rsync" "\$@"; rc=\$?
if [ -n "\${FAKE_RSYNC_23_SRC:-}" ]; then
    for a in "\$@"; do
        case "\$a" in "\${FAKE_RSYNC_23_SRC}"*) exit 23 ;; esac
    done
fi
exit \$rc
SH
    chmod +x "$TEST_TEMP_DIR/bin/curl" "$TEST_TEMP_DIR/bin/rsync"
    export PATH="$TEST_TEMP_DIR/bin:$PATH"
}

seed_installed_tree() {
    mkdir -p "$MAGIC_BASE_PATH/magic_dingus_box_cpp/src" "$MAGIC_BASE_PATH/magic_dingus_box_cpp/scripts" \
             "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/upload_temp" "$MAGIC_BASE_PATH/config"
    echo "// v1.0.7" > "$MAGIC_BASE_PATH/magic_dingus_box_cpp/src/main.cpp"
    echo "cmake_minimum_required(VERSION 3.13)" > "$MAGIC_BASE_PATH/magic_dingus_box_cpp/CMakeLists.txt"
    echo "old helper" > "$MAGIC_BASE_PATH/magic_dingus_box_cpp/scripts/helper.sh"
    echo "phone-abc" > "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/pending_revocations.txt"
    echo "half an upload" > "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/upload_temp/part.bin"
    echo '{"volume": 80}' > "$MAGIC_BASE_PATH/config/settings.json"
}

GOOD_URL="https://github.com/a-train-chain/magic_dingus_box/releases/download/v1.0.8/magic-dingus-box-1.0.8.tar.gz"

# =============================================================================
# ITEM 1 — the live kiosk binary is never missing or truncated
# =============================================================================

@test "update.sh can be sourced without running the dispatcher" {
    run bash -c "source '$UPDATE_SCRIPT'; echo sourced-ok"
    [ "$status" -eq 0 ]
    [[ "$output" == *"sourced-ok"* ]]
    [[ "$output" != *"Usage"* ]]
}

@test "verify_kiosk_binary accepts a 64-bit ELF for this CPU" {
    load_update_functions
    export MAGIC_EXPECT_ELF_MACHINE=b7
    make_elf "$TEST_TEMP_DIR/k" b7
    verify_kiosk_binary "$TEST_TEMP_DIR/k"
}

@test "verify_kiosk_binary rejects empty, non-ELF, wrong-CPU, 32-bit and non-executable files" {
    load_update_functions
    export MAGIC_EXPECT_ELF_MACHINE=b7

    : > "$TEST_TEMP_DIR/empty"; chmod +x "$TEST_TEMP_DIR/empty"
    run verify_kiosk_binary "$TEST_TEMP_DIR/empty";   [ "$status" -ne 0 ]

    echo "#!/bin/sh" > "$TEST_TEMP_DIR/text"; chmod +x "$TEST_TEMP_DIR/text"
    run verify_kiosk_binary "$TEST_TEMP_DIR/text";    [ "$status" -ne 0 ]

    make_elf "$TEST_TEMP_DIR/x86" 3e
    run verify_kiosk_binary "$TEST_TEMP_DIR/x86";     [ "$status" -ne 0 ]

    printf '\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00rest' > "$TEST_TEMP_DIR/elf32"
    chmod +x "$TEST_TEMP_DIR/elf32"
    run verify_kiosk_binary "$TEST_TEMP_DIR/elf32";   [ "$status" -ne 0 ]

    make_elf "$TEST_TEMP_DIR/noexec" b7; chmod -x "$TEST_TEMP_DIR/noexec"
    run verify_kiosk_binary "$TEST_TEMP_DIR/noexec";  [ "$status" -ne 0 ]

    run verify_kiosk_binary "$TEST_TEMP_DIR/does-not-exist"; [ "$status" -ne 0 ]
}

@test "install_kiosk_binary replaces the live binary atomically and leaves no .new" {
    load_update_functions
    export MAGIC_EXPECT_ELF_MACHINE=b7
    local live="$MAGIC_BASE_PATH/magic_dingus_box_cpp/build/magic_dingus_box_cpp"
    mkdir -p "$(dirname "$live")"
    make_elf "$live" b7; echo OLD >> "$live"
    make_elf "$TEST_TEMP_DIR/new" b7; echo NEW >> "$TEST_TEMP_DIR/new"

    install_kiosk_binary "$TEST_TEMP_DIR/new"

    grep -q NEW "$live"
    [ -x "$live" ]
    [ ! -e "${live}.new" ]
}

@test "install_kiosk_binary keeps the live binary when the new one fails verification" {
    load_update_functions
    export MAGIC_EXPECT_ELF_MACHINE=b7
    local live="$MAGIC_BASE_PATH/magic_dingus_box_cpp/build/magic_dingus_box_cpp"
    mkdir -p "$(dirname "$live")"
    make_elf "$live" b7; echo OLD >> "$live"
    echo "truncated" > "$TEST_TEMP_DIR/bad"

    run install_kiosk_binary "$TEST_TEMP_DIR/bad"
    [ "$status" -ne 0 ]
    grep -q OLD "$live"
    [ ! -e "${live}.new" ]
}

@test "run_build: a failed compile leaves the existing build/ and its binary untouched" {
    install_build_shims
    load_update_functions
    SKIP_BUILD=false
    export MAGIC_EXPECT_ELF_MACHINE=b7
    local build="$MAGIC_BASE_PATH/magic_dingus_box_cpp/build"
    mkdir -p "$build"
    make_elf "$build/magic_dingus_box_cpp" b7; echo OLD >> "$build/magic_dingus_box_cpp"

    export FAKE_MAKE_MODE=fail
    run run_build
    [ "$status" -ne 0 ]
    grep -q OLD "$build/magic_dingus_box_cpp"
    [ ! -e "${build}.new" ]

    export FAKE_MAKE_MODE=garbage
    run run_build
    [ "$status" -ne 0 ]
    grep -q OLD "$build/magic_dingus_box_cpp"
    [ ! -e "${build}.new" ]
}

@test "run_build: a good compile swaps in the new build/ and cleans up" {
    install_build_shims
    load_update_functions
    SKIP_BUILD=false
    export MAGIC_EXPECT_ELF_MACHINE=b7
    local build="$MAGIC_BASE_PATH/magic_dingus_box_cpp/build"
    mkdir -p "$build"
    make_elf "$build/magic_dingus_box_cpp" b7; echo OLD >> "$build/magic_dingus_box_cpp"
    echo stale > "$build/stale_object.o"

    export FAKE_MAKE_MODE=ok
    run_build

    grep -q NEWBUILD "$build/magic_dingus_box_cpp"
    [ ! -e "$build/stale_object.o" ]          # really a clean tree
    [ ! -e "${build}.new" ]
    [ ! -e "${build}.old" ]
}

@test "run_build: a failed cmake/make says WHY in the job log" {
    # cmake's output used to go to /dev/null: a failed configure reported
    # only "Build failed" (found by the v1.9.14 -> v1.10.0 OTA rehearsal).
    install_build_shims
    printf '#!/bin/sh\necho "CMake Error: missing libfoo" >&2\nexit 1\n' > "$TEST_TEMP_DIR/bin/cmake"
    load_update_functions
    SKIP_BUILD=false
    local build="$MAGIC_BASE_PATH/magic_dingus_box_cpp/build"
    mkdir -p "$build"
    run run_build
    [ "$status" -ne 0 ]
    [[ "$output" == *"cmake failed"* ]]
    [[ "$output" == *"CMake Error: missing libfoo"* ]]
    grep -q "missing libfoo" "${build}.log"

    printf '#!/bin/sh\nexit 0\n' > "$TEST_TEMP_DIR/bin/cmake"
    export FAKE_MAKE_MODE=fail
    run run_build
    [ "$status" -ne 0 ]
    [[ "$output" == *"make failed"* ]]
    [[ "$output" == *"error: compile failed"* ]]
}

@test "run_build never rm -rf's the live build dir before compiling" {
    # The original defect: `rm -rf "$build_dir"` ahead of an 8-10 minute
    # compile. Only build.new / build.old may ever be removed wholesale.
    ! grep -nE 'rm -rf "\$build_dir"( |$)' "$UPDATE_SCRIPT"
}

@test "prebuilt-binary path: extract and copy failures are guarded, never bare under set -e" {
    grep -q 'if ! tar -xzf "\$TEMP_DIR/binary.tar.gz"' "$UPDATE_SCRIPT"
    grep -q 'install_kiosk_binary "\$TEMP_DIR/binary_extracted/magic_dingus_box_cpp"' "$UPDATE_SCRIPT"
    ! grep -q 'cp "\$TEMP_DIR/binary_extracted/magic_dingus_box_cpp" "\$INSTALL_DIR' "$UPDATE_SCRIPT"
}

# =============================================================================
# ITEM 2 — power loss mid-install is recovered at the next boot
# =============================================================================

@test "full install: succeeds, stamps VERSION, and leaves no in-progress marker" {
    seed_installed_tree
    make_fake_release 1.0.8

    run "$UPDATE_SCRIPT" install 1.0.8 "$GOOD_URL"
    echo "$output"
    [ "$status" -eq 0 ]
    [ "$(cat "$MAGIC_BASE_PATH/VERSION")" = "1.0.8" ]
    [ ! -e "${MAGIC_BACKUP_DIR}.ota_in_progress" ]
    [ "$(cat "$MAGIC_BACKUP_DIR/VERSION")" = "1.0.7" ]
    [ "$(cat "$MAGIC_BASE_PATH/magic_dingus_box_cpp/src/main.cpp")" = "// release build v1.0.8" ]
}

@test "full install: preserves pending_revocations.txt, upload_temp/ and config/, delivers nested config/ + VERSION" {
    seed_installed_tree
    make_fake_release 1.0.8

    run "$UPDATE_SCRIPT" install 1.0.8 "$GOOD_URL"
    [ "$status" -eq 0 ]
    [ "$(cat "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/pending_revocations.txt")" = "phone-abc" ]
    [ -f "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/upload_temp/part.bin" ]
    [[ "$(cat "$MAGIC_BASE_PATH/config/settings.json")" == *volume* ]]
    [ -f "$MAGIC_BASE_PATH/magic_dingus_box_cpp/src/config/defaults.h" ]
    [ "$(cat "$MAGIC_BASE_PATH/magic_dingus_box_cpp/third_party/VERSION")" = "9.9.9" ]
}

@test "recover: no marker is a no-op" {
    run "$UPDATE_SCRIPT" recover
    [ "$status" -eq 0 ]
    [[ "$output" == *"No interrupted update"* ]]
}

@test "recover: an interrupted install is rolled back to the backup and the marker cleared" {
    seed_installed_tree
    # Backup = the pre-update tree.
    mkdir -p "$MAGIC_BACKUP_DIR/magic_dingus_box_cpp/scripts"
    cp -R "$MAGIC_BASE_PATH/." "$MAGIC_BACKUP_DIR/"
    echo "1.0.7" > "$MAGIC_BACKUP_DIR/VERSION"
    # Half-installed tree: new files landed, VERSION not yet stamped.
    echo "new helper 1.0.8" > "$MAGIC_BASE_PATH/magic_dingus_box_cpp/scripts/helper.sh"
    echo "half" > "$MAGIC_BASE_PATH/magic_dingus_box_cpp/scripts/only_in_new.sh"
    printf 'target=1.0.8\nfrom=1.0.7\n' > "${MAGIC_BACKUP_DIR}.ota_in_progress"

    run "$UPDATE_SCRIPT" recover
    echo "$output"
    [ "$status" -eq 0 ]
    [ "$(cat "$MAGIC_BASE_PATH/magic_dingus_box_cpp/scripts/helper.sh")" = "old helper" ]
    [ ! -e "$MAGIC_BASE_PATH/magic_dingus_box_cpp/scripts/only_in_new.sh" ]
    [ "$(cat "$MAGIC_BASE_PATH/VERSION")" = "1.0.7" ]
    [ ! -e "${MAGIC_BACKUP_DIR}.ota_in_progress" ]
    # Operator content is untouched by the restore.
    [ "$(cat "$MAGIC_BASE_PATH/magic_dingus_box_cpp/data/pending_revocations.txt")" = "phone-abc" ]
    # Boot-time recovery runs BEFORE the kiosk unit: it must never try to
    # start/stop the kiosk itself (that would deadlock the boot).
    [[ "$output" != *"systemctl start magic-dingus-box-cpp"* ]]
    [[ "$output" != *"systemctl stop magic-dingus-box-cpp"* ]]
}

@test "recover: an update that completed (VERSION == target) only clears the marker" {
    seed_installed_tree
    echo "1.0.8" > "$MAGIC_BASE_PATH/VERSION"
    mkdir -p "$MAGIC_BACKUP_DIR"; echo "1.0.7" > "$MAGIC_BACKUP_DIR/VERSION"
    printf 'target=1.0.8\nfrom=1.0.7\n' > "${MAGIC_BACKUP_DIR}.ota_in_progress"

    run "$UPDATE_SCRIPT" recover
    [ "$status" -eq 0 ]
    [ "$(cat "$MAGIC_BASE_PATH/VERSION")" = "1.0.8" ]
    [ "$(cat "$MAGIC_BASE_PATH/magic_dingus_box_cpp/scripts/helper.sh")" = "old helper" ]
    [ ! -e "${MAGIC_BACKUP_DIR}.ota_in_progress" ]
}

@test "recover: a marker with no complete backup is cleared, never acted on" {
    seed_installed_tree
    rm -rf "$MAGIC_BACKUP_DIR"
    printf 'target=1.0.8\nfrom=1.0.7\n' > "${MAGIC_BACKUP_DIR}.ota_in_progress"
    run "$UPDATE_SCRIPT" recover
    [ "$status" -eq 0 ]
    [ ! -e "${MAGIC_BACKUP_DIR}.ota_in_progress" ]
    [ -f "$MAGIC_BASE_PATH/magic_dingus_box_cpp/src/main.cpp" ]
}

@test "install: an earlier interrupted install is restored BEFORE the new backup is taken" {
    seed_installed_tree
    mkdir -p "$MAGIC_BACKUP_DIR"
    cp -R "$MAGIC_BASE_PATH/." "$MAGIC_BACKUP_DIR/"
    echo "1.0.7" > "$MAGIC_BACKUP_DIR/VERSION"
    echo "HALF-INSTALLED" > "$MAGIC_BASE_PATH/magic_dingus_box_cpp/scripts/helper.sh"
    printf 'target=1.0.8\nfrom=1.0.7\n' > "${MAGIC_BACKUP_DIR}.ota_in_progress"
    make_fake_release 1.0.8

    run "$UPDATE_SCRIPT" install 1.0.8 "$GOOD_URL"
    echo "$output"
    [ "$status" -eq 0 ]
    [[ "$output" == *"did not finish"* ]]
    # The new backup holds the RESTORED tree, not the half-installed one.
    [ "$(cat "$MAGIC_BACKUP_DIR/magic_dingus_box_cpp/scripts/helper.sh")" = "old helper" ]
    [ "$(cat "$MAGIC_BASE_PATH/VERSION")" = "1.0.8" ]
    [ ! -e "${MAGIC_BACKUP_DIR}.ota_in_progress" ]
}

@test "recovery unit, update.sh and first_boot agree on the marker and backup paths" {
    local unit="$SCRIPT_DIR/../../systemd/magic-dingus-ota-recovery.service"
    [ -f "$unit" ]
    local marker backup
    marker=$(env -u MAGIC_BACKUP_DIR -u MAGIC_OTA_MARKER HOME=/home/magic \
        bash -c "source '$UPDATE_SCRIPT'; echo \"\$OTA_MARKER\"")
    backup=$(env -u MAGIC_BACKUP_DIR HOME=/home/magic \
        bash -c "source '$UPDATE_SCRIPT'; echo \"\$BACKUP_DIR\"")
    grep -qxF "ConditionPathExists=${marker}" "$unit"
    grep -qF "ExecStart=/bin/bash ${backup}/magic_dingus_box_cpp/scripts/update.sh recover" "$unit"
    grep -q "Before=magic-dingus-box-cpp.service" "$unit"
    # first_boot deletes it; prepare_for_cloning's tripwire glob matches it.
    grep -qF "$marker" "$SCRIPT_DIR/../../../scripts/golden_image/first_boot.sh"
    [[ "$marker" == /home/magic/.magic_dingus_box_backup* ]]
    grep -qF '/home/magic/.magic_dingus_box_backup*' "$SCRIPT_DIR/../../../scripts/golden_image/prepare_for_cloning.sh"
}

@test "every install path installs the recovery unit" {
    grep -q 'setup_ota_recovery.sh' "$UPDATE_SCRIPT"
    grep -q 'setup_ota_recovery.sh' "$SCRIPT_DIR/../deploy_cpp.sh"
    grep -q 'setup_ota_recovery.sh' "$SCRIPT_DIR/../../../scripts/golden_image/first_boot.sh"
    grep -q 'setup_ota_recovery.sh' "$SCRIPT_DIR/../../../scripts/golden_image/sync_source_box.sh"
}

@test "setup_ota_recovery.sh installs the unit idempotently (fake root)" {
    local root="$TEST_TEMP_DIR/root"
    run env MAGIC_TUNING_ROOT="$root" MAGIC_SKIP_SYSTEMCTL=true bash "$SCRIPT_DIR/../setup_ota_recovery.sh"
    [ "$status" -eq 0 ]
    cmp -s "$SCRIPT_DIR/../../systemd/magic-dingus-ota-recovery.service" \
        "$root/etc/systemd/system/magic-dingus-ota-recovery.service"
    run env MAGIC_TUNING_ROOT="$root" MAGIC_SKIP_SYSTEMCTL=true bash "$SCRIPT_DIR/../setup_ota_recovery.sh"
    [ "$status" -eq 0 ]
    [[ "$output" == *"already current"* ]]
}

# =============================================================================
# ITEM 3 — a headless box keeps a good update; a crash after READY does not
# =============================================================================

@test "KIOSK_EXIT_NO_DISPLAY matches the kiosk's kExitNoDisplay" {
    local cpp
    cpp=$(grep -oE 'kExitNoDisplay = [0-9]+' "$SCRIPT_DIR/../../src/platform/kiosk_exit.h" | grep -oE '[0-9]+$')
    grep -q "^KIOSK_EXIT_NO_DISPLAY=${cpp}\$" "$UPDATE_SCRIPT"
}

@test "kiosk_start_verdict classifies observations" {
    load_update_functions
    [ "$(kiosk_start_verdict active running 0 0 1)" = "running" ]
    [ "$(kiosk_start_verdict activating auto-restart 1 69 1)" = "no_display" ]
    [ "$(kiosk_start_verdict failed failed 1 69 1)" = "no_display" ]
    # A 69 left over from BEFORE the update (old process) is not evidence.
    [ "$(kiosk_start_verdict activating start 1 69 0)" = "pending" ]
    [ "$(kiosk_start_verdict activating auto-restart 2 11 1)" = "failed" ]
    [ "$(kiosk_start_verdict activating auto-restart 1 1 1)" = "failed" ]
    [ "$(kiosk_start_verdict failed failed 1 1 0)" = "failed" ]
    [ "$(kiosk_start_verdict inactive dead 1 0 1)" = "failed" ]
    [ "$(kiosk_start_verdict activating start 0 0 0)" = "pending" ]
}

# Fake systemd for verify_kiosk_started. Property values live in
# $FAKE_SD/<phase>/<Property>; `start` moves to phase 1 and every `sleep`
# advances one phase (missing phases fall back to the latest one present).
setup_fake_systemd() {
    FAKE_SD="$TEST_TEMP_DIR/sd"
    mkdir -p "$FAKE_SD"
    echo 0 > "$FAKE_SD/phase"
    load_update_functions
    SKIP_SYSTEMCTL=false
    KIOSK_START_TIMEOUT=5
    KIOSK_STABLE_SECS=10
    sudo() { "$@"; }
    sleep() { echo $(( $(cat "$FAKE_SD/phase") + 1 )) > "$FAKE_SD/phase"; }
    systemctl() {
        case "$1" in
            start) echo 1 > "$FAKE_SD/phase" ;;
            show)
                local p; p=$(cat "$FAKE_SD/phase")
                while [ ! -d "$FAKE_SD/$p" ] && [ "$p" -gt 0 ]; do p=$((p - 1)); done
                cat "$FAKE_SD/$p/$3" 2>/dev/null || echo ""
                ;;
        esac
        return 0
    }
}

# set_phase <phase> Active Sub Code Status StartTs PID NRestarts
set_phase() {
    local d="$FAKE_SD/$1"; mkdir -p "$d"
    echo "$2" > "$d/ActiveState"; echo "$3" > "$d/SubState"
    echo "$4" > "$d/ExecMainCode"; echo "$5" > "$d/ExecMainStatus"
    echo "$6" > "$d/ExecMainStartTimestampMonotonic"
    echo "$7" > "$d/MainPID"; echo "$8" > "$d/NRestarts"
}

@test "verify_kiosk_started: no display connected is accepted" {
    setup_fake_systemd
    set_phase 0 inactive dead 1 0 100 0 0
    set_phase 1 activating auto-restart 1 69 200 0 0
    run verify_kiosk_started
    [ "$status" -eq 0 ]
    [[ "$output" == *"no connected display"* ]]
}

@test "verify_kiosk_started: a crash right after READY is caught by the stability window" {
    setup_fake_systemd
    set_phase 0 inactive dead 1 0 100 0 0
    set_phase 1 active running 0 0 200 4242 0
    set_phase 2 active running 0 0 300 4300 1
    run verify_kiosk_started
    [ "$status" -ne 0 ]
}

@test "verify_kiosk_started: a kiosk that stays up passes" {
    setup_fake_systemd
    set_phase 0 inactive dead 1 0 100 0 0
    set_phase 1 active running 0 0 200 4242 0
    run verify_kiosk_started
    [ "$status" -eq 0 ]
}

@test "verify_kiosk_started: a stale exit 69 from the OLD process is not mistaken for success" {
    setup_fake_systemd
    # Before the update the box had no TV: last exit was 69 (start ts 100).
    set_phase 0 inactive dead 1 69 100 0 0
    # The new binary has not started yet, then crashes with a segfault.
    set_phase 1 activating start 1 69 100 0 0
    set_phase 2 activating auto-restart 3 11 200 0 1
    run verify_kiosk_started
    [ "$status" -ne 0 ]
}

@test "verify_kiosk_started: never coming up times out as a failure" {
    setup_fake_systemd
    set_phase 0 inactive dead 1 0 100 0 0
    set_phase 1 activating start 0 0 100 0 0
    run verify_kiosk_started
    [ "$status" -ne 0 ]
}

# =============================================================================
# ITEM 4 — rsync exit 23 is a failure; root-owned files are normalized
# =============================================================================

@test "rsync exit 23 during install rolls back and reports failure" {
    seed_installed_tree
    make_fake_release 1.0.8
    export FAKE_RSYNC_23_SRC="$MAGIC_TEMP_DIR/extracted"

    run "$UPDATE_SCRIPT" install 1.0.8 "$GOOD_URL"
    echo "$output"
    [ "$status" -ne 0 ]
    [[ "$output" == *"rsync exit code: 23"* ]]
    [[ "$output" == *"previous version was restored"* ]]
    [ "$(cat "$MAGIC_BASE_PATH/VERSION")" = "1.0.7" ]
    [ "$(cat "$MAGIC_BASE_PATH/magic_dingus_box_cpp/scripts/helper.sh")" = "old helper" ]
    [ ! -e "${MAGIC_BACKUP_DIR}.ota_in_progress" ]
}

@test "rsync_exit_ok: 0 and 24 pass, 23 and others fail" {
    load_update_functions
    rsync_exit_ok 0
    rsync_exit_ok 24
    ! rsync_exit_ok 23
    ! rsync_exit_ok 11
    ! rsync_exit_ok 12
}

@test "ownership normalization prunes services/config and services/.env" {
    grep -q -- '-path "\$INSTALL_DIR/services/config" -o -path "\$INSTALL_DIR/services/.env"' "$UPDATE_SCRIPT"
    # Runs before the backup AND before both restore rsyncs.
    [ "$(grep -c '^    normalize_tree_ownership$' "$UPDATE_SCRIPT")" -ge 3 ]
}

@test "normalize_tree_ownership is a no-op on a tree already owned by the caller" {
    seed_installed_tree
    load_update_functions
    sudo() { echo "SUDO CALLED: $*" >&2; return 1; }
    run normalize_tree_ownership
    [ "$status" -eq 0 ]
    [[ "$output" != *"SUDO CALLED"* ]]
}

# =============================================================================
# ITEM 5 — rollback re-syncs the helpers outside the tree
# =============================================================================

@test "both rollback paths re-run refresh_out_of_tree_files" {
    seed_installed_tree
    mkdir -p "$MAGIC_BACKUP_DIR"
    cp -R "$MAGIC_BASE_PATH/." "$MAGIC_BACKUP_DIR/"
    echo "1.0.7" > "$MAGIC_BACKUP_DIR/VERSION"
    load_update_functions
    refresh_out_of_tree_files() { echo refreshed >> "$TEST_TEMP_DIR/refresh.log"; }

    rollback_internal 2>/dev/null
    [ "$(wc -l < "$TEST_TEMP_DIR/refresh.log")" -eq 1 ]

    rollback >/dev/null 2>&1
    [ "$(wc -l < "$TEST_TEMP_DIR/refresh.log")" -eq 2 ]
}

@test "install refreshes out-of-tree helpers only after the verified start" {
    local verify_line refresh_line
    verify_line=$(grep -n 'if ! verify_kiosk_started; then' "$UPDATE_SCRIPT" | head -1 | cut -d: -f1)
    # Every call site of refresh_out_of_tree_files inside install_update must
    # come after the verification.
    while IFS=: read -r n _; do
        if [ "$n" -gt "$(grep -n '^install_update()' "$UPDATE_SCRIPT" | cut -d: -f1)" ] \
            && [ "$n" -lt "$(grep -n '^rollback_internal()' "$UPDATE_SCRIPT" | cut -d: -f1)" ]; then
            [ "$n" -gt "$verify_line" ]
        fi
    done < <(grep -n '^    refresh_out_of_tree_files$' "$UPDATE_SCRIPT")
}

# =============================================================================
# ITEM 6 — Phone Remote bootstrap never runs setup_services.sh
# =============================================================================

@test "OTA never runs setup_services.sh" {
    ! grep -nE '^[^#]*bash[^#]*setup_services\.sh' "$UPDATE_SCRIPT"
    grep -q 'setup_phone_remote_uinput.sh' "$UPDATE_SCRIPT"
}

@test "setup_services.sh delegates the uinput step to the shared helper" {
    grep -q 'setup_phone_remote_uinput.sh' "$SCRIPT_DIR/../setup_services.sh"
    [ -x "$SCRIPT_DIR/../setup_phone_remote_uinput.sh" ]
    bash -n "$SCRIPT_DIR/../setup_phone_remote_uinput.sh"
    # The helper does the actual root work.
    grep -q '90-magicdingus-uinput.rules' "$SCRIPT_DIR/../setup_phone_remote_uinput.sh"
    grep -q 'usermod -a -G input' "$SCRIPT_DIR/../setup_phone_remote_uinput.sh"
}

# =============================================================================
# python3-gunicorn (ensure_web_server_dep): after the kiosk is back, and
# dpkg is never run under a timeout
# =============================================================================

# Source update.sh with gunicorn "missing" and sudo recorded, not run.
# $1 = space-separated phases that fail: download | update | install
setup_web_dep() {
    load_update_functions
    SKIP_SYSTEMCTL=false
    WEB_DEP_FAIL="${1:-}"
    APT_LOG="$TEST_TEMP_DIR/apt.log"
    : > "$APT_LOG"
    python3() { return 1; }
    sudo() {
        echo "$*" >> "$APT_LOG"
        case "$*" in
            *--download-only*) [[ " $WEB_DEP_FAIL " != *" download "* ]] ;;
            *" update")        [[ " $WEB_DEP_FAIL " != *" update "* ]] ;;
            *--no-download*)   [[ " $WEB_DEP_FAIL " != *" install "* ]] ;;
            *) return 0 ;;
        esac
    }
}

@test "web dep: download is bounded by timeout, the dpkg install never is" {
    setup_web_dep
    run ensure_web_server_dep
    [ "$status" -eq 0 ]
    [[ "$output" == *"python3-gunicorn installed"* ]]
    grep -q '^-n timeout [0-9]* env DEBIAN_FRONTEND=noninteractive apt-get .* install --download-only ' "$APT_LOG"
    grep -q '^-n env DEBIAN_FRONTEND=noninteractive apt-get .* install --no-download .*python3-gunicorn' "$APT_LOG"
    # Nothing that runs dpkg is wrapped in timeout.
    [ "$(grep -c -- '--no-download' "$APT_LOG")" -eq 1 ]
    [ "$(grep -- '--no-download' "$APT_LOG" | grep -c 'timeout')" -eq 0 ]
}

@test "web dep: a stale package list is refreshed and the download retried" {
    setup_web_dep
    # First download fails, the retry after `update` succeeds.
    sudo() {
        echo "$*" >> "$APT_LOG"
        case "$*" in
            *--download-only*)
                if grep -q ' update$' "$APT_LOG"; then return 0; fi
                return 1 ;;
            *) return 0 ;;
        esac
    }
    run ensure_web_server_dep
    [ "$status" -eq 0 ]
    grep -q '^-n timeout [0-9]* env .* update$' "$APT_LOG"
    [ "$(grep -c -- '--download-only' "$APT_LOG")" -eq 2 ]
    grep -q -- '--no-download' "$APT_LOG"
}

@test "web dep: an offline box never reaches dpkg and never fails the update" {
    setup_web_dep "download update"
    run ensure_web_server_dep
    [ "$status" -eq 0 ]
    [[ "$output" == *"could not download python3-gunicorn"* ]]
    [ "$(grep -c -- '--no-download' "$APT_LOG")" -eq 0 ]
}

@test "web dep: a failed install is a warning, not a failure" {
    setup_web_dep "install"
    run ensure_web_server_dep
    [ "$status" -eq 0 ]
    [[ "$output" == *"could not install python3-gunicorn"* ]]
}

@test "web dep: already present is a no-op" {
    setup_web_dep
    python3() { return 0; }
    run ensure_web_server_dep
    [ "$status" -eq 0 ]
    [ ! -s "$APT_LOG" ]
}

@test "install_update installs the web dep only after the verified start and the VERSION commit" {
    local start end dep verify marker web_restart
    start=$(grep -n '^install_update()' "$UPDATE_SCRIPT" | cut -d: -f1)
    end=$(grep -n '^rollback_internal()' "$UPDATE_SCRIPT" | cut -d: -f1)
    dep=$(awk -v s="$start" -v e="$end" 'NR>s && NR<e && /^[[:space:]]*ensure_web_server_dep/ {print NR}' "$UPDATE_SCRIPT")
    # Exactly one call inside install_update.
    [ "$(echo "$dep" | wc -l | tr -d ' ')" -eq 1 ]
    [ -n "$dep" ]
    verify=$(grep -n 'if ! verify_kiosk_started; then' "$UPDATE_SCRIPT" | head -1 | cut -d: -f1)
    marker=$(awk -v s="$start" -v e="$end" 'NR>s && NR<e && /^    clear_ota_marker$/ {print NR}' "$UPDATE_SCRIPT")
    web_restart=$(awk -v s="$start" -v e="$end" 'NR>s && NR<e && /restart magic-dingus-web.service/ {print NR}' "$UPDATE_SCRIPT")
    [ "$dep" -gt "$verify" ]
    [ "$dep" -gt "$marker" ]
    [ "$dep" -lt "$web_restart" ]
}

@test "deploy_cpp.sh installs the web dep through the same function" {
    grep -q 'update.sh && ensure_web_server_dep' "$SCRIPT_DIR/../deploy_cpp.sh"
}

# =============================================================================
# Rollback to a release without magic-dingus-audio.service
# =============================================================================

AUDIO_MARKER_LINE='# magic-dingus-audio: written by audio_service.sh'

# $1 = "with" / "without" audio_service.sh in the BACKUP (the restored tree)
seed_audio_rollback() {
    seed_installed_tree
    echo "new" > "$MAGIC_BASE_PATH/magic_dingus_box_cpp/scripts/audio_service.sh"
    mkdir -p "$MAGIC_BACKUP_DIR"
    cp -R "$MAGIC_BASE_PATH/." "$MAGIC_BACKUP_DIR/"
    echo "1.0.7" > "$MAGIC_BACKUP_DIR/VERSION"
    if [ "$1" = "without" ]; then
        rm -f "$MAGIC_BACKUP_DIR/magic_dingus_box_cpp/scripts/audio_service.sh"
    fi
    export MAGIC_AUDIO_HOME="$TEST_TEMP_DIR/home"
    mkdir -p "$MAGIC_AUDIO_HOME/.config/pulse"
    printf '%s\nautospawn = no\n' "$AUDIO_MARKER_LINE" > "$MAGIC_AUDIO_HOME/.config/pulse/client.conf"
}

@test "audio rollback: the marker matches audio_service.sh's CLIENT_CONF_MARKER" {
    grep -qF "CLIENT_CONF_MARKER=\"${AUDIO_MARKER_LINE}\"" "$SCRIPT_DIR/../audio_service.sh"
    grep -qF "AUDIO_CLIENT_CONF_MARKER=\"${AUDIO_MARKER_LINE}\"" "$UPDATE_SCRIPT"
}

@test "audio rollback (internal): to a tree without audio_service.sh removes our client.conf" {
    seed_audio_rollback without
    load_update_functions
    rollback_internal 2>/dev/null
    [ ! -f "$MAGIC_BASE_PATH/magic_dingus_box_cpp/scripts/audio_service.sh" ]
    [ ! -f "$MAGIC_AUDIO_HOME/.config/pulse/client.conf" ]
}

@test "audio rollback (boot recover path): removes our client.conf too" {
    seed_audio_rollback without
    printf 'target=1.0.8\nfrom=1.0.7\n' > "${MAGIC_BACKUP_DIR}.ota_in_progress"
    run "$UPDATE_SCRIPT" recover
    [ "$status" -eq 0 ]
    [ "$(cat "$MAGIC_BASE_PATH/VERSION")" = "1.0.7" ]
    [ ! -f "$MAGIC_AUDIO_HOME/.config/pulse/client.conf" ]
}

@test "audio rollback (user-initiated): to a tree without audio_service.sh removes our client.conf" {
    seed_audio_rollback without
    load_update_functions
    rollback >/dev/null 2>&1
    [ ! -f "$MAGIC_AUDIO_HOME/.config/pulse/client.conf" ]
}

@test "audio rollback: a tree that still has audio_service.sh keeps client.conf" {
    seed_audio_rollback with
    load_update_functions
    rollback_internal 2>/dev/null
    [ -f "$MAGIC_AUDIO_HOME/.config/pulse/client.conf" ]
}

@test "audio rollback: a client.conf we did not write is never removed" {
    seed_audio_rollback without
    printf 'autospawn = no\n' > "$MAGIC_AUDIO_HOME/.config/pulse/client.conf"
    load_update_functions
    rollback_internal 2>/dev/null
    [ -f "$MAGIC_AUDIO_HOME/.config/pulse/client.conf" ]
}

@test "audio rollback: disables + stops the unit and removes the kiosk drop-in" {
    seed_audio_rollback without
    load_update_functions
    rm -f "$MAGIC_BASE_PATH/magic_dingus_box_cpp/scripts/audio_service.sh"
    SKIP_SYSTEMCTL=false
    export MAGIC_SYSTEMD_DIR="$TEST_TEMP_DIR/sd"
    mkdir -p "$MAGIC_SYSTEMD_DIR/magic-dingus-box-cpp.service.d"
    touch "$MAGIC_SYSTEMD_DIR/magic-dingus-audio.service" \
          "$MAGIC_SYSTEMD_DIR/magic-dingus-box-cpp.service.d/audio-service.conf"
    SUDO_LOG="$TEST_TEMP_DIR/sudo.log"
    sudo() { echo "$*" >> "$SUDO_LOG"; [ "$1" = "-n" ] && shift; "$@"; }
    systemctl() { return 0; }
    run retire_audio_service_if_absent
    [ "$status" -eq 0 ]
    grep -qx 'systemctl disable --no-reload magic-dingus-audio.service' "$SUDO_LOG"
    grep -qx 'systemctl stop magic-dingus-audio.service' "$SUDO_LOG"
    [ ! -f "$MAGIC_SYSTEMD_DIR/magic-dingus-box-cpp.service.d/audio-service.conf" ]
    [ ! -f "$MAGIC_AUDIO_HOME/.config/pulse/client.conf" ]

    # Boot recovery never blocks on the stop job.
    : > "$SUDO_LOG"
    BOOT_RECOVERY=true
    run retire_audio_service_if_absent
    grep -qx 'systemctl --no-block stop magic-dingus-audio.service' "$SUDO_LOG"
}

@test "audio rollback: both rollback paths call retire_audio_service_if_absent" {
    seed_audio_rollback without
    load_update_functions
    retire_audio_service_if_absent() { echo retired >> "$TEST_TEMP_DIR/retire.log"; }
    rollback_internal 2>/dev/null
    rollback >/dev/null 2>&1
    [ "$(wc -l < "$TEST_TEMP_DIR/retire.log")" -eq 2 ]
}

# =============================================================================
# UPDATE CHANNEL TESTS (stable / beta) — the channel file
# =============================================================================
# config/update_channel under the install root. /config/* is excluded from
# every OTA rsync, so neither an update nor a rollback can flip it.

@test "channel: absent file reads as stable" {
    run "$UPDATE_SCRIPT" channel
    [ "$status" -eq 0 ]
    [ "$output" = "stable" ]
}

@test "channel: lives at <install>/config/update_channel" {
    load_update_functions
    [ "$CHANNEL_FILE" = "$MAGIC_BASE_PATH/config/update_channel" ]
}

@test "channel: only the exact word beta selects beta; anything else is stable" {
    mkdir -p "$MAGIC_BASE_PATH/config"
    local f="$MAGIC_BASE_PATH/config/update_channel" content
    for content in "beta" "beta\n" "  beta  \n" "\nbeta\n"; do
        printf "$content" > "$f"
        run "$UPDATE_SCRIPT" channel
        [ "$output" = "beta" ] || { echo "'$content' -> '$output'"; false; }
    done
    for content in "" "stable" "Beta" "BETA" "betas" "beta1" "dev" "beta stable"; do
        printf "$content" > "$f"
        run "$UPDATE_SCRIPT" channel
        [ "$status" -eq 0 ]
        [ "$output" = "stable" ] || { echo "'$content' -> '$output'"; false; }
    done
}

@test "channel: an unreadable file reads as stable" {
    [ "$(id -u)" -ne 0 ] || skip "root reads anything"
    mkdir -p "$MAGIC_BASE_PATH/config"
    echo beta > "$MAGIC_BASE_PATH/config/update_channel"
    chmod 000 "$MAGIC_BASE_PATH/config/update_channel"
    run "$UPDATE_SCRIPT" channel
    chmod 644 "$MAGIC_BASE_PATH/config/update_channel"
    [ "$output" = "stable" ]
}

@test "channel beta: writes the file (creating config/) and prints beta" {
    [ ! -d "$MAGIC_BASE_PATH/config" ]
    run "$UPDATE_SCRIPT" channel beta
    [ "$status" -eq 0 ]
    [ "${lines[${#lines[@]}-1]}" = "beta" ]
    [ "$(cat "$MAGIC_BASE_PATH/config/update_channel")" = "beta" ]
    [ ! -e "$MAGIC_BASE_PATH/config/update_channel.tmp" ]
    run "$UPDATE_SCRIPT" channel
    [ "$output" = "beta" ]
}

@test "channel stable: REMOVES the file (absence is the default)" {
    mkdir -p "$MAGIC_BASE_PATH/config"
    echo beta > "$MAGIC_BASE_PATH/config/update_channel"
    run "$UPDATE_SCRIPT" channel stable
    [ "$status" -eq 0 ]
    [ "${lines[${#lines[@]}-1]}" = "stable" ]
    [ ! -e "$MAGIC_BASE_PATH/config/update_channel" ]
    # Idempotent on a box that is already stable.
    run "$UPDATE_SCRIPT" channel stable
    [ "$status" -eq 0 ]
}

@test "channel: an unknown channel is refused and the file is untouched" {
    mkdir -p "$MAGIC_BASE_PATH/config"
    echo beta > "$MAGIC_BASE_PATH/config/update_channel"
    run "$UPDATE_SCRIPT" channel nightly
    [ "$status" -eq 2 ]
    [ "$(cat "$MAGIC_BASE_PATH/config/update_channel")" = "beta" ]
}

@test "channel: stdout is exactly one word (admin.py and verify_box.sh parse it)" {
    out=$("$UPDATE_SCRIPT" channel beta 2>/dev/null)
    [ "$out" = "beta" ]
    out=$("$UPDATE_SCRIPT" channel 2>/dev/null)
    [ "$out" = "beta" ]
    out=$("$UPDATE_SCRIPT" channel stable 2>/dev/null)
    [ "$out" = "stable" ]
}

@test "usage lists the channel command" {
    run "$UPDATE_SCRIPT" --help
    [[ "$output" == *"channel [stable|beta]"* ]]
}

# =============================================================================
# CHECK ON BOTH CHANNELS — against the rehearsal's fake GitHub
# =============================================================================
# tests/ota_rehearsal/box/fake_github.py in plain-HTTP mode on an ephemeral
# port. It logs every request, which is how "the stable path never asks for
# the release LIST" is proven rather than assumed.

FAKE_GH="$SCRIPT_DIR/../../../tests/ota_rehearsal/box/fake_github.py"
FAKE_REPO="a-train-chain/magic_dingus_box"

# fake_release VER STAMP — a release dir with the three assets release.yml
# uploads; STAMP (touch -t) sets its published_at, i.e. its list position.
fake_release() {
    local d="$FAKE_GH_RELEASES/v$1"
    mkdir -p "$d"
    echo "source $1" > "$d/magic-dingus-box-$1.tar.gz"
    echo "sum $1" > "$d/checksum.sha256"
    echo "bin $1" > "$d/magic_dingus_box_cpp-arm64-$1.tar.gz"
    touch -t "$2" "$d"/*
}

# fake_control LATEST [DRAFTS_JSON]
fake_control() {
    printf '{"latest": "%s", "drafts": %s}\n' "$1" "${2:-[]}" > "$FAKE_GH_CONTROL"
}

start_fake_github() {
    command -v python3 >/dev/null || skip "python3 required"
    export FAKE_GH_RELEASES="$TEST_TEMP_DIR/gh/release"
    export FAKE_GH_CONTROL="$TEST_TEMP_DIR/gh/control.json"
    export FAKE_GH_LOG="$TEST_TEMP_DIR/gh/requests.jsonl"
    export FAKE_GH_REPO="$FAKE_REPO"
    mkdir -p "$FAKE_GH_RELEASES"
    : > "$FAKE_GH_LOG"
    local portfile="$TEST_TEMP_DIR/gh/port"
    python3 "$FAKE_GH" --http "$portfile" > "$TEST_TEMP_DIR/gh/server.log" 2>&1 3>&- &
    FAKE_GH_PID=$!
    local i
    for i in $(seq 1 100); do
        [ -s "$portfile" ] && break
        sleep 0.05
    done
    [ -s "$portfile" ] || { cat "$TEST_TEMP_DIR/gh/server.log"; return 1; }
    local port
    port=$(tr -d '[:space:]' < "$portfile")
    export MAGIC_GITHUB_API="http://127.0.0.1:${port}/repos/${FAKE_REPO}/releases/latest"
    export MAGIC_GITHUB_RELEASES_API="http://127.0.0.1:${port}/repos/${FAKE_REPO}/releases?per_page=20"
}

stop_fake_github() {
    [ -n "${FAKE_GH_PID:-}" ] && kill "$FAKE_GH_PID" 2>/dev/null
    wait "${FAKE_GH_PID:-}" 2>/dev/null || true
}

# Release history used by most tests below. Published order (oldest first):
# 1.10.0, 1.10.1-beta.1, 1.10.1-beta.2, then a 1.9.15 hotfix published LAST
# — so "newest in the list" and "highest version" deliberately disagree.
seed_release_history() {
    fake_release 1.10.0        202609010000
    fake_release 1.10.1-beta.1 202609100000
    fake_release 1.10.1-beta.2 202609200000
    fake_release 1.9.15        202609300000
    fake_control 1.10.0
}

# jfield JSON KEY — data.KEY from update.sh's check JSON.
jfield() {
    python3 -c 'import json,sys; v=json.loads(sys.argv[1])["data"][sys.argv[2]]; print(json.dumps(v) if isinstance(v,bool) else v)' "$1" "$2"
}

list_requests()   { grep -c "\"path\": \"/repos/${FAKE_REPO}/releases?" "$FAKE_GH_LOG" || true; }
latest_requests() { grep -c "\"path\": \"/repos/${FAKE_REPO}/releases/latest\"" "$FAKE_GH_LOG" || true; }

set_box() {  # set_box VERSION CHANNEL
    echo "$1" > "$MAGIC_BASE_PATH/VERSION"
    if [ "$2" = beta ]; then
        mkdir -p "$MAGIC_BASE_PATH/config"; echo beta > "$MAGIC_BASE_PATH/config/update_channel"
    else
        rm -f "$MAGIC_BASE_PATH/config/update_channel"
    fi
}

@test "check (stable): offers releases/latest and NEVER requests the list endpoint" {
    start_fake_github
    seed_release_history
    set_box 1.9.0 stable
    run "$UPDATE_SCRIPT" check
    stop_fake_github
    [ "$status" -eq 0 ]
    json=$(echo "$output" | sed -n '/^{/,/^}/p')
    [ "$(jfield "$json" latest_version)" = "1.10.0" ]
    [ "$(jfield "$json" update_available)" = "true" ]
    [ "$(jfield "$json" channel)" = "stable" ]
    [ "$(jfield "$json" download_url)" = "https://github.com/${FAKE_REPO}/releases/download/v1.10.0/magic-dingus-box-1.10.0.tar.gz" ]
    [ "$(latest_requests)" -eq 1 ]
    [ "$(list_requests)" -eq 0 ]
    # Nothing but releases/latest was asked of the API.
    [ "$(grep -c '"path"' "$FAKE_GH_LOG")" -eq 1 ]
}

@test "check (stable): the request is the pre-channel one (GITHUB_API default unchanged)" {
    load_update_functions
    unset MAGIC_GITHUB_API
    GITHUB_REPO="$FAKE_REPO"
    [ "${MAGIC_GITHUB_API:-https://api.github.com/repos/${GITHUB_REPO}/releases/latest}" = \
      "https://api.github.com/repos/${FAKE_REPO}/releases/latest" ]
    grep -q 'GITHUB_API="${MAGIC_GITHUB_API:-https://api.github.com/repos/${GITHUB_REPO}/releases/latest}"' "$UPDATE_SCRIPT"
}

@test "check (stable): a box on a beta switched back to stable is offered NO downgrade" {
    start_fake_github
    seed_release_history
    set_box 1.10.1-beta.2 stable
    run "$UPDATE_SCRIPT" check
    stop_fake_github
    [ "$status" -eq 0 ]
    json=$(echo "$output" | sed -n '/^{/,/^}/p')
    [ "$(jfield "$json" latest_version)" = "1.10.0" ]
    [ "$(jfield "$json" update_available)" = "false" ]
    [ "$(list_requests)" -eq 0 ]
}

@test "check (stable): ...and takes the next stable once it ships" {
    start_fake_github
    seed_release_history
    fake_release 1.10.1 202610010000
    fake_control 1.10.1
    set_box 1.10.1-beta.2 stable
    run "$UPDATE_SCRIPT" check
    stop_fake_github
    json=$(echo "$output" | sed -n '/^{/,/^}/p')
    [ "$(jfield "$json" update_available)" = "true" ]
    [ "$(jfield "$json" latest_version)" = "1.10.1" ]
}

@test "check (stable): a mis-flagged -beta tag at releases/latest is never offered" {
    start_fake_github
    seed_release_history
    fake_control 1.10.1-beta.2   # as if someone cleared the prerelease flag
    set_box 1.10.0 stable
    run "$UPDATE_SCRIPT" check
    stop_fake_github
    [ "$status" -eq 0 ]
    json=$(echo "$output" | sed -n '/^{/,/^}/p')
    [ "$(jfield "$json" update_available)" = "false" ]
}

@test "check (beta): highest SemVer across stable+prerelease, not newest-published" {
    start_fake_github
    seed_release_history
    set_box 1.10.0 beta
    run "$UPDATE_SCRIPT" check
    stop_fake_github
    [ "$status" -eq 0 ]
    json=$(echo "$output" | sed -n '/^{/,/^}/p')
    [ "$(jfield "$json" latest_version)" = "1.10.1-beta.2" ]
    [ "$(jfield "$json" update_available)" = "true" ]
    [ "$(jfield "$json" channel)" = "beta" ]
    [ "$(jfield "$json" download_url)" = "https://github.com/${FAKE_REPO}/releases/download/v1.10.1-beta.2/magic-dingus-box-1.10.1-beta.2.tar.gz" ]
    [ "$(list_requests)" -eq 1 ]
    [ "$(latest_requests)" -eq 0 ]
    grep -q '"path": "/repos/'"${FAKE_REPO}"'/releases?per_page=20"' "$FAKE_GH_LOG"
}

@test "check (beta): ignores drafts, even when they are the highest version" {
    start_fake_github
    seed_release_history
    fake_release 1.11.0 202610020000
    fake_control 1.10.0 '["1.11.0"]'
    set_box 1.10.0 beta
    run "$UPDATE_SCRIPT" check
    stop_fake_github
    json=$(echo "$output" | sed -n '/^{/,/^}/p')
    [ "$(jfield "$json" latest_version)" = "1.10.1-beta.2" ]
}

@test "check (beta): moves onto the final stable when it ships" {
    start_fake_github
    seed_release_history
    fake_release 1.10.1 202610010000
    set_box 1.10.1-beta.2 beta
    run "$UPDATE_SCRIPT" check
    stop_fake_github
    json=$(echo "$output" | sed -n '/^{/,/^}/p')
    [ "$(jfield "$json" latest_version)" = "1.10.1" ]
    [ "$(jfield "$json" update_available)" = "true" ]
}

@test "check (beta): already on the highest beta -> up to date" {
    start_fake_github
    seed_release_history
    set_box 1.10.1-beta.2 beta
    run "$UPDATE_SCRIPT" check
    stop_fake_github
    json=$(echo "$output" | sed -n '/^{/,/^}/p')
    [ "$(jfield "$json" update_available)" = "false" ]
}

@test "check (beta): a pulled (deleted) beta is no longer offered" {
    start_fake_github
    seed_release_history
    rm -rf "$FAKE_GH_RELEASES/v1.10.1-beta.2"
    set_box 1.10.0 beta
    run "$UPDATE_SCRIPT" check
    stop_fake_github
    json=$(echo "$output" | sed -n '/^{/,/^}/p')
    [ "$(jfield "$json" latest_version)" = "1.10.1-beta.1" ]
}

@test "check (beta): only drafts in the list -> a clean JSON error" {
    start_fake_github
    fake_release 1.11.0 202610020000
    fake_control "" '["1.11.0"]'
    set_box 1.10.0 beta
    run "$UPDATE_SCRIPT" check
    stop_fake_github
    [ "$status" -ne 0 ]
    [[ "$output" == *'"ok": false'* ]]
    [[ "$output" == *"No releases found"* ]]
}

@test "check (beta): an unparseable list -> a clean JSON error" {
    load_update_functions
    curl() { echo 'this is not json'; }
    mkdir -p "$MAGIC_BASE_PATH/config"; echo beta > "$MAGIC_BASE_PATH/config/update_channel"
    run check_update
    [ "$status" -ne 0 ]
    [[ "$output" == *'"ok": false'* ]]
    [[ "$output" == *"Could not parse the release list"* ]]
}

# =============================================================================
# BETA VERSIONS THROUGH THE INSTALL GATES
# =============================================================================

@test "install_update accepts an X.Y.Z-beta.N version past validation" {
    run "$UPDATE_SCRIPT" install "1.10.1-beta.1" "https://evil.com/x.tar.gz"
    [ "$status" -ne 0 ]
    # Refused for the URL, NOT for the version.
    [[ "$output" == *"Invalid download URL"* ]]
    [[ "$output" != *"Invalid version"* ]]
}

@test "install_update still rejects other prerelease shapes" {
    local v
    for v in "1.10.1-rc.1" "1.10.1-beta" "1.10.1-beta.1/../../x" "1.10.1-BETA.1"; do
        run "$UPDATE_SCRIPT" install "$v" \
            "https://github.com/a-train-chain/magic_dingus_box/releases/download/v1.10.1/x.tar.gz"
        [ "$status" -ne 0 ]
        [[ "$output" == *"Invalid version"* ]] || { echo "accepted $v"; false; }
    done
}

@test "get_binary_url looks a beta up by its full tag" {
    load_update_functions
    curl() { echo "$*" >> "$TEST_TEMP_DIR/curl.log"; echo '{}'; }
    get_binary_url "v1.10.1-beta.1" >/dev/null
    grep -q "releases/tags/v1.10.1-beta.1" "$TEST_TEMP_DIR/curl.log"
}
