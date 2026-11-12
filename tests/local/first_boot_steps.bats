#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# first_boot.sh runs once on every unit sold. It used to be one linear run
# under `set -e`: the first failing command anywhere ended it and nothing
# below that line ran. On the first golden image (2026-08-04) `parted -s`
# exited 1 in Step 2, so no unit regenerated its identity, wiped the source
# box's saves / pairings / Wi-Fi password, or disabled the service.
#
# It now runs as isolated steps with a durable record. What that has to
# guarantee, and what these tests pin:
#   - a failing step never stops a later one (above all not the wipes);
#   - a step still stops at ITS first failing command (no ploughing on);
#   - a completed step is never run again — otherwise one permanently
#     failing harmless step would re-run the WIPES on every boot and destroy
#     the customer's own saves, pairings and Wi-Fi;
#   - a step that keeps failing is abandoned after a bounded number of
#     boots, the service disables itself, and the unit is flagged;
#   - the record never rides into an image, and is ignored on another board.
#
# The tests run the REAL engine and step list, sliced out of first_boot.sh by
# its `# >>> / # <<<` markers, with stub step functions (or, for the wipes,
# the real step bodies) against a scratch tree.

FB="$TESTS_REPO_ROOT/scripts/golden_image/first_boot.sh"
GI_DIR="$TESTS_REPO_ROOT/scripts/golden_image"
VERIFY_BOX="$CPP_DIR/scripts/verify_box.sh"

setup() {
    T="$(mktemp -d "${BATS_TMPDIR:-/tmp}/mdb-fbsteps.XXXXXX")"
    STUBS="$T/stubs"
    ROOT="$T/opt/magic_dingus_box"
    DATA="$ROOT/magic_dingus_box_cpp/data"
    BUILD_DATA="$ROOT/magic_dingus_box_cpp/build/data"
    NM="$T/etc/NetworkManager/system-connections"
    STATE="$T/state/10000000abcdef01"
    mkdir -p "$STUBS"
    # The device tree's serial carries a trailing NUL.
    printf '10000000abcdef01\0' > "$T/serial"
    stub systemctl 'printf "%s\n" "$*" >> "$CALLS"; exit 0'
    stub logger 'exit 0'
    stub sync 'exit 0'
    # The real step list, as first_boot.sh declares it.
    eval "$(sed -n '/^FB_STEP_TABLE=(/,/^)/p' "$FB")"
    STEP_NAMES=()
    local entry
    for entry in "${FB_STEP_TABLE[@]}"; do
        STEP_NAMES+=("${entry%%:*}")
    done
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
    echo "--- output:"; echo "$output"
    return 1
}
refute_has() {
    case "$output" in *"$1"*) echo "output unexpectedly contains: $1"; echo "$output"; return 1 ;; esac
    return 0
}

fb_slice() {   # MARKER -> the text between `# >>> MARKER` and `# <<< MARKER`
    sed -n "/^# >>> $1\$/,/^# <<< $1\$/p" "$FB"
}

# A top-level function of first_boot.sh. (Not usable for step_device_identity:
# its heredoc has a `}` in column 0.)
fb_function() {   # NAME
    sed -n "/^$1() {/,/^}/p" "$FB"
}

# The REAL body of a step, with the absolute paths it names moved under the
# scratch tree. Refuses a body that still names a system path: these tests
# delete what the step deletes.
real_step() {   # NAME
    local body
    body="$(fb_function "step_$1" | sed "s#/var/lib/magic-dingus/#$T/var/lib/magic-dingus/#g")"
    [ -n "$body" ] || { echo "step_$1 not found" >&2; return 1; }
    if printf '%s\n' "$body" | grep -v '^[[:space:]]*#' | sed "s#$T/##g" \
            | grep -E '(^|[ "=(])/(etc|var|home|opt|boot|root|run|usr)/' >&2; then
        echo "step_$1 names a system path — not safe to run in a test" >&2
        return 1
    fi
    printf '%s\n' "$body"
}

# Writes $T/run.sh: first_boot.sh's logging + engine + step list, with every
# step stubbed to append its name to $T/ran. $1 = step definitions that
# replace stubs; $2 = a sed expression applied to the "run steps" block.
make_run() {   # [STEP-DEFS] [RUN-BLOCK-SED]
    local name
    {
        echo 'set -euo pipefail'
        cat <<EOF
INSTALL_DIR="$ROOT"
CPP_DIR="\${INSTALL_DIR}/magic_dingus_box_cpp"
DATA_DIR="\${CPP_DIR}/data"
CONFIG_DIR="\${INSTALL_DIR}/config"
MAGIC_USER="magic"
MAGIC_HOME="$T/home/magic"
BUILD_DATA_DIR="\${CPP_DIR}/build/data"
NM_DIR="$NM"
FB_STATE_ROOT="\${FB_TEST_STATE_ROOT:-$T/state}"
FB_BOARD_SERIAL_FILE="\${FB_TEST_SERIAL_FILE:-$T/serial}"
FIRST_BOOT_LOG="$T/first-boot.log"
EOF
        fb_function log
        fb_function first_boot_failed
        grep -m1 "^trap 'first_boot_failed" "$FB"
        echo 'log "=== Magic Dingus Box first-boot setup starting ==="'
        fb_slice "step engine"
        for name in "${STEP_NAMES[@]}"; do
            printf 'step_%s() { echo "%s" >> "%s/ran"; }\n' "$name" "$name" "$T"
        done
        printf '%s\n' "${1:-}"
        fb_slice "run steps" | sed "${2:-}"
    } > "$T/run.sh"
    grep -q '^run_step() {' "$T/run.sh" || { echo "engine not sliced"; return 1; }
    grep -q '^fb_finish$' "$T/run.sh" || { echo "run block not sliced"; return 1; }
}

# One boot: runs $T/run.sh. What ran is in $T/ran (reset per boot), systemctl
# calls accumulate in $T/calls, the log in $T/first-boot.log.
boot() {
    rm -f "$T/ran"
    CALLS="$T/calls" PATH="$STUBS:$PATH" run bash "$T/run.sh"
}

ran() {   # the steps that ran on the last boot, space-separated
    [ -f "$T/ran" ] || return 0
    tr '\n' ' ' < "$T/ran" | sed 's/ $//'
}

disabled() {
    grep -qx 'disable magic-first-boot.service' "$T/calls" 2>/dev/null
}

all_steps() {
    printf '%s ' "${STEP_NAMES[@]}" | sed 's/ $//'
}

# A step that fails at its second command (and records that it started).
failing_step() {   # NAME
    cat <<EOF
step_$1() {
echo "$1" >> "$T/ran"
false
echo "AFTER-THE-FAILURE" >> "$T/ran"
}
EOF
}

# Source-box state under the scratch tree, as an image carries it.
seed_source_state() {
    mkdir -p "$DATA/saves/PCSX-ReARMed" "$DATA/states" "$DATA/media" \
             "$BUILD_DATA/saves" "$NM" "$T/home/magic/.config/retroarch/system"
    echo '{"phones":["source"]}' > "$DATA/paired_remotes.json"
    echo 'source-hmac' > "$DATA/flask_secret.key"
    echo 'source-hmac' > "$BUILD_DATA/flask_secret.key"
    echo '{"diag":1}' > "$DATA/remote_viewport_diag.jsonl"
    echo 'psk=source-wifi-password' > "$NM/SourceHome.nmconnection"
    echo 'srm' > "$DATA/saves/PCSX-ReARMed/game.srm"
    echo 'srm' > "$BUILD_DATA/saves/game.srm"
    echo 'video' > "$DATA/media/shipped.mp4"
    echo 'bios' > "$T/home/magic/.config/retroarch/system/scph5501.bin"
}

real_wipes() {
    real_step wipe_phone_remote
    real_step wipe_wifi
    real_step wipe_saves
}

# ---------------------------------------------------------------------------
# The step list
# ---------------------------------------------------------------------------

@test "every listed step has a function, and every step function is listed" {
    local name
    for name in "${STEP_NAMES[@]}"; do
        grep -q "^step_${name}() {\$" "$FB" || { echo "no function for step '$name'"; return 1; }
    done
    listed="$(printf '%s\n' "${STEP_NAMES[@]}" | sort)"
    defined="$(sed -n 's/^step_\([a-z0-9_]*\)() {$/\1/p' "$FB" | sort)"
    [ "$listed" = "$defined" ]
    # No step twice.
    [ "$(printf '%s\n' "${STEP_NAMES[@]}" | sort -u | wc -l)" -eq "${#STEP_NAMES[@]}" ]
}

@test "the steps run in the order the linear script had" {
    expected="ssh_host_keys machine_id vpn_identity expand_rootfs device_identity data_dirs"
    expected+=" wipe_media_browser reset_settings wipe_phone_remote wipe_personal_data wipe_wifi"
    expected+=" wipe_logs stale_journal_dirs:machine_id wipe_audit_docker_logs wipe_cloning_backup"
    expected+=" prune_pi4_content wipe_saves memory_posture ota_recovery update_channel"
    [ "${FB_STEP_TABLE[*]}" = "$expected" ]
}

@test "a step's NEEDS is a listed, earlier step with no NEEDS of its own" {
    local entry seen=" " needs
    for entry in "${FB_STEP_TABLE[@]}"; do
        if [[ "$entry" == *:* ]]; then
            needs="${entry#*:}"
            [[ "$seen" == *" $needs "* ]] || { echo "$entry: '$needs' is not an earlier plain step"; return 1; }
        else
            seen+="$entry "
        fi
    done
}

@test "run_step is only ever called as a plain statement (bash ignores set -e under if/||/&&)" {
    calls="$(grep -n 'run_step' "$FB" | grep -v '^[0-9]*:[[:space:]]*#' | grep -v '^[0-9]*:run_step() {')"
    [ "$(printf '%s\n' "$calls" | wc -l)" -eq 1 ]
    printf '%s\n' "$calls" | grep -qE '^[0-9]+:    run_step "\$FB_ENTRY"$'
    # ...and the step function itself is a plain statement inside it.
    grep -qE '^        "step_\$\{name\}"$' "$FB"
}

@test "BUILD_DATA_DIR is set once, before the steps (a step's variables do not reach the next)" {
    # Each step runs in its own subshell. The linear script assigned this
    # inside one step and read it in three later ones.
    [ "$(grep -c '^BUILD_DATA_DIR=' "$FB")" -eq 1 ]
    first_step="$(grep -n '^step_ssh_host_keys() {' "$FB" | cut -d: -f1)"
    [ "$(grep -n '^BUILD_DATA_DIR=' "$FB" | cut -d: -f1)" -lt "$first_step" ]
}

# ---------------------------------------------------------------------------
# Success path
# ---------------------------------------------------------------------------

@test "success: every step runs once in order, the service is disabled, the documented lines are logged" {
    make_run
    boot
    [ "$status" -eq 0 ]
    [ "$(ran)" = "$(all_steps)" ]
    disabled
    assert_has "[7/7] Disabling first-boot service..."
    assert_has "[7/7] magic-first-boot.service disabled (will not run again)"
    assert_has "=== Magic Dingus Box first-boot setup complete ==="
    refute_has "FAILED"
    refute_has "[skip]"
    # The log file is what survives the journal wipe and what verify_box reads.
    grep -q '=== Magic Dingus Box first-boot setup starting ===' "$T/first-boot.log"
    [ "$(tail -1 "$T/first-boot.log" | sed 's/^[^ ]* //')" = "=== Magic Dingus Box first-boot setup complete ===" ]
    # The record: the full list, and a .done per step.
    [ "$(tr '\n' ' ' < "$STATE/steps" | sed 's/ $//')" = "$(all_steps)" ]
    local name
    for name in "${STEP_NAMES[@]}"; do
        [ -e "$STATE/$name.done" ] || { echo "no .done for $name"; return 1; }
    done
}

@test "success: a second run repeats nothing" {
    make_run
    boot
    [ "$status" -eq 0 ]
    boot
    [ "$status" -eq 0 ]
    [ "$(ran)" = "" ]
    assert_has "[skip] wipe_saves: completed on an earlier boot"
    assert_has "=== Magic Dingus Box first-boot setup complete ==="
    # A by-hand "reset" of a finished unit must not look like it wiped anything.
    assert_has "=== Every step was already recorded complete — NOTHING was run."
    assert_has "sudo rm -rf $STATE "
}

# ---------------------------------------------------------------------------
# Isolation, and fail-fast inside a step
# ---------------------------------------------------------------------------

@test "a failing early step does not stop any later step" {
    make_run "$(failing_step expand_rootfs)"
    boot
    [ "$status" -eq 1 ]
    [ "$(ran)" = "$(all_steps)" ]
    refute_has "=== Magic Dingus Box first-boot setup complete ==="
    assert_has "=== FAILED: first-boot setup INCOMPLETE — steps not completed: expand_rootfs"
    assert_has "Retrying on the next boot: expand_rootfs"
    # Left enabled: the failed step gets another boot.
    ! disabled
    [ ! -e "$STATE/expand_rootfs.done" ]
    [ -e "$STATE/wipe_saves.done" ]
}

@test "a step still stops at ITS first failing command" {
    make_run "$(failing_step expand_rootfs)"
    boot
    ! grep -q 'AFTER-THE-FAILURE' "$T/ran"
}

@test "the failed step is logged with its name and failing line, once" {
    make_run "$(failing_step expand_rootfs)"
    boot
    assert_has "=== FAILED step expand_rootfs at line "
    assert_has "(exit 1): false"
    [ "$(grep -c '=== FAILED step expand_rootfs at line' "$T/first-boot.log")" -eq 1 ]
    # bash 3.2 (the Mac's) reports the function's first line in an ERR trap;
    # bash 4+ (every box) reports the failing command's.
    if bash -c '[ "${BASH_VERSINFO[0]}" -ge 4 ]'; then
        line="$(grep -n '^false$' "$T/run.sh" | head -1 | cut -d: -f1)"
        assert_has "=== FAILED step expand_rootfs at line ${line} (exit 1): false"
    fi
    # The preamble's whole-script trap must not claim the failure as its own.
    refute_has "=== FAILED at line"
}

@test "a failure inside a helper, a pipeline or a \$(...) fails the step once" {
    make_run 'helper() {
    local x
    x="$(false | cat)"
    echo "helper went on" >> "'"$T"'/ran"
}
step_machine_id() {
helper
echo "step went on" >> "'"$T"'/ran"
}
step_wipe_logs() {
find /nonexistent-mdb-dir -name x 2>/dev/null | wc -l > /dev/null
echo "pipeline went on" >> "'"$T"'/ran"
}'
    boot
    [ "$status" -eq 1 ]
    ! grep -q 'went on' "$T/ran"
    [ "$(grep -c '=== FAILED step machine_id at line' "$T/first-boot.log")" -eq 1 ]
    [ "$(grep -c '=== FAILED step wipe_logs at line' "$T/first-boot.log")" -eq 1 ]
    grep -q 'update_channel' "$T/ran"
}

@test "an unset variable or a killed step fails that step only" {
    make_run 'step_machine_id() {
echo "${MDB_NO_SUCH_VARIABLE}"
}
step_device_identity() {
kill -9 "$(exec sh -c '"'"'echo $PPID'"'"')"
}'
    boot
    [ "$status" -eq 1 ]
    assert_has "=== FAILED step machine_id (exit "
    assert_has "=== FAILED step device_identity (exit 137"
    grep -q '^update_channel$' "$T/ran"
    assert_has "steps not completed: machine_id device_identity stale_journal_dirs"
}

@test "steps that are guarded the way the real ones are do not fail" {
    # The idioms the real step bodies end with or lean on: a false `[[ ]] &&`,
    # an if with no branch taken, `|| true`, a guarded $(...), a function
    # called in a condition.
    make_run 'probe() { false; }
step_wipe_saves() {
[[ -d /nonexistent-mdb-dir ]] && { echo no; }
n=$(echo x | grep -c y) || n=0
if probe; then echo no; fi
( cd /nonexistent-mdb-dir 2>/dev/null && echo no || true )
find /nonexistent-mdb-dir -mindepth 1 -delete 2>/dev/null || true
echo "wipe_saves" >> "'"$T"'/ran"
if [[ -d /nonexistent-mdb-dir ]]; then echo no; fi
}'
    boot
    [ "$status" -eq 0 ]
    refute_has "FAILED"
    [ -e "$STATE/wipe_saves.done" ]
}

@test "the engine notices when it runs where bash ignores set -e" {
    # What a future `run_step x || ...` would do: every step would plough on
    # past its errors and be recorded as done.
    make_run "$(failing_step expand_rootfs)" 's/^    run_step "\$FB_ENTRY"$/    run_step "$FB_ENTRY" || true/'
    grep -q 'run_step "$FB_ENTRY" || true' "$T/run.sh"
    boot
    assert_has "=== FAILED: the step engine is running where bash ignores 'set -e'"
    grep -q 'AFTER-THE-FAILURE' "$T/ran"
}

@test "a closed stdout cannot end the run" {
    make_run
    rm -f "$T/ran"
    CALLS="$T/calls" PATH="$STUBS:$PATH" run bash -c 'bash "$1" >&- 2>&-' _ "$T/run.sh"
    [ "$status" -eq 0 ]
    [ "$(ran)" = "$(all_steps)" ]
    grep -q '=== Magic Dingus Box first-boot setup complete ===' "$T/first-boot.log"
}

# ---------------------------------------------------------------------------
# The wipes (real step bodies)
# ---------------------------------------------------------------------------

@test "wipes: a failing early step does not stop the credential and personal-data wipes" {
    seed_source_state
    make_run "$(failing_step ssh_host_keys)
$(failing_step expand_rootfs)
$(real_wipes)"
    boot
    [ "$status" -eq 1 ]
    assert_has "=== FAILED step ssh_host_keys at line"
    assert_has "=== FAILED step expand_rootfs at line"
    # Pairing + Flask secret (both copies), queues, Wi-Fi, saves (both copies).
    [ ! -e "$DATA/paired_remotes.json" ]
    [ ! -e "$DATA/flask_secret.key" ]
    [ ! -e "$BUILD_DATA/flask_secret.key" ]
    [ ! -e "$DATA/remote_viewport_diag.jsonl" ]
    [ ! -e "$NM/SourceHome.nmconnection" ]
    [ ! -e "$DATA/saves/PCSX-ReARMed" ]
    [ ! -e "$BUILD_DATA/saves/game.srm" ]
    # What ships stays.
    [ -f "$DATA/media/shipped.mp4" ]
    [ -f "$T/home/magic/.config/retroarch/system/scph5501.bin" ]
    [ -d "$DATA/saves" ]
    [ -e "$STATE/wipe_phone_remote.done" ]
    [ -e "$STATE/wipe_wifi.done" ]
    [ -e "$STATE/wipe_saves.done" ]
}

@test "wipes: the customer's own data survives the re-runs another step's failure causes" {
    seed_source_state
    make_run "$(failing_step expand_rootfs)
$(real_wipes)"
    boot
    [ "$status" -eq 1 ]
    [ ! -e "$DATA/paired_remotes.json" ]

    # The unit is in use now: the customer pairs a phone, joins Wi-Fi, plays.
    echo '{"phones":["customer"]}' > "$DATA/paired_remotes.json"
    echo 'customer-hmac' > "$DATA/flask_secret.key"
    echo 'psk=customer-wifi' > "$NM/CustomerHome.nmconnection"
    mkdir -p "$DATA/saves/Nestopia"
    echo 'progress' > "$DATA/saves/Nestopia/zelda.srm"

    # Boot 2: the expand fails again. Only it runs.
    boot
    [ "$status" -eq 1 ]
    [ "$(ran)" = "expand_rootfs" ]
    assert_has "[skip] wipe_phone_remote: completed on an earlier boot"
    assert_has "[skip] wipe_wifi: completed on an earlier boot"
    assert_has "[skip] wipe_saves: completed on an earlier boot"

    # Boot 3: the expand works. First boot completes.
    make_run "$(real_wipes)"
    boot
    [ "$status" -eq 0 ]
    [ "$(ran)" = "expand_rootfs" ]
    disabled
    assert_has "=== Magic Dingus Box first-boot setup complete ==="

    grep -q customer "$DATA/paired_remotes.json"
    grep -q customer-hmac "$DATA/flask_secret.key"
    grep -q customer-wifi "$NM/CustomerHome.nmconnection"
    grep -q progress "$DATA/saves/Nestopia/zelda.srm"
}

# ---------------------------------------------------------------------------
# Retries are bounded
# ---------------------------------------------------------------------------

@test "a step that keeps failing is retried on 3 boots, then abandoned and the unit flagged" {
    make_run "$(failing_step expand_rootfs)"
    boot
    [ "$status" -eq 1 ]
    ! disabled
    boot
    [ "$status" -eq 1 ]
    [ "$(ran)" = "expand_rootfs" ]
    assert_has "attempt 2 of 3"
    ! disabled

    # Third failure: no boots left for it — give up, loudly, and disarm.
    boot
    [ "$status" -eq 1 ]
    [ "$(ran)" = "expand_rootfs" ]
    assert_has "attempt 3 of 3"
    assert_has "=== GIVING UP on: expand_rootfs"
    assert_has "sudo rm -f $STATE/*.attempt.*"
    refute_has "Retrying on the next boot"
    disabled

    # A run after that (by hand) does not try it again, and still says FAILED.
    boot
    [ "$status" -eq 1 ]
    [ "$(ran)" = "" ]
    assert_has "[gave up] expand_rootfs: failed or was interrupted on 3 boots"
    assert_has "=== FAILED: first-boot setup INCOMPLETE — steps not completed: expand_rootfs"

    # verify_box.sh's reader flags exactly that step.
    eval "$(sed -n '/^first_boot_incomplete_steps() {/,/^}/p' "$VERIFY_BOX")"
    [ "$(first_boot_incomplete_steps "$STATE")" = "expand_rootfs" ]

    # The documented way back: clear the attempts once the cause is fixed.
    rm -f "$STATE"/*.attempt.*
    make_run
    boot
    [ "$status" -eq 0 ]
    [ "$(ran)" = "expand_rootfs" ]
    assert_has "=== Magic Dingus Box first-boot setup complete ==="
    [ "$(first_boot_incomplete_steps "$STATE")" = "" ]
}

@test "a step that takes the whole run down (power cut, hang) is counted too, then skipped" {
    # kill -9 of the script itself, from inside the step: nothing after the
    # attempt marker is written, as with a power cut mid-step.
    make_run 'step_expand_rootfs() {
echo "expand_rootfs" >> "'"$T"'/ran"
kill -9 $$
}'
    local i
    for i in 1 2 3; do
        boot
        [ "$status" -eq 137 ]
        ! disabled
    done
    [ -e "$STATE/expand_rootfs.attempt.3" ]
    # Boot 4: it is not started again, and everything after it finally runs.
    boot
    [ "$status" -eq 1 ]
    assert_has "[gave up] expand_rootfs"
    ! grep -q '^expand_rootfs$' "$T/ran"
    grep -q '^wipe_saves$' "$T/ran"
    grep -q '^update_channel$' "$T/ran"
    assert_has "=== GIVING UP on: expand_rootfs"
    disabled
}

@test "steps after the failed one are not charged an attempt for it" {
    make_run "$(failing_step expand_rootfs)"
    boot
    boot
    boot
    [ -e "$STATE/expand_rootfs.attempt.3" ]
    [ -e "$STATE/wipe_saves.attempt.1" ]
    [ ! -e "$STATE/wipe_saves.attempt.2" ]
}

# ---------------------------------------------------------------------------
# NEEDS: the old journal directory only goes once the machine-id has changed
# ---------------------------------------------------------------------------

@test "stale_journal_dirs waits for machine_id, uncounted, and runs once it completes" {
    make_run "$(failing_step machine_id)"
    boot
    [ "$status" -eq 1 ]
    ! grep -q '^stale_journal_dirs$' "$T/ran"
    assert_has "[wait] stale_journal_dirs: needs 'machine_id'"
    [ ! -e "$STATE/stale_journal_dirs.attempt.1" ]
    # The journal WIPE itself does not wait.
    grep -q '^wipe_logs$' "$T/ran"
    assert_has "Retrying on the next boot: machine_id stale_journal_dirs"

    make_run
    boot
    [ "$status" -eq 0 ]
    [ "$(ran)" = "machine_id stale_journal_dirs" ]
    disabled
}

@test "a step whose NEEDS was abandoned is abandoned with it" {
    make_run "$(failing_step machine_id)"
    boot
    boot
    boot
    [ "$status" -eq 1 ]
    assert_has "=== GIVING UP on: machine_id stale_journal_dirs"
    disabled
}

# ---------------------------------------------------------------------------
# The record: per board, never inherited, and what happens without one
# ---------------------------------------------------------------------------

@test "a record made on another board is ignored (an image that carried one)" {
    # The source box's complete record, baked into the image.
    mkdir -p "$T/state/10000000source00"
    local name
    for name in "${STEP_NAMES[@]}"; do
        : > "$T/state/10000000source00/$name.done"
    done
    make_run
    boot
    [ "$status" -eq 0 ]
    [ "$(ran)" = "$(all_steps)" ]
    refute_has "[skip]"
}

@test "an unreadable board serial still gets a record" {
    make_run
    FB_TEST_SERIAL_FILE="$T/no-such-file" boot
    [ "$status" -eq 0 ]
    [ -e "$T/state/unknown/wipe_saves.done" ]
    FB_TEST_SERIAL_FILE="$T/no-such-file" boot
    [ "$(ran)" = "" ]
}

@test "no record can be written: every step still runs, ONCE — the service is disabled whatever happened" {
    # Without a record a finished wipe would run again next boot and take the
    # customer's data with it.
    : > "$T/not-a-directory"
    make_run "$(failing_step expand_rootfs)"
    FB_TEST_STATE_ROOT="$T/not-a-directory/state" boot
    [ "$status" -eq 1 ]
    [ "$(ran)" = "$(all_steps)" ]
    assert_has "=== FAILED to record first-boot progress"
    [ "$(grep -c '=== FAILED to record first-boot progress' "$T/first-boot.log")" -eq 1 ]
    assert_has "=== GIVING UP on: expand_rootfs"
    assert_has "a re-run by hand repeats EVERY"
    disabled
}

@test "no record can be written, nothing failed: complete, and disabled" {
    : > "$T/not-a-directory"
    make_run
    FB_TEST_STATE_ROOT="$T/not-a-directory/state" boot
    [ "$status" -eq 0 ]
    [ "$(ran)" = "$(all_steps)" ]
    disabled
    assert_has "=== Magic Dingus Box first-boot setup complete ==="
    # ...but the unit is still flagged: verify_box FAILs on this line.
    assert_has "=== FAILED to record first-boot progress"
}

@test "the self-disable failing leaves every step recorded: the next boot only retries it" {
    stub systemctl 'printf "%s\n" "$*" >> "$CALLS"; exit 1'
    make_run
    boot
    [ "$status" -eq 1 ]
    assert_has "=== FAILED to disable magic-first-boot.service"
    refute_has "=== Magic Dingus Box first-boot setup complete ==="
    stub systemctl 'printf "%s\n" "$*" >> "$CALLS"; exit 0'
    boot
    [ "$status" -eq 0 ]
    [ "$(ran)" = "" ]
    assert_has "=== Magic Dingus Box first-boot setup complete ==="
}

@test "the record is outside the install tree and cloning_backup, and the markers are empty files" {
    root="$(sed -n 's/^FB_STATE_ROOT="\(.*\)"$/\1/p' "$FB")"
    [ "$root" = "/var/lib/magic-dingus-box/first_boot" ]
    make_run
    boot
    [ ! -s "$STATE/wipe_saves.done" ]
    [ ! -s "$STATE/wipe_saves.attempt.1" ]
}

# ---------------------------------------------------------------------------
# Clone prep clears the record; verify_box reads it
# ---------------------------------------------------------------------------

@test "prepare_for_cloning deletes the record before it arms first boot, and refuses to go on if it cannot" {
    f="$GI_DIR/prepare_for_cloning.sh"
    root="$(sed -n 's/^FB_STATE_ROOT="\(.*\)"$/\1/p' "$FB")"
    grep -qx "FIRST_BOOT_STATE_ROOT=\"$root\"" "$f"
    rm_line="$(grep -n '^rm -rf "\$FIRST_BOOT_STATE_ROOT"' "$f" | cut -d: -f1)"
    enable_line="$(grep -n 'systemctl enable magic-first-boot.service' "$f" | head -1 | cut -d: -f1)"
    [ -n "$rm_line" ] && [ -n "$enable_line" ]
    [ "$rm_line" -lt "$enable_line" ]
    # A leftover record is an abort, not a warning.
    body="$(sed -n "${rm_line},${enable_line}p" "$f")"
    printf '%s\n' "$body" | grep -q 'if \[\[ -e "\$FIRST_BOOT_STATE_ROOT" \]\]; then'
    printf '%s\n' "$body" | grep -q '^    exit 1$'
}

@test "prepare_golden_image deletes the record before it arms first boot, and verifies it" {
    f="$GI_DIR/prepare_golden_image.sh"
    root="$(sed -n 's/^FB_STATE_ROOT="\(.*\)"$/\1/p' "$FB")"
    grep -qx "FIRST_BOOT_STATE_ROOT=\"$root\"" "$f"
    rm_line="$(grep -n '^rm -rf "\$FIRST_BOOT_STATE_ROOT"' "$f" | cut -d: -f1)"
    enable_line="$(grep -n 'systemctl enable magic-first-boot.service' "$f" | head -1 | cut -d: -f1)"
    [ -n "$rm_line" ] && [ -n "$enable_line" ]
    [ "$rm_line" -lt "$enable_line" ]
    grep -q 'check_fail "First-boot step record still present' "$f"
}

@test "first_boot's own wipes never touch the record" {
    # Step 6d empties cloning_backup/; the record is its sibling, not inside.
    grep -q '^BACKUP_DIR="/var/lib/magic-dingus-box/cloning_backup"$' "$FB"
    ! grep -n 'magic-dingus-box/first_boot' "$FB" | grep -v '^[0-9]*:FB_STATE_ROOT=' | grep -v '^[0-9]*:#' || false
    # (The one hit allowed is the retry-by-hand hint the engine LOGS.)
    ! grep -nE '(rm|find) .*FB_STATE_(ROOT|DIR)' "$FB" | grep -v '^[0-9]*:[[:space:]]*log "' || false
}

@test "verify_box and first_boot name the record's directory the same way" {
    a="$(sed -n '/^fb_board_id() {/,/^}/p' "$FB")"
    b="$(sed -n '/^fb_board_id() {/,/^}/p' "$VERIFY_BOX")"
    [ -n "$a" ]
    [ "$a" = "$b" ]
    [ "$(sed -n 's/^FB_STATE_ROOT="\(.*\)"$/\1/p' "$FB")" = "$(sed -n 's/^FB_STATE_ROOT=\(.*\)$/\1/p' "$VERIFY_BOX")" ]
    [ "$(sed -n 's/^FB_BOARD_SERIAL_FILE="\(.*\)"$/\1/p' "$FB")" = "$(sed -n 's/^FB_BOARD_SERIAL_FILE=\(.*\)$/\1/p' "$VERIFY_BOX")" ]
    eval "$b"
    FB_BOARD_SERIAL_FILE="$T/serial"
    [ "$(fb_board_id)" = "10000000abcdef01" ]
    FB_BOARD_SERIAL_FILE="$T/no-such-file"
    [ "$(fb_board_id)" = "unknown" ]
}

@test "verify_box: lists the steps with no .done marker, in order" {
    eval "$(sed -n '/^first_boot_incomplete_steps() {/,/^}/p' "$VERIFY_BOX")"
    mkdir -p "$T/st"
    printf 'a\nb\nc\n' > "$T/st/steps"
    [ "$(first_boot_incomplete_steps "$T/st")" = "a b c" ]
    : > "$T/st/b.done"
    [ "$(first_boot_incomplete_steps "$T/st")" = "a c" ]
    : > "$T/st/a.done"; : > "$T/st/c.done"
    [ "$(first_boot_incomplete_steps "$T/st")" = "" ]
    # An attempt marker is not completion.
    printf 'a\nd\n' > "$T/st/steps"
    : > "$T/st/d.attempt.1"
    [ "$(first_boot_incomplete_steps "$T/st")" = "d" ]
}

@test "verify_box FAILs (not WARNs) a unit with an unfinished first-boot step" {
    grep -q 'fail "first-boot step(s) never completed: \${FB_MISSING}' "$VERIFY_BOX"
    grep -q 'FB_STATE="\${FB_STATE_ROOT}/\$(fb_board_id)"' "$VERIFY_BOX"
    # Keyed on a non-empty step list: a unit set up by an older first_boot.sh
    # has no record and is judged by its log, as before.
    grep -q 'if \[\[ -s "\${FB_STATE}/steps" \]\]; then' "$VERIFY_BOX"
}

@test "verify_box's log check still fails an incomplete run and passes a complete one" {
    # The summary lines first_boot.sh writes are the ones verify_box greps.
    grep -q 'FB_DONE="=== Magic Dingus Box first-boot setup complete ==="' "$VERIFY_BOX"
    grep -q 'grep -qF "=== FAILED"' "$VERIFY_BOX"
    make_run "$(failing_step expand_rootfs)"
    boot
    last="$(awk -v s="=== Magic Dingus Box first-boot setup starting ===" \
        'index($0, s) { buf = "" } { buf = buf $0 "\n" } END { printf "%s", buf }' "$T/first-boot.log")"
    printf '%s' "$last" | grep -qF "=== FAILED"
    ! printf '%s' "$last" | grep -qF "=== Magic Dingus Box first-boot setup complete ===" || false
    make_run
    boot
    last="$(awk -v s="=== Magic Dingus Box first-boot setup starting ===" \
        'index($0, s) { buf = "" } { buf = buf $0 "\n" } END { printf "%s", buf }' "$T/first-boot.log")"
    ! printf '%s' "$last" | grep -qF "=== FAILED" || false
    printf '%s' "$last" | grep -qF "=== Magic Dingus Box first-boot setup complete ==="
}

@test "the engine and first_boot.sh pass shellcheck at the gate's severity" {
    if ! command -v shellcheck >/dev/null 2>&1; then skip "shellcheck not installed"; fi
    run shellcheck -S info "$FB"
    [ "$status" -eq 0 ]
}
