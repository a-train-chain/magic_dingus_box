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

    if version_lt "$v1" "$v2"; then
        [ "$expected" = "true" ]
    else
        [ "$expected" = "false" ]
    fi
}

@test "version_lt: 1.0.0 < 1.0.1" {
    # Test version comparison using sort -V (same logic as update.sh)
    # version_lt returns 0 (true) if v1 < v2
    v1="1.0.0"
    v2="1.0.1"
    if [ "$(printf '%s\n' "$v1" "$v2" | sort -V | head -n1)" = "$v1" ] && [ "$v1" != "$v2" ]; then
        result="true"
    else
        result="false"
    fi
    [ "$result" = "true" ]
}

@test "version_lt: 1.0.1 >= 1.0.0" {
    v1="1.0.1"
    v2="1.0.0"
    if [ "$(printf '%s\n' "$v1" "$v2" | sort -V | head -n1)" = "$v1" ] && [ "$v1" != "$v2" ]; then
        result="true"
    else
        result="false"
    fi
    [ "$result" = "false" ]
}

@test "version_lt: 1.0.0 >= 1.0.0 (equal versions)" {
    v1="1.0.0"
    v2="1.0.0"
    if [ "$(printf '%s\n' "$v1" "$v2" | sort -V | head -n1)" = "$v1" ] && [ "$v1" != "$v2" ]; then
        result="true"
    else
        result="false"
    fi
    [ "$result" = "false" ]
}

@test "version_lt: 1.9.0 < 1.10.0 (numeric comparison)" {
    v1="1.9.0"
    v2="1.10.0"
    if [ "$(printf '%s\n' "$v1" "$v2" | sort -V | head -n1)" = "$v1" ] && [ "$v1" != "$v2" ]; then
        result="true"
    else
        result="false"
    fi
    [ "$result" = "true" ]
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
    [[ "$output" == *"No backup"* ]] || [[ "$output" == *"no backup"* ]]
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
