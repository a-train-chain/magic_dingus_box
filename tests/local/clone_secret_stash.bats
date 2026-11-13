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
# Content curation: the per-video store (media_info.json)
# ---------------------------------------------------------------------------
# The store names every video the box ever had a Crop setting or a length
# for — the operator's own file names, on every unit sold — and an orphaned
# crop:false would be inherited by a later upload of the same name. prepare
# moves the original into the content stash with the curated media (restore
# mv's it back) and ships a copy pruned to the videos that ship.

# A data dir with three videos: one shipping, one curated away, plus a store
# naming those, a video that is long gone, and fields/keys to carry over.
media_store_fixture() {
    D="$T/data"
    mkdir -p "$D/media/sub"
    : > "$D/media/ship.mp4"
    : > "$D/media/personal.mp4"
    : > "$D/media/sub/deep.mp4"
    cat > "$D/media_info.json" <<'JSON'
{
  "version": 1,
  "future_top_level": {"x": 1},
  "videos": {
    "media/ship.mp4": {"duration_s": 10.0, "display_aspect": 1.7778, "crop": false, "future": "kept"},
    "media/personal.mp4": {"duration_s": 3.0, "crop": false},
    "media/long_gone.mp4": {"crop": false},
    "media/sub/deep.mp4": {"display_aspect": 1.3333},
    "media/../../etc/passwd": {"crop": false},
    "/abs/x.mp4": {"crop": false}
  }
}
JSON
}

@test "media_info prune: keeps only the shipping videos, everything else unchanged" {
    media_store_fixture
    run mdb_prune_media_info "$D" "$T/pruned.json" "$D/media/personal.mp4"
    [ "$status" -eq 0 ]
    run python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
assert d["version"] == 1 and d["future_top_level"] == {"x": 1}, d
assert d["videos"] == {
    "media/ship.mp4": {"duration_s": 10.0, "display_aspect": 1.7778,
                       "crop": False, "future": "kept"},
    "media/sub/deep.mp4": {"display_aspect": 1.3333}}, d["videos"]
' "$T/pruned.json"
    echo "$output"
    [ "$status" -eq 0 ]
    # The original is never touched by the prune itself.
    grep -q personal.mp4 "$D/media_info.json"
    ! grep -q -e personal -e long_gone -e passwd "$T/pruned.json"
}

@test "media_info prune: nothing to remove leaves the store alone (10)" {
    D="$T/data"
    mkdir -p "$D/media"
    : > "$D/media/ship.mp4"
    printf '{"version":1,"videos":{"media/ship.mp4":{"crop":false}}}\n' > "$D/media_info.json"
    run mdb_prune_media_info "$D" "$T/pruned.json"
    [ "$status" -eq 10 ]
}

@test "media_info prune: an unreadable or wrong-shaped store ships as none" {
    D="$T/data"
    mkdir -p "$D/media"
    for body in '' '{oops' '[]' '{"version":1,"videos":[]}'; do
        printf '%s' "$body" > "$D/media_info.json"
        run mdb_prune_media_info "$D" "$T/pruned.json"
        [ "$status" -ne 0 ] && [ "$status" -ne 10 ] || { echo "body: $body -> $status"; false; }
    done
}

@test "prepare: the store is decided after curation and stashed with the content" {
    local f="$GI_DIR/prepare_for_cloning.sh"
    local curated_line prune_line move_line copy_line
    curated_line=$(grep -n 'unset _d _pl _base _keep _want _m _refs_file' "$f" | cut -d: -f1)
    prune_line=$(grep -n 'mdb_prune_media_info "\$_d"' "$f" | cut -d: -f1)
    move_line=$(grep -n 'mv "\$_src" "\${CONTENT_STASH}/\${_key}"' "$f" | cut -d: -f1)
    copy_line=$(grep -n 'cp "\$_pruned" "\$_store"' "$f" | cut -d: -f1)
    [ -n "$curated_line" ] && [ -n "$prune_line" ] && [ -n "$move_line" ] && [ -n "$copy_line" ]
    # Pruned against the full curated list, before anything moves; the
    # pruned copy is written only once the original is in the stash.
    [ "$curated_line" -lt "$prune_line" ]
    [ "$prune_line" -lt "$move_line" ]
    [ "$move_line" -lt "$copy_line" ]
    # The original rides the content manifest (that is what restore reads).
    run grep -n 'CONTENT_PATHS+=("\$_store")' "$f"
    [ "$status" -eq 0 ]
}

@test "restore: the manifest loop puts the original back over the pruned copy" {
    # Plain mv (no -n / --no-clobber): the stashed original replaces the
    # pruned copy prepare left at the same path.
    run grep -n 'mv "\${CONTENT_STASH}/\${key}" "\$dest"' "$GI_DIR/restore_after_cloning.sh"
    [ "$status" -eq 0 ]
    run grep -nE '^[^#]*mv (-[a-zA-Z]*n|--no-clobber)' "$GI_DIR/restore_after_cloning.sh"
    [ "$status" -ne 0 ]
}

@test "content stash round trip: shipping copy in the image, original back after" {
    # prepare's move + manifest and restore's manifest loop, as the scripts
    # do them, around the prune.
    media_store_fixture
    local stash="$T/content-stash" orig pruned key dest
    orig="$(cat "$D/media_info.json")"
    mkdir -p "$stash"; : > "$stash/manifest"
    pruned="$T/pruned.json"
    mdb_prune_media_info "$D" "$pruned" "$D/media/personal.mp4"
    for src in "$D/media/personal.mp4" "$D/media_info.json"; do
        key="$(printf '%s' "$src" | tr '/' '_')"
        mv "$src" "$stash/$key"
        printf '%s\t%s\n' "$key" "$src" >> "$stash/manifest"
    done
    cp "$pruned" "$D/media_info.json"
    # What the dd sees:
    ! grep -q -e personal -e long_gone "$D/media_info.json"
    grep -q ship.mp4 "$D/media_info.json"
    # restore_after_cloning.sh's loop:
    while IFS=$'\t' read -r key dest; do
        [[ -n "$key" && -n "$dest" ]] || continue
        [[ -f "$stash/${key}" ]] || continue
        mkdir -p "$(dirname "$dest")"
        mv "$stash/${key}" "$dest"
    done < "$stash/manifest"
    [ "$(cat "$D/media_info.json")" = "$orig" ]
    [ -f "$D/media/personal.mp4" ]
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
