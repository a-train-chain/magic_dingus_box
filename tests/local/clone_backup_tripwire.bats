#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

setup() {
    T="$(mktemp -d "${BATS_TMPDIR:-/tmp}/mdb-tripwire.XXXXXX")"
    mkdir -p "$T/home/magic" "$T/root"
    # Run the actual preflight block in a disposable filesystem namespace.
    # No root-only preparation, service stop, or cleanup is executed.
    {
        printf 'set -euo pipefail\nlog() { printf "%%s\\n" "$*"; }\n'
        awk '
            /# Preflight: secret tripwire/ { section=1 }
            section && /^shopt -s nullglob/ { emit=1 }
            emit { print }
            emit && /^log "Preflight: secret tripwire clean"/ { exit }
        ' "$TESTS_REPO_ROOT/scripts/golden_image/prepare_for_cloning.sh" \
            | sed "s|/home/magic|$T/home/magic|g; s|/root/|$T/root/|g"
    } > "$T/preflight.sh"
}

teardown() { rm -rf "$T"; }

@test "clone refuses the Pi4 rollback directory and preserves its archive" {
    backup="$T/home/magic/rollback-pre-pi4-current"
    mkdir -p "$backup" "$T/payload/services"
    printf 'WIREGUARD_PRIVATE_KEY=test-only-secret\n' > "$T/payload/services/.env"
    tar -czf "$backup/code-and-config.tgz" -C "$T/payload" services
    cp "$backup/code-and-config.tgz" "$T/expected.tgz"

    run bash "$T/preflight.sh"
    [ "$status" -eq 1 ]
    [[ "$output" == *"$backup"* ]]
    [[ "$output" == *"move them off the SD card"* ]]
    cmp "$backup/code-and-config.tgz" "$T/expected.tgz"
}

@test "clone refuses standalone rollback archives for either operator account" {
    for account in home/magic root; do
        backup="$T/$account/rollback-pre-release.tgz"
        printf 'archive fixture\n' > "$backup"
        run bash "$T/preflight.sh"
        [ "$status" -eq 1 ]
        [[ "$output" == *"$backup"* ]]
        [ -f "$backup" ]
        rm "$backup"
    done
}

@test "clone permits clean accounts and unrelated archives" {
    printf 'ordinary asset\n' > "$T/home/magic/artwork.tgz"
    run bash "$T/preflight.sh"
    [ "$status" -eq 0 ]
    [[ "$output" == *"secret tripwire clean"* ]]
    [ -f "$T/home/magic/artwork.tgz" ]
}
