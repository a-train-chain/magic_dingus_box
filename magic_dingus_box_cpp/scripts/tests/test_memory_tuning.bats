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
