#!/usr/bin/env bats
#
# BATS tests for converge_system_units.sh — the installed systemd units are
# converged to the tree's unit files. Design:
# docs/superpowers/specs/2026-10-14-converge-system-units-design.md
#
# Every test runs against a fake tree whose path contains a space (the dev
# checkout lives under "magic_dingus_box /") and a fake root via
# MAGIC_TUNING_ROOT, so nothing touches the real /etc. MAGIC_SKIP_SYSTEMCTL
# stays true unless a test installs the systemctl stub.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")" && pwd)"
CONVERGE="$SCRIPT_DIR/../converge_system_units.sh"

setup() {
    TEST_TEMP_DIR="$(mktemp -d)"
    TREE="$TEST_TEMP_DIR/a tree"
    for d in systemd magic_dingus_box_cpp/systemd magic_dingus_box_cpp/scripts/missing_search \
             magic_dingus_box_cpp/scripts/episode_priority; do
        mkdir -p "$TREE/$d"
    done
    export MAGIC_TUNING_ROOT="$TEST_TEMP_DIR/root"
    export MAGIC_SKIP_SYSTEMCTL=true
    SD="$MAGIC_TUNING_ROOT/etc/systemd/system"
    STATE="$MAGIC_TUNING_ROOT/var/lib/magic-dingus/system_units"
    mkdir -p "$SD"
    # Every real tree ships the kiosk unit; the script refuses a tree without it.
    tree_unit magic_dingus_box_cpp/systemd/magic-dingus-box-cpp.service "kiosk v2"
    installed magic-dingus-box-cpp.service "kiosk v2"
}

teardown() {
    [ -n "$TEST_TEMP_DIR" ] && rm -rf "$TEST_TEMP_DIR"
}

# tree_unit <tree-relative path> <description> — a unit file in the fake tree
tree_unit() { printf '[Unit]\nDescription=%s\n' "$2" > "$TREE/$1"; }
# installed <name> <description> — the box's installed copy, same format
installed() { printf '[Unit]\nDescription=%s\n' "$2" > "$SD/$1"; }
converge() { run bash "$CONVERGE" --tree "$TREE" "$@"; }
ino() { ls -i "$1" | awk '{ print $1 }'; }
# `! cmd` mid-test never fails a bats test (set -e ignores negations).
refute() {
    if "$@"; then
        echo "unexpectedly succeeded: $*"
        return 1
    fi
}

@test "an installed unit identical to the tree is not rewritten" {
    tree_unit systemd/usb-gadget-network.service "gadget"
    installed usb-gadget-network.service "gadget"
    before="$(ino "$SD/usb-gadget-network.service")"
    converge
    [ "$status" -eq 0 ]
    [ "$(ino "$SD/usb-gadget-network.service")" = "$before" ]
    [[ "$output" != *"updated usb-gadget-network.service"* ]]
}

@test "a drifted unit is replaced with the tree's copy" {
    tree_unit systemd/usb-gadget-network.service "gadget v2"
    installed usb-gadget-network.service "gadget v1"
    converge
    [ "$status" -eq 0 ]
    cmp -s "$TREE/systemd/usb-gadget-network.service" "$SD/usb-gadget-network.service"
    [[ "$output" == *"updated usb-gadget-network.service"* ]]
}

@test "the write is a rename: new inode, no temp file left behind" {
    tree_unit systemd/usb-gadget-network.service "gadget v2"
    installed usb-gadget-network.service "gadget v1"
    before="$(ino "$SD/usb-gadget-network.service")"
    converge
    [ "$status" -eq 0 ]
    [ "$(ino "$SD/usb-gadget-network.service")" != "$before" ]
    [ -z "$(ls -A "$SD" | grep 'mdb-tmp' || true)" ]
}

@test "a masked unit (symlink to /dev/null) is never written" {
    tree_unit systemd/usb-gadget-network.service "gadget v2"
    ln -s /dev/null "$SD/usb-gadget-network.service"
    converge
    [ "$status" -eq 0 ]
    [ -L "$SD/usb-gadget-network.service" ]
    [ "$(readlink "$SD/usb-gadget-network.service")" = "/dev/null" ]
}

@test "a destination that is not a regular file is never written into" {
    tree_unit systemd/usb-gadget-network.service "gadget v2"
    mkdir "$SD/usb-gadget-network.service"
    converge
    [ "$status" -eq 2 ]
    [ -d "$SD/usb-gadget-network.service" ]
    [ -z "$(ls -A "$SD/usb-gadget-network.service")" ]
}

@test "an absent unit with a skip policy is not installed" {
    tree_unit magic_dingus_box_cpp/systemd/wifi-powersave-off.service "wifi"
    converge
    [ "$status" -eq 0 ]
    [ ! -e "$SD/wifi-powersave-off.service" ]
}

@test "absent units with install and enable policies are installed" {
    tree_unit systemd/usb-gadget-network.service "gadget"
    tree_unit systemd/magic-first-boot.service "first boot"
    converge
    [ "$status" -eq 0 ]
    cmp -s "$TREE/systemd/usb-gadget-network.service" "$SD/usb-gadget-network.service"
    cmp -s "$TREE/systemd/magic-first-boot.service" "$SD/magic-first-boot.service"
}

@test "an absent Media Browser unit is installed only when services/.env exists" {
    tree_unit magic_dingus_box_cpp/scripts/missing_search/magic-dingus-missing-search.timer "timer"
    converge
    [ "$status" -eq 0 ]
    [ ! -e "$SD/magic-dingus-missing-search.timer" ]
    mkdir -p "$TREE/services" && touch "$TREE/services/.env"
    converge
    [ "$status" -eq 0 ]
    cmp -s "$TREE/magic_dingus_box_cpp/scripts/missing_search/magic-dingus-missing-search.timer" \
        "$SD/magic-dingus-missing-search.timer"
}

@test "an installed Media Browser unit is kept current even without services/.env" {
    tree_unit magic_dingus_box_cpp/systemd/magic-dingus-storage-attach.service "attach 600"
    installed magic-dingus-storage-attach.service "attach 180"
    converge
    [ "$status" -eq 0 ]
    cmp -s "$TREE/magic_dingus_box_cpp/systemd/magic-dingus-storage-attach.service" \
        "$SD/magic-dingus-storage-attach.service"
}

@test "legacy (ignore) units are never touched" {
    tree_unit systemd/magic-ui.service "legacy v2"
    tree_unit systemd/magic-mpv.service "legacy"
    installed magic-ui.service "legacy v1"
    converge
    [ "$status" -eq 0 ]
    grep -qx "Description=legacy v1" "$SD/magic-ui.service"
    [ ! -e "$SD/magic-mpv.service" ]
}

@test "a unit with no policy row is kept current but never installed" {
    tree_unit systemd/brand-new.service "new"
    tree_unit systemd/other-new.timer "new v2"
    installed other-new.timer "new v1"
    converge
    [ "$status" -eq 0 ]
    [ ! -e "$SD/brand-new.service" ]
    cmp -s "$TREE/systemd/other-new.timer" "$SD/other-new.timer"
}

@test "a tree without the kiosk unit is refused and nothing changes" {
    rm "$TREE/magic_dingus_box_cpp/systemd/magic-dingus-box-cpp.service"
    tree_unit systemd/usb-gadget-network.service "gadget v2"
    installed usb-gadget-network.service "gadget v1"
    converge
    [ "$status" -eq 2 ]
    grep -qx "Description=gadget v1" "$SD/usb-gadget-network.service"
    [ ! -e "$STATE" ]
}

@test "a unit name shipped in two tree directories is not converged" {
    tree_unit systemd/usb-gadget-network.service "gadget A"
    tree_unit magic_dingus_box_cpp/systemd/usb-gadget-network.service "gadget B"
    converge
    [ "$status" -eq 2 ]
    [ ! -e "$SD/usb-gadget-network.service" ]
}

@test "the state file lists exactly the installed tree units" {
    tree_unit systemd/usb-gadget-network.service "gadget"
    tree_unit magic_dingus_box_cpp/systemd/wifi-powersave-off.service "wifi"
    tree_unit systemd/magic-ui.service "legacy"
    installed magic-ui.service "legacy"
    converge
    [ "$status" -eq 0 ]
    [ "$(cat "$STATE")" = "$(printf '%s\n' magic-dingus-box-cpp.service usb-gadget-network.service)" ]
}

@test "a second run changes nothing" {
    tree_unit systemd/usb-gadget-network.service "gadget v2"
    installed usb-gadget-network.service "gadget v1"
    converge
    [ "$status" -eq 0 ]
    converge
    [ "$status" -eq 0 ]
    [[ "$output" == *"done: 0 updated, 0 installed, 0 failed"* ]]
}

@test "--check reports DRIFT and MISSING, exits 1 and writes nothing" {
    tree_unit systemd/usb-gadget-network.service "gadget v2"
    installed usb-gadget-network.service "gadget v1"
    tree_unit systemd/magic-first-boot.service "first boot"
    converge --check
    [ "$status" -eq 1 ]
    [ "$output" = "$(printf '%s\n' 'MISSING magic-first-boot.service' 'DRIFT usb-gadget-network.service')" ]
    grep -qx "Description=gadget v1" "$SD/usb-gadget-network.service"
    [ ! -e "$SD/magic-first-boot.service" ]
    [ ! -e "$STATE" ]
}

@test "--check on a converged box prints nothing and exits 0" {
    tree_unit systemd/usb-gadget-network.service "gadget"
    installed usb-gadget-network.service "gadget"
    converge --check
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "without --tree the script converges the tree it lives in" {
    run bash "$CONVERGE"
    [ "$status" -eq 0 ]
    cmp -s "$SCRIPT_DIR/../../systemd/magic-dingus-box-cpp.service" "$SD/magic-dingus-box-cpp.service"
}
