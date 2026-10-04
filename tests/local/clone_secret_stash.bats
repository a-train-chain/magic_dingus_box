#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# The live-clone scrub removes every secret from the SD card for the length of
# the dd, keeping the only copy in a stash. That stash used to be /dev/shm: a
# reboot mid-clone erased it, restore found no manifest, said "nothing to
# restore" and exited 0 — and the source box had permanently lost its *arr
# libraries, VPN key, API keys and phone pairings.
#
# The stash now lives on the movie drive, and restore fails LOUDLY when a
# stash it was promised is missing. These tests drive the shared library
# (scripts/golden_image/clone_stash_lib.sh) against a scratch directory —
# the "reboot" is simply that nothing in the scratch tree is RAM.

GI_DIR="$TESTS_REPO_ROOT/scripts/golden_image"
LIB="$GI_DIR/clone_stash_lib.sh"

setup() {
    # shellcheck disable=SC1090
    source "$LIB"
    T="$(mktemp -d "${BATS_TMPDIR:-/tmp}/mdb-stash.XXXXXX")"
    BACKUP="$T/cloning_backup"
    mkdir -p "$BACKUP"
    # Point every location the library knows about into the scratch tree.
    # The "mounted drive" stash sits outside MDB_STASH_DRIVE so no mount check
    # applies to it; MDB_STASH_DRIVE itself is a plain directory, i.e. an
    # UNMOUNTED drive, for the unreachable case.
    MDB_STASH_DRIVE="$T/ssd-unmounted"
    MDB_SECRET_STASH_DIR="$T/drive/.mdb-secret-stash"
    MDB_RAM_SECRET_STASH_DIR="$T/shm/mdb-secret-stash"
    mkdir -p "$T/root/opt/services/config/radarr" "$T/root/boot"
    printf 'WIREGUARD_PRIVATE_KEY=abc123\n' > "$T/root/opt/services/.env"
    printf 'radarr-db-bytes\0with-nul\n' > "$T/root/opt/services/config/radarr/radarr.db"
    printf 'psk=hunter2\n' > "$T/root/boot/network-config"
    FILES=("$T/root/opt/services/.env"
           "$T/root/opt/services/config/radarr/radarr.db"
           "$T/root/boot/network-config")
    for f in "${FILES[@]}"; do cp "$f" "$f.orig"; done
}

teardown() {
    rm -rf "$T"
}

# What prepare_for_cloning.sh does, minus the root-only parts: stash every
# file, prove the copies, shred the originals, record the stash location.
fake_prepare() {
    local stash="${1:-$MDB_SECRET_STASH_DIR}"
    mdb_init_secret_stash "$stash"
    printf '%s\n' "$stash" > "$BACKUP/$MDB_STASH_POINTER_NAME"
    for f in "${FILES[@]}"; do mdb_stash_copy "$f" "$stash"; done
    for f in "${FILES[@]}"; do mdb_stash_matches "$f" "$stash"; done
    for f in "${FILES[@]}"; do mdb_shred_file "$f"; done
}

assert_all_restored() {
    for f in "${FILES[@]}"; do
        cmp -s "$f" "$f.orig" || { echo "not restored: $f"; return 1; }
    done
}

# ---------------------------------------------------------------------------
# Location
# ---------------------------------------------------------------------------

@test "stash location: persistent drive, never /dev/shm" {
    run bash -c "source '$LIB'; echo \"\$MDB_SECRET_STASH_DIR\""
    [ "$status" -eq 0 ]
    [ "$output" = "/mnt/ssd/.mdb-secret-stash" ]
    [[ "$output" != /dev/shm* ]]
}

@test "prepare no longer hardcodes a RAM stash" {
    run grep -nE '^(SECRET_STASH|BOOT_STASH)="/dev/shm' "$GI_DIR/prepare_for_cloning.sh"
    [ "$status" -ne 0 ]
    run grep -n 'SECRET_STASH="$MDB_SECRET_STASH_DIR"' "$GI_DIR/prepare_for_cloning.sh"
    [ "$status" -eq 0 ]
}

@test "RAM stash is opt-in only (--allow-ram-stash)" {
    run grep -n 'SECRET_STASH="$MDB_RAM_SECRET_STASH_DIR"' "$GI_DIR/prepare_for_cloning.sh"
    [ "$status" -eq 0 ]
    run grep -B1 'SECRET_STASH="$MDB_RAM_SECRET_STASH_DIR"' "$GI_DIR/prepare_for_cloning.sh"
    [[ "$output" == *'ALLOW_RAM_STASH'* ]]
}

@test "stash location is chosen before the clone-in-progress marker" {
    local verdict_line marker_line
    verdict_line=$(grep -n 'STASH_VERDICT="$(mdb_check_stash_drive)"' "$GI_DIR/prepare_for_cloning.sh" | cut -d: -f1)
    marker_line=$(grep -n 'cat > "$MARKER_PATH"' "$GI_DIR/prepare_for_cloning.sh" | cut -d: -f1)
    [ -n "$verdict_line" ] && [ -n "$marker_line" ]
    [ "$verdict_line" -lt "$marker_line" ]
}

@test "drive verdict: only a mounted drive on a different disk is ok" {
    [ "$(mdb_stash_drive_verdict 1 sda mmcblk0)" = "ok" ]
    [ "$(mdb_stash_drive_verdict 0 sda mmcblk0)" = "not-mounted" ]
    [ "$(mdb_stash_drive_verdict 0 '' mmcblk0)" = "not-mounted" ]
    [ "$(mdb_stash_drive_verdict 1 mmcblk0 mmcblk0)" = "same-disk-as-root" ]
    [ "$(mdb_stash_drive_verdict 1 '' mmcblk0)" = "disk-unknown" ]
    [ "$(mdb_stash_drive_verdict 1 sda '')" = "disk-unknown" ]
}

# ---------------------------------------------------------------------------
# Stash creation
# ---------------------------------------------------------------------------

@test "init creates a 0700 dir with an empty manifest" {
    mdb_init_secret_stash "$MDB_SECRET_STASH_DIR"
    [ -f "$MDB_SECRET_STASH_DIR/manifest" ]
    [ ! -s "$MDB_SECRET_STASH_DIR/manifest" ]
    perms=$(stat -c %a "$MDB_SECRET_STASH_DIR" 2>/dev/null || stat -f %Lp "$MDB_SECRET_STASH_DIR")
    [ "$perms" = "700" ]
}

@test "init refuses to reuse an unrestored stash" {
    fake_prepare
    run mdb_init_secret_stash "$MDB_SECRET_STASH_DIR"
    [ "$status" -ne 0 ]
    [[ "$output" == *"already exists"* ]]
    # ...and left it intact
    [ -s "$MDB_SECRET_STASH_DIR/manifest" ]
}

@test "init clears debris, including a stale restored flag" {
    mkdir -p "$MDB_SECRET_STASH_DIR"
    : > "$MDB_SECRET_STASH_DIR/$MDB_STASH_RESTORED_NAME"
    echo junk > "$MDB_SECRET_STASH_DIR/_old_file"
    mdb_init_secret_stash "$MDB_SECRET_STASH_DIR"
    [ ! -e "$MDB_SECRET_STASH_DIR/$MDB_STASH_RESTORED_NAME" ]
    [ ! -e "$MDB_SECRET_STASH_DIR/_old_file" ]
}

@test "stash_copy never touches the original and skips duplicates" {
    mdb_init_secret_stash "$MDB_SECRET_STASH_DIR"
    mdb_stash_copy "${FILES[0]}" "$MDB_SECRET_STASH_DIR"
    mdb_stash_copy "${FILES[0]}" "$MDB_SECRET_STASH_DIR"
    cmp -s "${FILES[0]}" "${FILES[0]}.orig"
    [ "$(wc -l < "$MDB_SECRET_STASH_DIR/manifest" | tr -d ' ')" = "1" ]
}

# ---------------------------------------------------------------------------
# Restore
# ---------------------------------------------------------------------------

@test "restore plan decisions" {
    [ "$(mdb_secret_restore_plan '' 1 0 0)" = "none" ]
    [ "$(mdb_secret_restore_plan /s 1 1 0)" = "restore" ]
    [ "$(mdb_secret_restore_plan '' 1 1 0)" = "restore" ]
    [ "$(mdb_secret_restore_plan /s 1 1 1)" = "cleanup" ]
    [ "$(mdb_secret_restore_plan /s 1 0 1)" = "cleanup" ]
    [ "$(mdb_secret_restore_plan /s 1 0 0)" = "lost" ]
    [ "$(mdb_secret_restore_plan /s 0 0 0)" = "unreachable" ]
}

@test "reboot mid-clone: everything comes back from the drive stash" {
    fake_prepare
    for f in "${FILES[@]}"; do [ ! -e "$f" ]; done
    run mdb_restore_secrets "$BACKUP" 0
    [ "$status" -eq 0 ]
    assert_all_restored
    # stash securely removed, pointer gone
    [ ! -e "$MDB_SECRET_STASH_DIR" ]
    [ ! -e "$BACKUP/$MDB_STASH_POINTER_NAME" ]
}

@test "restore is idempotent: a second run is a no-op" {
    fake_prepare
    mdb_restore_secrets "$BACKUP" 0
    echo "newer live data" > "${FILES[1]}"
    run mdb_restore_secrets "$BACKUP" 0
    [ "$status" -eq 0 ]
    [ "$(cat "${FILES[1]}")" = "newer live data" ]
}

@test "interrupted after restore: re-run only cleans up, never copies stale data back" {
    fake_prepare
    mdb_copy_back_stash "$MDB_SECRET_STASH_DIR"
    : > "$MDB_SECRET_STASH_DIR/$MDB_STASH_RESTORED_NAME"
    echo "written by a service after restore" > "${FILES[1]}"
    run mdb_restore_secrets "$BACKUP" 0
    [ "$status" -eq 0 ]
    [[ "$output" == *"already restored"* ]]
    [ "$(cat "${FILES[1]}")" = "written by a service after restore" ]
    [ ! -e "$MDB_SECRET_STASH_DIR" ]
}

@test "pointer present but stash missing: exits non-zero LOUDLY and keeps state" {
    fake_prepare "$MDB_RAM_SECRET_STASH_DIR"
    rm -rf "$MDB_RAM_SECRET_STASH_DIR"          # the reboot that erased RAM
    run mdb_restore_secrets "$BACKUP" 0
    [ "$status" -eq 1 ]
    [[ "$output" == *"SECRET STASH IS MISSING"* ]]
    [ -s "$BACKUP/$MDB_STASH_POINTER_NAME" ]
}

@test "--accept-secret-loss lets restore continue and clears the pointer" {
    fake_prepare "$MDB_RAM_SECRET_STASH_DIR"
    rm -rf "$MDB_RAM_SECRET_STASH_DIR"
    run mdb_restore_secrets "$BACKUP" 1
    [ "$status" -eq 0 ]
    [ ! -e "$BACKUP/$MDB_STASH_POINTER_NAME" ]
}

@test "unreachable drive keeps the stash and the pointer" {
    # The stash is under the drive path, and the drive path is a plain
    # directory — exactly the drive being unplugged after the reboot.
    local s="$MDB_STASH_DRIVE/.mdb-secret-stash"
    fake_prepare "$s"
    run mdb_restore_secrets "$BACKUP" 0
    [ "$status" -eq 2 ]
    [[ "$output" == *"NOT mounted"* ]]
    [ -s "$s/manifest" ]
    [ -s "$BACKUP/$MDB_STASH_POINTER_NAME" ]
    for f in "${FILES[@]}"; do [ ! -e "$f" ]; done
}

@test "no marker, no pointer, but a stash on the drive is still found" {
    # first_boot.sh on a non-SD root wipes cloning_backup/ (pointer included)
    # while the stash on the drive survives.
    fake_prepare
    rm -f "$BACKUP/$MDB_STASH_POINTER_NAME"
    mdb_secret_stash_pending "$BACKUP"
    run mdb_restore_secrets "$BACKUP" 0
    [ "$status" -eq 0 ]
    assert_all_restored
}

@test "nothing pending when there is no pointer and no stash" {
    run mdb_secret_stash_pending "$BACKUP"
    [ "$status" -ne 0 ]
    run mdb_restore_secrets "$BACKUP" 0
    [ "$status" -eq 0 ]
    [[ "$output" == *"nothing to restore"* ]]
}

# ---------------------------------------------------------------------------
# Script wiring
# ---------------------------------------------------------------------------

@test "restore_after_cloning.sh exits 1 when the secret restore fails" {
    run grep -A4 'mdb_restore_secrets "$BACKUP_DIR"' "$GI_DIR/restore_after_cloning.sh"
    [[ "$output" == *'_secrets_rc=$?'* ]]
    run grep -A5 'if \[\[ "$_secrets_rc" -ne 0 \]\]' "$GI_DIR/restore_after_cloning.sh"
    [[ "$output" == *'exit 1'* ]]
}

@test "restore no longer reports a missing stash as nothing-to-restore" {
    # The old silent path: a log line saying so, then carry on and exit 0.
    run grep -n 'log "\[1/4\] No application-secret stash found' "$GI_DIR/restore_after_cloning.sh"
    [ "$status" -ne 0 ]
    # The secret restore runs before identity/services, not after them.
    local secrets_line identity_line
    secrets_line=$(grep -n 'mdb_restore_secrets "$BACKUP_DIR"' "$GI_DIR/restore_after_cloning.sh" | cut -d: -f1)
    identity_line=$(grep -n 'Restoring per-Pi identity' "$GI_DIR/restore_after_cloning.sh" | cut -d: -f1)
    [ "$secrets_line" -lt "$identity_line" ]
}

@test "sourcing the library runs nothing" {
    run bash -c "source '$LIB' && echo sourced-ok"
    [ "$status" -eq 0 ]
    [ "$output" = "sourced-ok" ]
}

@test "sync_source_box.sh pushes the library both scripts source" {
    grep -q 'scripts/golden_image/clone_stash_lib.sh' "$GI_DIR/sync_source_box.sh"
}

@test "shellcheck clean: clone tooling" {
    command -v shellcheck >/dev/null 2>&1 || skip "shellcheck not installed"
    cd "$GI_DIR"
    run shellcheck -x -S warning clone_stash_lib.sh prepare_for_cloning.sh \
        restore_after_cloning.sh clone_live_sd.sh sync_source_box.sh
    echo "$output"
    [ "$status" -eq 0 ]
}
