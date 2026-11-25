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
            | sed "s|/home/magic|$T/home/magic|g; s|/root|$T/root|g"
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

@test "clone refuses OTA backups and bench credentials in either operator home" {
    for account in home/magic root; do
        home_dir="$T/$account"
        mkdir -p "$home_dir/.magic_dingus_box_backup" "$home_dir/mdb-bench"
        touch "$home_dir/.magic_dingus_box_backup.ota_in_progress"
        printf 'private fixture value\n' > "$home_dir/mdb-bench/services.env.backup"
        run bash "$T/preflight.sh"
        [ "$status" -eq 1 ]
        for name in .magic_dingus_box_backup .magic_dingus_box_backup.ota_in_progress mdb-bench/services.env.backup; do
            [[ "$output" == *"$home_dir/$name"$'\n'* ]] || return 1
            [ -e "$home_dir/$name" ]
        done
        [[ "$output" != *'private fixture value'* ]] || return 1
    done
}

@test "clone reports validation artifacts in either home until they are moved off-card" {
    dirs=(bluetooth-input-source-bd0ee66 bluetooth-input-validation-20261113 hw_validate_20261004_041641)
    files=(hw_validate_20261004_041641.json predeploy-index-20261115 pad_walk.py ui_launch_test.py
           kit2_before.md5 ota_1.11.0.log ota_beta3.log ota_beta4.log)
    for account in home/magic root; do
        home_dir="$T/$account"
        for name in "${dirs[@]}"; do mkdir "$home_dir/$name"; done
        for name in "${files[@]}"; do touch "$home_dir/$name"; done
        for _ in 1 2; do
            run bash "$T/preflight.sh"
            [ "$status" -eq 1 ]
            for name in "${dirs[@]}" "${files[@]}"; do
                [[ "$output" == *"$home_dir/$name"$'\n'* ]] || return 1
                [ -e "$home_dir/$name" ]
            done
        done
        mkdir -p "$T/off-card/$account"
        for name in "${dirs[@]}" "${files[@]}"; do mv "$home_dir/$name" "$T/off-card/$account/"; done
    done
    run bash "$T/preflight.sh"
    [ "$status" -eq 0 ]
}

@test "clone permits clean accounts and unrelated files on repeat runs" {
    files=(artwork.tgz ota_notes.log hw_validate_notes.txt mdb-bench/notes.txt project/ui_launch_test.py)
    for account in home/magic root; do
        home_dir="$T/$account"
        mkdir -p "$home_dir/mdb-bench" "$home_dir/project"
        for name in "${files[@]}"; do
            printf 'ordinary asset\n' > "$home_dir/$name"
        done
    done
    for _ in 1 2; do
        run bash "$T/preflight.sh"
        [ "$status" -eq 0 ]
        [[ "$output" == *"secret tripwire clean"* ]]
    done
    for account in home/magic root; do
        for name in "${files[@]}"; do
            [ "$(cat "$T/$account/$name")" = 'ordinary asset' ]
        done
    done
}
