#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# first_boot.sh Step 1b regenerates /etc/machine-id while systemd-journald is
# already running. journald names its directory after the id it read at
# startup and never re-reads it; journalctl only reads the CURRENT id's
# directory. So for the whole first boot every `journalctl -b` said "No
# journal files were found" (verify_box WARNed no platform line / mode
# unknown / canvas canary not logged on a v1.10.0 Pi 4B, 2026-10-26), and the
# image's old-id directory — the source box's last entries — survived the
# Step 6c-2 vacuum forever, because that vacuum only reaches the current id.
#
# Measured on Trixie's systemd 257: `journalctl --flush` / `--rotate` leave
# journald on the old directory; `systemctl restart systemd-journald` moves
# it, and a oneshot's stdout stream survives the restart (journald's fd
# store). These tests run the real function text from first_boot.sh against
# a scratch tree with a stubbed systemctl and a fake /proc.

FB="$TESTS_REPO_ROOT/scripts/golden_image/first_boot.sh"
OLD_ID="2d44dc6c8e9a43bc846415118e160657"
NEW_ID="3a0098f6e0324cfb9ad8fa5f1b5c663c"

setup() {
    T="$(mktemp -d "${BATS_TMPDIR:-/tmp}/mdb-fbjournal.XXXXXX")"
    STUBS="$T/stubs"
    mkdir -p "$STUBS" "$T/var/log/journal" "$T/run/log/journal" "$T/proc"
    printf '%s\n' "$NEW_ID" > "$T/machine-id"
    stub systemctl 'printf "%s\n" "$*" >> "$CALLS"; exit 0'
}

teardown() {
    rm -rf "$T"
}

stub() {   # NAME BODY
    printf '#!/bin/bash\n%s\n' "$2" > "$STUBS/$1"
    chmod +x "$STUBS/$1"
}

assert_has() {
    case "$output" in *"$1"*) return 0 ;; esac
    echo "output lacks: $1"
    return 1
}
refute_has() {
    case "$output" in *"$1"*) echo "output unexpectedly contains: $1"; return 1 ;; esac
    return 0
}

# Runs one first_boot.sh function under first_boot's own shell options and an
# ERR trap, with every path it touches pointed into the scratch tree.
run_fb_function() {   # CALL FUNCTION-NAME...
    local call="$1" name body=""
    shift
    for name in "$@"; do
        body+="$(sed -n "/^${name}() {/,/^}/p" "$FB")"$'\n'
        [[ "$body" == *"${name}() {"* ]] || { echo "${name} not found"; return 1; }
    done
    cat > "$T/run.sh" <<EOF
set -euo pipefail
trap 'echo "=== FAILED at line \$LINENO"' ERR
log() { echo "\$1"; }
MACHINE_ID_FILE="$T/machine-id"
JOURNAL_ROOTS=("$T/var/log/journal" "$T/run/log/journal")
PROC_ROOT="$T/proc"
$body
$call
echo "REACHED NEXT STEP"
EOF
    CALLS="$T/calls" PATH="$STUBS:$PATH" bash "$T/run.sh"
}

run_restart()  { run_fb_function restart_journald_for_machine_id restart_journald_for_machine_id; }
run_cleanup()  { run_fb_function remove_stale_machine_id_journals journal_dir_held_open remove_stale_machine_id_journals; }

journal_dir() {   # ROOT NAME
    mkdir -p "$T/$1/$2"
    printf 'entries\n' > "$T/$1/$2/system.journal"
}

# ---------------------------------------------------------------------------
# Placement in first_boot.sh
# ---------------------------------------------------------------------------

@test "journald restart runs in Step 1b, after the new id and before Step 1c" {
    setup_line="$(grep -n '^systemd-machine-id-setup' "$FB" | cut -d: -f1)"
    call_line="$(grep -n '^restart_journald_for_machine_id$' "$FB" | cut -d: -f1)"
    s1c="$(grep -n '^# Step 1c:' "$FB" | cut -d: -f1)"
    [ -n "$setup_line" ] && [ -n "$call_line" ] && [ -n "$s1c" ]
    [ "$setup_line" -lt "$call_line" ]
    [ "$call_line" -lt "$s1c" ]
}

@test "old-id cleanup runs in Step 6c-2, after the vacuum" {
    vacuum="$(grep -n '^journalctl --vacuum-time=1s' "$FB" | cut -d: -f1)"
    call_line="$(grep -n '^remove_stale_machine_id_journals$' "$FB" | cut -d: -f1)"
    s6d="$(grep -n '^# Step 6d:' "$FB" | cut -d: -f1)"
    [ -n "$vacuum" ] && [ -n "$call_line" ] && [ -n "$s6d" ]
    [ "$vacuum" -lt "$call_line" ]
    [ "$call_line" -lt "$s6d" ]
}

# ---------------------------------------------------------------------------
# Step 1b: restart_journald_for_machine_id
# ---------------------------------------------------------------------------

@test "Step 1b: RESTARTS journald (a flush or rotate does not move it)" {
    journal_dir var/log/journal "$NEW_ID"
    run run_restart
    [ "$status" -eq 0 ]
    grep -qx 'restart systemd-journald.service' "$T/calls"
    assert_has "journald now writing under the new machine-id"
    assert_has "REACHED NEXT STEP"
    refute_has "FAILED"
}

@test "Step 1b: a volatile (/run) journal directory counts too" {
    journal_dir run/log/journal "$NEW_ID"
    run run_restart
    [ "$status" -eq 0 ]
    assert_has "journald now writing under the new machine-id"
}

@test "Step 1b: a failed restart is logged and never aborts first boot" {
    stub systemctl 'exit 1'
    run run_restart
    [ "$status" -eq 0 ]
    assert_has "WARNING: could not restart journald"
    assert_has "REACHED NEXT STEP"
    refute_has "FAILED"
}

@test "Step 1b: no new-id directory after the restart is a warning, not an abort" {
    run run_restart
    [ "$status" -eq 0 ]
    assert_has "WARNING: journald restarted but no journal directory"
    assert_has "REACHED NEXT STEP"
}

@test "Step 1b: without a valid machine-id journald is left alone" {
    printf 'uninitialized\n' > "$T/machine-id"
    run run_restart
    [ "$status" -eq 0 ]
    [ ! -e "$T/calls" ]
    assert_has "WARNING: no valid machine-id"
    assert_has "REACHED NEXT STEP"
}

# ---------------------------------------------------------------------------
# Step 6c-2: remove_stale_machine_id_journals
# ---------------------------------------------------------------------------

@test "Step 6c-2: removes the old id's directories, keeps the current id's" {
    journal_dir var/log/journal "$OLD_ID"
    journal_dir var/log/journal "${OLD_ID}.mdbns"
    journal_dir run/log/journal "$OLD_ID"
    journal_dir var/log/journal "$NEW_ID"
    run run_cleanup
    [ "$status" -eq 0 ]
    [ ! -e "$T/var/log/journal/$OLD_ID" ]
    [ ! -e "$T/var/log/journal/${OLD_ID}.mdbns" ]
    [ ! -e "$T/run/log/journal/$OLD_ID" ]
    [ -f "$T/var/log/journal/$NEW_ID/system.journal" ]
    assert_has "Removed 3 journal directory(ies)"
    assert_has "REACHED NEXT STEP"
    refute_has "FAILED"
}

@test "Step 6c-2: leaves non-id entries alone (remote/, files, symlinks)" {
    journal_dir var/log/journal remote
    journal_dir var/log/journal "$NEW_ID"
    printf 'x\n' > "$T/var/log/journal/$OLD_ID"          # a FILE named like an id
    mkdir -p "$T/elsewhere"
    ln -s "$T/elsewhere" "$T/run/log/journal/$OLD_ID"    # a symlink named like an id
    run run_cleanup
    [ "$status" -eq 0 ]
    [ -d "$T/var/log/journal/remote" ]
    [ -f "$T/var/log/journal/$OLD_ID" ]
    [ -L "$T/run/log/journal/$OLD_ID" ]
    [ -d "$T/elsewhere" ]
    refute_has "Removed"
}

@test "Step 6c-2: a directory journald still has open is KEPT, loudly" {
    journal_dir var/log/journal "$OLD_ID"
    journal_dir var/log/journal "$NEW_ID"
    mkdir -p "$T/proc/123/fd"
    ln -s "$T/var/log/journal/$OLD_ID/system.journal" "$T/proc/123/fd/7"
    run run_cleanup
    [ "$status" -eq 0 ]
    [ -f "$T/var/log/journal/$OLD_ID/system.journal" ]
    assert_has "WARNING: kept $T/var/log/journal/$OLD_ID"
    assert_has "REACHED NEXT STEP"
}

@test "Step 6c-2: an open fd under the CURRENT id does not protect the old one" {
    journal_dir var/log/journal "$OLD_ID"
    journal_dir var/log/journal "$NEW_ID"
    mkdir -p "$T/proc/123/fd"
    ln -s "$T/var/log/journal/$NEW_ID/system.journal" "$T/proc/123/fd/7"
    run run_cleanup
    [ "$status" -eq 0 ]
    [ ! -e "$T/var/log/journal/$OLD_ID" ]
    [ -d "$T/var/log/journal/$NEW_ID" ]
}

@test "Step 6c-2: without a valid machine-id nothing is removed" {
    journal_dir var/log/journal "$OLD_ID"
    : > "$T/machine-id"
    run run_cleanup
    [ "$status" -eq 0 ]
    [ -d "$T/var/log/journal/$OLD_ID" ]
    assert_has "WARNING: no valid machine-id"
    assert_has "REACHED NEXT STEP"
}

@test "Step 6c-2: empty journal roots are a no-op" {
    rm -rf "$T/run/log/journal"
    run run_cleanup
    [ "$status" -eq 0 ]
    refute_has "Removed"
    refute_has "WARNING"
    assert_has "REACHED NEXT STEP"
}
