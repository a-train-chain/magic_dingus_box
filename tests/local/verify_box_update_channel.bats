#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# verify_box.sh WARNs (never FAILs) on a box whose OTA update channel is
# beta, so the owner never ships a beta-channel unit by accident. Its reader
# must apply EXACTLY update.sh's rule — if the two disagreed, Box Health
# could call a box stable while update.sh hands it betas. verify_box.sh runs
# top-to-bottom against live hardware, so the function is extracted.

setup() {
    local def
    def=$(sed -n '/^update_channel_of() {/,/^}/p' "$CPP_DIR/scripts/verify_box.sh")
    [ -n "$def" ] || { echo "update_channel_of not found in verify_box.sh"; return 1; }
    eval "$def"
    F="$BATS_TEST_TMPDIR/update_channel"
}

update_sh_reads() {
    MAGIC_CHANNEL_FILE="$1" "$CPP_DIR/scripts/update.sh" channel 2>/dev/null
}

@test "absent file -> stable" {
    [ "$(update_channel_of "$BATS_TEST_TMPDIR/nope")" = "stable" ]
}

@test "verify_box and update.sh agree on every input" {
    local content a b bad=0
    for content in "beta" "beta\n" "  beta \n" "\nbeta" "" "stable" "Beta" "betas" "beta1" "nightly" "be ta"; do
        printf "$content" > "$F"
        a=$(update_channel_of "$F")
        b=$(update_sh_reads "$F")
        if [ "$a" != "$b" ]; then echo "'$content': verify_box=$a update.sh=$b"; bad=1; fi
    done
    [ "$bad" -eq 0 ]
}

@test "beta is a WARN with the fix command, never a FAIL" {
    grep -q 'warn "update channel is BETA' "$CPP_DIR/scripts/verify_box.sh"
    ! grep -q 'fail "update channel' "$CPP_DIR/scripts/verify_box.sh"
    grep -q 'update.sh channel stable' "$CPP_DIR/scripts/verify_box.sh"
}

@test "verify_box reads the same path update.sh writes" {
    grep -q 'update_channel_of "${BASE}/config/update_channel"' "$CPP_DIR/scripts/verify_box.sh"
    grep -qF 'CHANNEL_FILE="${MAGIC_CHANNEL_FILE:-${INSTALL_DIR}/config/update_channel}"' \
        "$CPP_DIR/scripts/update.sh"
}

@test "clone path: prepare refuses a beta box and first boot deletes the flag" {
    grep -q 'update_channel' "$TESTS_REPO_ROOT/scripts/golden_image/prepare_for_cloning.sh"
    grep -q 'rm -f "\${INSTALL_DIR}/config/update_channel"' \
        "$TESTS_REPO_ROOT/scripts/golden_image/first_boot.sh"
}
