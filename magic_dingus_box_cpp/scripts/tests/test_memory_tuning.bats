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

# The trigger list systemd builds from a timer unit plus its drop-ins, in
# load order, sorted. systemd.timer(5): an empty assignment to ANY trigger
# setting resets the WHOLE list (OnCalendar= and every On*Sec=), not just
# the key it names.
effective_timer_triggers() {
    awk '
        /^\[/ { sect = $0; next }
        sect != "[Timer]" { next }
        /^(OnActiveSec|OnBootSec|OnStartupSec|OnUnitActiveSec|OnUnitInactiveSec|OnCalendar)=/ {
            v = $0; sub(/^[^=]*=/, "", v)
            if (v == "") { n = 0; next }
            t[++n] = $0
        }
        END { for (i = 1; i <= n; i++) print t[i] }
    ' "$@" | sort
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

REPO_UNITS="$SCRIPT_DIR/../../systemd"
REPO_TIMER="$SCRIPT_DIR/../missing_search/magic-dingus-missing-search.timer"

# A drop-in exactly as the pre-2026-10 script wrote it (marker line first).
field_dropin() {
    mkdir -p "$(dirname "$1")"
    printf '# Magic Dingus Box (setup_memory_tuning.sh): fix drop-in\n%s\n' "$2" > "$1"
}

@test "no longer writes the unit-fix drop-ins" {
    run bash "$TUNING_SCRIPT"
    [ "$status" -eq 0 ]
    sys="$MAGIC_TUNING_ROOT/etc/systemd/system"
    [ ! -e "$sys/magic-dingus-box-cpp.service.d/stop-timeout.conf" ]
    [ ! -e "$sys/magic-dingus-storage-attach.service.d/mdb-timeout.conf" ]
    [ ! -e "$sys/magic-dingus-smoke-test.service.d/mdb-timeout.conf" ]
    [ ! -e "$sys/magic-dingus-missing-search.timer.d/mdb-boot-delay.conf" ]
}

@test "a field box: stale units replaced by the tree's, fix drop-ins removed" {
    sys="$MAGIC_TUNING_ROOT/etc/systemd/system"
    mkdir -p "$sys"
    cp "$REPO_UNITS/magic-dingus-box-cpp.service" "$sys/"
    field_dropin "$sys/magic-dingus-box-cpp.service.d/stop-timeout.conf" "$(printf '[Service]\nTimeoutStopSec=20')"
    printf '[Service]\nType=oneshot\nTimeoutStartSec=180\n' > "$sys/magic-dingus-storage-attach.service"
    field_dropin "$sys/magic-dingus-storage-attach.service.d/mdb-timeout.conf" "$(printf '[Service]\nTimeoutStartSec=600')"
    printf '[Service]\nType=oneshot\n' > "$sys/magic-dingus-smoke-test.service"
    field_dropin "$sys/magic-dingus-smoke-test.service.d/mdb-timeout.conf" "$(printf '[Service]\nTimeoutStartSec=300')"
    # The unit field boxes carried (3min), plus the drop-in that broke it.
    printf '[Timer]\nOnBootSec=3min\nOnUnitActiveSec=4h\nPersistent=true\n' > "$sys/magic-dingus-missing-search.timer"
    field_dropin "$sys/magic-dingus-missing-search.timer.d/mdb-boot-delay.conf" "$(printf '[Timer]\nOnBootSec=\nOnBootSec=11min')"

    run bash "$TUNING_SCRIPT"
    [ "$status" -eq 0 ]
    cmp -s "$REPO_UNITS/magic-dingus-storage-attach.service" "$sys/magic-dingus-storage-attach.service"
    cmp -s "$REPO_UNITS/magic-dingus-smoke-test.service" "$sys/magic-dingus-smoke-test.service"
    cmp -s "$REPO_TIMER" "$sys/magic-dingus-missing-search.timer"
    [ ! -e "$sys/magic-dingus-box-cpp.service.d/stop-timeout.conf" ]
    [ ! -e "$sys/magic-dingus-storage-attach.service.d" ]
    [ ! -e "$sys/magic-dingus-smoke-test.service.d" ]
    [ ! -e "$sys/magic-dingus-missing-search.timer.d" ]
    # What systemd now loads for the timer: the tree's triggers, 4 h repeat included.
    [ "$(effective_timer_triggers "$sys/magic-dingus-missing-search.timer")" = \
      "$(printf 'OnBootSec=11min\nOnUnitActiveSec=4h')" ]
    # Posture drop-ins are not unit fixes: they stay.
    [ -f "$sys/magic-dingus-box-cpp.service.d/memory-protect.conf" ]
}

@test "the unit converge runs before the audio unit install and the cmdline step" {
    converge_line="$(grep -n 'converge_system_units.sh"$' "$TUNING_SCRIPT" | head -1 | cut -d: -f1)"
    audio_line="$(grep -n '^# --- 1d\. ' "$TUNING_SCRIPT" | cut -d: -f1)"
    cmdline_line="$(grep -n '^# --- 4\. kernel cmdline' "$TUNING_SCRIPT" | cut -d: -f1)"
    [ -n "$converge_line" ] && [ -n "$audio_line" ] && [ -n "$cmdline_line" ]
    [ "$converge_line" -lt "$audio_line" ]
    [ "$converge_line" -lt "$cmdline_line" ]
}

@test "the unit converge call is guarded: its failure never fails the posture" {
    # The script finds its helpers beside itself, so a copy of it next to a
    # converge stub that exits 2 (the "something could not be converged"
    # code) injects the failure. Sibling helpers are deliberately absent:
    # each has a "not found" / test-mode branch.
    mkdir -p "$TEST_TEMP_DIR/scripts"
    cp "$TUNING_SCRIPT" "$TEST_TEMP_DIR/scripts/setup_memory_tuning.sh"
    cat > "$TEST_TEMP_DIR/scripts/converge_system_units.sh" <<'STUB'
#!/bin/bash
echo "[units] stub: pretending a unit could not be converged"
exit 2
STUB
    run bash "$TEST_TEMP_DIR/scripts/setup_memory_tuning.sh"
    [ "$status" -eq 0 ]
    [[ "$output" == *"stub: pretending a unit could not be converged"* ]]
    [[ "$output" == *"WARNING: unit convergence reported failures"* ]]
    # The rest of the posture still ran after the failed converge.
    [[ "$output" == *"memory posture converged"* ]]
    grep -q "vm.page-cluster = 0" "$MAGIC_TUNING_ROOT/etc/sysctl.d/99-mdb-zram.conf"
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

@test "usb0: a legacy 10-usb-gadget.network is moved aside (dingus.box fix)" {
    net="$MAGIC_TUNING_ROOT/etc/systemd/network"
    mkdir -p "$net"
    printf '[Match]\nName=usb0\n\n[Network]\nAddress=192.168.7.1/24\nDHCPServer=yes\n' \
        > "$net/10-usb-gadget.network"
    run bash "$TUNING_SCRIPT"
    [ "$status" -eq 0 ]
    [ ! -e "$net/10-usb-gadget.network" ]
    ls "$MAGIC_TUNING_ROOT/var/lib/magic-dingus/retired/10-usb-gadget.network."*
    [[ "$output" == *"retired legacy usb0 DHCP server"* ]]
    # Refresh-only: the hook never provisions the dnsmasq conf.
    [ ! -e "$MAGIC_TUNING_ROOT/etc/dnsmasq.d/usb0.conf" ]
}
