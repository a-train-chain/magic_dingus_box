#!/usr/bin/env bats
#
# BATS tests for converge_custom_formats.sh — the standalone converge script
# that reconciles the codified Radarr/Sonarr Custom Formats and both "Any"
# profile score maps.
#
# Run with: bats test_converge_custom_formats.bats
#
# What these tests can and cannot cover: the reconcilers themselves talk to
# live Radarr/Sonarr APIs, so their idempotence is proven on hardware (see
# .superpowers/sdd/cf-converge-report.md). What must hold on ANY machine,
# including a CI runner with no Media Browser, is the SKIP contract — an OTA
# must never fail because a box has no Media Browser, and it must never make
# a network call on such a box. Those are the tests here.
#
# The script is pointed at a fake services dir via MAGIC_SERVICES_DIR so no
# test can ever read a real /opt/magic_dingus_box/services.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")" && pwd)"
CONVERGE_SCRIPT="$SCRIPT_DIR/../converge_custom_formats.sh"
UPDATE_SCRIPT="$SCRIPT_DIR/../update.sh"

setup() {
    TEST_TEMP_DIR="$(mktemp -d)"
    export MAGIC_SERVICES_DIR="$TEST_TEMP_DIR/services"
    mkdir -p "$MAGIC_SERVICES_DIR"
    # The suite is invoked with MAGIC_SKIP_SYSTEMCTL=true; most tests here
    # need the real (non-test-mode) path, so clear it per test and let the
    # test-mode test set it back explicitly.
    unset MAGIC_SKIP_SYSTEMCTL
    # Keep any accidental probe short if a test ever reaches one.
    export MAGIC_CF_PROBE_ATTEMPTS=1
}

teardown() {
    [ -n "$TEST_TEMP_DIR" ] && [ -d "$TEST_TEMP_DIR" ] && rm -rf "$TEST_TEMP_DIR"
}

@test "script exists and is syntactically valid bash" {
    [ -f "$CONVERGE_SCRIPT" ]
    bash -n "$CONVERGE_SCRIPT"
}

@test "unprovisioned box (no services/.env) exits 0 and says why" {
    # THE contract this hook lives or dies on: an OTA must never fail
    # because the box has no Media Browser.
    run bash "$CONVERGE_SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"no Media Browser"* ]]
}

@test "unprovisioned box makes no network call at all" {
    # Prove the skip happens BEFORE any probe: stub curl to fail the test
    # loudly if it is ever reached.
    mkdir -p "$TEST_TEMP_DIR/bin"
    cat > "$TEST_TEMP_DIR/bin/curl" <<'EOF'
#!/bin/bash
echo "FORBIDDEN: converge script called curl on an unprovisioned box" >&2
exit 99
EOF
    chmod +x "$TEST_TEMP_DIR/bin/curl"
    PATH="$TEST_TEMP_DIR/bin:$PATH" run bash "$CONVERGE_SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" != *"FORBIDDEN"* ]]
}

@test "missing services dir entirely (not just .env) exits 0" {
    export MAGIC_SERVICES_DIR="$TEST_TEMP_DIR/nope"
    run bash "$CONVERGE_SCRIPT"
    [ "$status" -eq 0 ]
}

@test "MAGIC_SKIP_SYSTEMCTL=true skips the whole converge" {
    export MAGIC_SKIP_SYSTEMCTL=true
    # Even with a plausible-looking .env present, test mode wins.
    printf 'RADARR_API_KEY=deadbeef\nSONARR_API_KEY=deadbeef\n' \
        > "$MAGIC_SERVICES_DIR/.env"
    run bash "$CONVERGE_SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"SKIP"* ]]
    [[ "$output" == *"test mode"* ]]
}

@test "provisioned .env but no API keys skips each service, exit 0" {
    # A box whose stack was never brought up has an .env (the Content
    # Manager wrote the WireGuard config) but no Radarr/Sonarr config.xml
    # and no API keys. Both halves must skip without probing.
    printf 'WIREGUARD_PRIVATE_KEY=abc\n' > "$MAGIC_SERVICES_DIR/.env"
    mkdir -p "$TEST_TEMP_DIR/bin"
    cat > "$TEST_TEMP_DIR/bin/curl" <<'EOF'
#!/bin/bash
echo "FORBIDDEN: probed with no API key" >&2
exit 99
EOF
    chmod +x "$TEST_TEMP_DIR/bin/curl"
    PATH="$TEST_TEMP_DIR/bin:$PATH" run bash "$CONVERGE_SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" != *"FORBIDDEN"* ]]
    [[ "$output" == *"no Radarr API key"* ]]
    [[ "$output" == *"no Sonarr API key"* ]]
}

@test "unreachable service is a WARN and still exit 0" {
    # Keys present, nothing listening. The probe must fail closed and the
    # script must still exit 0 so the OTA continues.
    printf 'RADARR_API_KEY=deadbeef\nSONARR_API_KEY=deadbeef\n' \
        > "$MAGIC_SERVICES_DIR/.env"
    mkdir -p "$TEST_TEMP_DIR/bin"
    cat > "$TEST_TEMP_DIR/bin/curl" <<'EOF'
#!/bin/bash
exit 7
EOF
    chmod +x "$TEST_TEMP_DIR/bin/curl"
    PATH="$TEST_TEMP_DIR/bin:$PATH" run bash "$CONVERGE_SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"Radarr not reachable"* ]]
    [[ "$output" == *"Sonarr not reachable"* ]]
}

# =============================================================================
# DELIVERY WIRING — the gap this script exists to close
# =============================================================================

@test "update.sh invokes converge_custom_formats.sh on every OTA" {
    # The whole point: fielded boxes only ever update. If this hook is ever
    # dropped, a Custom Format retune silently reaches zero customers again.
    grep -q "converge_custom_formats.sh" "$UPDATE_SCRIPT"
    run grep -E 'sudo -n bash "\$\{INSTALL_DIR\}/magic_dingus_box_cpp/scripts/converge_custom_formats.sh"' "$UPDATE_SCRIPT"
    [ "$status" -eq 0 ]
}

@test "update.sh guards the converge hook like the memory-tuning hook" {
    # Same shape as its sibling: skipped in test mode, existence-checked so
    # an older release cannot break the OTA, and failure is a warning that
    # never aborts the update.
    run grep -E 'log_warn "Custom Format convergence failed' "$UPDATE_SCRIPT"
    [ "$status" -eq 0 ]
    run grep -E 'converge_custom_formats.sh not found in this release' "$UPDATE_SCRIPT"
    [ "$status" -eq 0 ]
    run grep -E 'SKIP: Custom Format convergence \(test mode\)' "$UPDATE_SCRIPT"
    [ "$status" -eq 0 ]
}

@test "setup_services.sh calls the converge script instead of carrying a copy" {
    # Extraction, not duplication — the two can never drift if there is only
    # one copy of the reconcilers in the repo.
    local setup_sh="$SCRIPT_DIR/../setup_services.sh"
    run grep -E 'bash "\$\{SCRIPT_DIR\}/converge_custom_formats.sh"' "$setup_sh"
    [ "$status" -eq 0 ]
    # No SCORE_MAP may remain behind in setup_services.sh.
    run grep -c "^SCORE_MAP = {" "$setup_sh"
    [ "$output" -eq 0 ]
}
