#!/usr/bin/env bats
#
# BATS tests for setup_memory_tuning.sh — the playback memory posture
# (kiosk MemoryLow protection, zram readahead tune, cgroup memory
# controller on the kernel cmdline).
#
# Run with: bats test_memory_tuning.bats
#
# The script is exercised against a fake root via MAGIC_TUNING_ROOT so
# no test ever touches the real /etc or /boot. MAGIC_SKIP_SYSTEMCTL
# suppresses daemon-reload/sysctl exactly like update.sh's test mode.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")" && pwd)"
TUNING_SCRIPT="$SCRIPT_DIR/../setup_memory_tuning.sh"

setup() {
    TEST_TEMP_DIR="$(mktemp -d)"
    export MAGIC_TUNING_ROOT="$TEST_TEMP_DIR/root"
    export MAGIC_SKIP_SYSTEMCTL=true
    mkdir -p "$MAGIC_TUNING_ROOT/boot/firmware"
    # A realistic single-line Pi cmdline (no trailing newline, like the
    # real file rpi-imager writes).
    printf '%s' "console=serial0,115200 console=tty1 root=PARTUUID=dead-02 rootfstype=ext4 fsck.repair=yes rootwait quiet" \
        > "$MAGIC_TUNING_ROOT/boot/firmware/cmdline.txt"
}

teardown() {
    [ -n "$TEST_TEMP_DIR" ] && [ -d "$TEST_TEMP_DIR" ] && rm -rf "$TEST_TEMP_DIR"
}

@test "installs kiosk MemoryLow drop-in, slice companion, and zram sysctl" {
    run bash "$TUNING_SCRIPT"
    [ "$status" -eq 0 ]
    grep -q "MemoryLow=512M" \
        "$MAGIC_TUNING_ROOT/etc/systemd/system/magic-dingus-box-cpp.service.d/memory-protect.conf"
    grep -q "MemoryLow=512M" \
        "$MAGIC_TUNING_ROOT/etc/systemd/system/system.slice.d/mdb-memory.conf"
    grep -q "vm.page-cluster = 0" \
        "$MAGIC_TUNING_ROOT/etc/sysctl.d/99-mdb-zram.conf"
}

@test "kiosk drop-in carries OOMScoreAdjust=-500, exactly once across re-runs" {
    run bash "$TUNING_SCRIPT"
    [ "$status" -eq 0 ]
    run bash "$TUNING_SCRIPT"
    [ "$status" -eq 0 ]
    dropin="$MAGIC_TUNING_ROOT/etc/systemd/system/magic-dingus-box-cpp.service.d/memory-protect.conf"
    [ "$(grep -cx "OOMScoreAdjust=-500" "$dropin")" -eq 1 ]
    grep -qx "\[Service\]" "$dropin"
}

@test "installs the service timing drop-ins (storage-attach, smoke-test, missing-search)" {
    run bash "$TUNING_SCRIPT"
    [ "$status" -eq 0 ]
    sys="$MAGIC_TUNING_ROOT/etc/systemd/system"
    grep -qx "TimeoutStartSec=600" "$sys/magic-dingus-storage-attach.service.d/mdb-timeout.conf"
    grep -qx "TimeoutStartSec=300" "$sys/magic-dingus-smoke-test.service.d/mdb-timeout.conf"
    timer="$sys/magic-dingus-missing-search.timer.d/mdb-boot-delay.conf"
    grep -qx "\[Timer\]" "$timer"
    # Empty assignment first (resets the unit's 3min), then the new value.
    [ "$(grep -n '^OnBootSec=' "$timer" | head -1)" = "$(grep -n '^OnBootSec=$' "$timer")" ]
    grep -qx "OnBootSec=11min" "$timer"
}

@test "timing drop-ins agree with the in-tree units" {
    units="$SCRIPT_DIR/../../systemd"
    grep -qx "TimeoutStartSec=600" "$units/magic-dingus-storage-attach.service"
    grep -qx "TimeoutStartSec=300" "$units/magic-dingus-smoke-test.service"
    grep -qx "OnBootSec=11min" "$SCRIPT_DIR/../missing_search/magic-dingus-missing-search.timer"
}

@test "installs kiosk TimeoutStopSec=20 drop-in, idempotently" {
    run bash "$TUNING_SCRIPT"
    [ "$status" -eq 0 ]
    dropin="$MAGIC_TUNING_ROOT/etc/systemd/system/magic-dingus-box-cpp.service.d/stop-timeout.conf"
    grep -qx "TimeoutStopSec=20" "$dropin"
    grep -qx "\[Service\]" "$dropin"
    first="$(cat "$dropin")"
    run bash "$TUNING_SCRIPT"
    [ "$status" -eq 0 ]
    [ "$(cat "$dropin")" = "$first" ]
    [ "$(grep -c "TimeoutStopSec" "$dropin")" -eq 1 ]
}

@test "appends cgroup flags to cmdline.txt exactly once, keeping it single-line" {
    run bash "$TUNING_SCRIPT"
    [ "$status" -eq 0 ]
    cmdline="$MAGIC_TUNING_ROOT/boot/firmware/cmdline.txt"
    [ "$(grep -c "cgroup_enable=memory cgroup_memory=1" "$cmdline")" -eq 1 ]
    # Still one line (a multi-line cmdline.txt does not boot).
    [ "$(wc -l < "$cmdline")" -le 1 ]
    # Original content preserved.
    grep -q "root=PARTUUID=dead-02" "$cmdline"
}

@test "second run is a no-op: no duplicate flags, no second backup" {
    run bash "$TUNING_SCRIPT"
    [ "$status" -eq 0 ]
    first_pass="$(cat "$MAGIC_TUNING_ROOT/boot/firmware/cmdline.txt")"
    run bash "$TUNING_SCRIPT"
    [ "$status" -eq 0 ]
    [ "$(cat "$MAGIC_TUNING_ROOT/boot/firmware/cmdline.txt")" = "$first_pass" ]
    [ "$(ls "$MAGIC_TUNING_ROOT/boot/firmware/" | grep -c "cmdline.txt.bak")" -eq 1 ]
}

@test "cmdline already carrying the flags is left untouched (no backup written)" {
    printf '%s' "console=tty1 root=PARTUUID=dead-02 rootwait cgroup_enable=memory cgroup_memory=1" \
        > "$MAGIC_TUNING_ROOT/boot/firmware/cmdline.txt"
    run bash "$TUNING_SCRIPT"
    [ "$status" -eq 0 ]
    [ "$(ls "$MAGIC_TUNING_ROOT/boot/firmware/" | grep -c "cmdline.txt.bak")" -eq 0 ]
    [ "$(grep -c "cgroup_enable=memory" "$MAGIC_TUNING_ROOT/boot/firmware/cmdline.txt")" -eq 1 ]
}

@test "missing cmdline.txt (dev machine) skips the cmdline step but still installs drop-ins" {
    rm "$MAGIC_TUNING_ROOT/boot/firmware/cmdline.txt"
    run bash "$TUNING_SCRIPT"
    [ "$status" -eq 0 ]
    [ -f "$MAGIC_TUNING_ROOT/etc/systemd/system/magic-dingus-box-cpp.service.d/memory-protect.conf" ]
}

@test "reports REBOOT_REQUIRED only when it changed the cmdline" {
    run bash "$TUNING_SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"REBOOT_REQUIRED"* ]]
    run bash "$TUNING_SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" != *"REBOOT_REQUIRED"* ]]
}

# --- 1e. stale cascade watcher ----------------------------------------------
# restart_stale_cascade_watcher.sh with a stubbed systemctl: the stub reports
# the unit's state from files in $STUB_DIR and logs every call.

CASCADE_HELPER="$SCRIPT_DIR/../restart_stale_cascade_watcher.sh"

cascade_stubs() {
    STUB_DIR="$TEST_TEMP_DIR/stubs"
    mkdir -p "$STUB_DIR"
    CALLS="$TEST_TEMP_DIR/systemctl.log"
    : > "$CALLS"
    cat > "$STUB_DIR/systemctl" <<STUB
#!/bin/bash
echo "systemctl \$*" >> "$CALLS"
case "\$1" in
    is-active) cat "$STUB_DIR/active" 2>/dev/null || echo inactive ;;
    show)      cat "$STUB_DIR/started" 2>/dev/null ;;
esac
exit 0
STUB
    chmod +x "$STUB_DIR/systemctl"
    export PATH="$STUB_DIR:$PATH"
    WATCHER="$TEST_TEMP_DIR/gluetun_cascade_restart.sh"
    export MAGIC_CASCADE_WATCHER_BIN="$WATCHER"
}

file_mtime() { stat -c %Y "$1" 2>/dev/null || stat -f %m "$1"; }

# `! cmd` mid-test never fails a bats test (set -e ignores negations).
refute() {
    if "$@"; then
        echo "unexpectedly succeeded: $*"
        return 1
    fi
}

@test "cascade: restarts the watcher when the installed script is newer than the process" {
    cascade_stubs
    echo "#new" > "$WATCHER"
    echo active > "$STUB_DIR/active"
    echo "@$(( $(file_mtime "$WATCHER") - 3600 ))" > "$STUB_DIR/started"
    run bash "$CASCADE_HELPER"
    [ "$status" -eq 0 ]
    grep -qx "systemctl try-restart --no-block gluetun-cascade-restart.service" "$CALLS"
}

@test "cascade: no restart once the process is newer than the script (idempotent)" {
    cascade_stubs
    echo "#new" > "$WATCHER"
    echo active > "$STUB_DIR/active"
    echo "@$(( $(file_mtime "$WATCHER") + 60 ))" > "$STUB_DIR/started"
    run bash "$CASCADE_HELPER"
    [ "$status" -eq 0 ]
    [[ "$output" == *"already runs the installed script"* ]]
    refute grep -q "try-restart" "$CALLS"
}

@test "cascade: games-only box (watcher not installed) is a no-op" {
    cascade_stubs
    run bash "$CASCADE_HELPER"
    [ "$status" -eq 0 ]
    refute grep -q "try-restart" "$CALLS"
    refute grep -q "is-active" "$CALLS"
}

@test "cascade: an inactive unit is never started" {
    cascade_stubs
    echo "#new" > "$WATCHER"
    echo inactive > "$STUB_DIR/active"
    echo "@1" > "$STUB_DIR/started"
    run bash "$CASCADE_HELPER"
    [ "$status" -eq 0 ]
    refute grep -q "try-restart" "$CALLS"
}

@test "cascade: an unreadable start time leaves the watcher alone and exits 0" {
    cascade_stubs
    echo "#new" > "$WATCHER"
    echo active > "$STUB_DIR/active"
    echo "n/a" > "$STUB_DIR/started"
    run bash "$CASCADE_HELPER"
    [ "$status" -eq 0 ]
    refute grep -q "try-restart" "$CALLS"
}

@test "cascade: setup_memory_tuning.sh runs the helper before the cmdline step can exit" {
    helper_line="$(grep -n 'restart_stale_cascade_watcher.sh"$' "$TUNING_SCRIPT" | head -1 | cut -d: -f1)"
    cmdline_line="$(grep -n '^# --- 4\. kernel cmdline' "$TUNING_SCRIPT" | cut -d: -f1)"
    [ -n "$helper_line" ] && [ -n "$cmdline_line" ]
    [ "$helper_line" -lt "$cmdline_line" ]
    # Test mode never reaches a real systemctl.
    run bash "$TUNING_SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"SKIP: cascade watcher staleness check (test mode)"* ]]
}
