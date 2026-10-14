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
# converge_split [args] — like converge, but stdout and stderr kept apart:
# $status, $output (stdout only) and $errout (stderr only). Not `run
# --separate-stderr`, which older bats (apt's on some CI images) lacks.
converge_split() {
    status=0
    output="$(bash "$CONVERGE" --tree "$TREE" "$@" 2>"$TEST_TEMP_DIR/stderr")" || status=$?
    errout="$(cat "$TEST_TEMP_DIR/stderr")"
}
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

# --- a name in several tree directories: the first directory wins ----------
# Origin: deploy_cpp.sh stages copies of magic-dingus-box-cpp.service and
# magic-cpu-performance.service into the box's root systemd/ (untracked), so
# on every deployed box those names exist in two tree dirs. They used to be
# refused as "ships in two tree directories", which left the kiosk unit
# unconverged and made --check exit 2.

@test "a name in the root systemd/ and magic_dingus_box_cpp/systemd/ converges to the latter" {
    tree_unit systemd/magic-cpu-performance.service "cpu staged by an old deploy"
    tree_unit magic_dingus_box_cpp/systemd/magic-cpu-performance.service "cpu fresh"
    installed magic-cpu-performance.service "cpu old"
    tree_unit systemd/magic-dingus-box-cpp.service "kiosk staged by an old deploy"
    converge_split
    [ "$status" -eq 0 ]
    cmp -s "$TREE/magic_dingus_box_cpp/systemd/magic-cpu-performance.service" "$SD/magic-cpu-performance.service"
    cmp -s "$TREE/magic_dingus_box_cpp/systemd/magic-dingus-box-cpp.service" "$SD/magic-dingus-box-cpp.service"
    [[ "$output" == *"updated magic-cpu-performance.service"* ]]
    [[ "$output" == *"done: 1 updated, 0 installed, 0 failed"* ]]
    [[ "$output" != *"note:"* ]]
    [[ "$output" != *"ERROR"* ]]
    [ "$(grep -cx 'magic-cpu-performance.service' "$STATE")" -eq 1 ]
    [ "$(grep -cx 'magic-dingus-box-cpp.service' "$STATE")" -eq 1 ]
}

@test "directory precedence is magic_dingus_box_cpp/systemd, missing_search, episode_priority, then systemd" {
    installed other-new.timer "old"
    tree_unit systemd/other-new.timer "D root"
    tree_unit magic_dingus_box_cpp/scripts/episode_priority/other-new.timer "C episode_priority"
    tree_unit magic_dingus_box_cpp/scripts/missing_search/other-new.timer "B missing_search"
    converge_split
    [ "$status" -eq 0 ]
    grep -qx "Description=B missing_search" "$SD/other-new.timer"
    tree_unit magic_dingus_box_cpp/systemd/other-new.timer "A cpp systemd"
    converge_split
    [ "$status" -eq 0 ]
    grep -qx "Description=A cpp systemd" "$SD/other-new.timer"
}

@test "a shadow copy that differs from the winner is noted on stderr, never stdout" {
    tree_unit systemd/usb-gadget-network.service "gadget staged"
    tree_unit magic_dingus_box_cpp/systemd/usb-gadget-network.service "gadget real"
    installed usb-gadget-network.service "gadget real"
    converge_split
    [ "$status" -eq 0 ]
    [ "$errout" = "[units] note: $TREE/systemd/usb-gadget-network.service differs from $TREE/magic_dingus_box_cpp/systemd/usb-gadget-network.service; using the latter" ]
    [[ "$output" != *"note:"* ]]
    [ "$(grep -cx 'usb-gadget-network.service' "$STATE")" -eq 1 ]
}

@test "--check: a differing shadow copy leaves stdout as findings only, each printed once" {
    tree_unit systemd/usb-gadget-network.service "gadget staged"
    tree_unit magic_dingus_box_cpp/systemd/usb-gadget-network.service "gadget real"
    installed usb-gadget-network.service "gadget old"
    tree_unit systemd/magic-first-boot.service "first boot staged"
    tree_unit magic_dingus_box_cpp/systemd/magic-first-boot.service "first boot real"
    converge_split --check
    [ "$status" -eq 1 ]
    [ "$output" = "$(printf '%s\n' 'MISSING magic-first-boot.service' 'DRIFT usb-gadget-network.service')" ]
    [ "$(printf '%s\n' "$errout" | grep -c '^\[units\] note: ')" -eq 2 ]
    [ ! -e "$STATE" ]
}

@test "an identical shadow copy is silent on both streams" {
    tree_unit systemd/usb-gadget-network.service "gadget"
    tree_unit magic_dingus_box_cpp/systemd/usb-gadget-network.service "gadget"
    installed usb-gadget-network.service "gadget"
    converge_split --check
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    [ -z "$errout" ]
    converge_split
    [ "$status" -eq 0 ]
    [ -z "$errout" ]
    [[ "$output" == *"done: 0 updated, 0 installed, 0 failed"* ]]
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

# --- retirement + drop-in cleanup ----------------------------------------

# A systemctl stand-in: logs every call to $CALLS. `is-enabled U` succeeds
# when U is listed in $STUB_DIR/enabled; `show ... U` prints
# $STUB_DIR/substate.U (default "waiting"); everything else exits 0.
stub_systemctl() {
    export MAGIC_SKIP_SYSTEMCTL=false
    STUB_DIR="$TEST_TEMP_DIR/stubs"
    mkdir -p "$STUB_DIR"
    : > "$STUB_DIR/enabled"
    CALLS="$TEST_TEMP_DIR/systemctl.log"
    : > "$CALLS"
    cat > "$STUB_DIR/systemctl" <<STUB
#!/bin/bash
echo "systemctl \$*" >> "$CALLS"
for last; do :; done
case "\$1" in
    is-enabled) grep -qxF -- "\$last" "$STUB_DIR/enabled" ;;
    show)       cat "$STUB_DIR/substate.\$last" 2>/dev/null || echo waiting ;;
    *)          exit 0 ;;
esac
STUB
    chmod +x "$STUB_DIR/systemctl"
    export PATH="$STUB_DIR:$PATH"
}
# The calls that change something (queries filtered out).
mutations() { grep -vE '^systemctl (is-enabled|show) ' "$CALLS" || true; }

# old_dropin <unit> <file> <setting> — a drop-in as setup_memory_tuning.sh wrote it
old_dropin() {
    mkdir -p "$SD/$1.d"
    printf '# Magic Dingus Box (setup_memory_tuning.sh): fix drop-in\n[Service]\n%s\n' "$3" > "$SD/$1.d/$2"
}

@test "a state-file unit the tree no longer ships is disabled and removed" {
    stub_systemctl
    installed old-feature.timer "from the rolled-back release"
    mkdir -p "$(dirname "$STATE")"
    printf '%s\n' magic-dingus-box-cpp.service old-feature.timer > "$STATE"
    converge
    [ "$status" -eq 0 ]
    [ ! -e "$SD/old-feature.timer" ]
    grep -qx "systemctl disable --now --no-block old-feature.timer" "$CALLS"
    [[ "$output" == *"retired old-feature.timer"* ]]
    refute grep -qx old-feature.timer "$STATE"
}

@test "the update machinery is never retired" {
    installed magic-dingus-web.service "web"
    installed magic-dingus-ota-recovery.service "recovery"
    mkdir -p "$(dirname "$STATE")"
    printf '%s\n' magic-dingus-web.service magic-dingus-ota-recovery.service > "$STATE"
    converge
    [ "$status" -eq 0 ]
    [ -f "$SD/magic-dingus-web.service" ]
    [ -f "$SD/magic-dingus-ota-recovery.service" ]
}

@test "garbage state lines are never acted on" {
    echo "keep me" > "$MAGIC_TUNING_ROOT/etc/passwd"
    installed "a b.service" "spaces"
    mkdir -p "$(dirname "$STATE")"
    printf '%s\n' '../../passwd' 'a b.service' '/etc/x.service' '' > "$STATE"
    converge
    [ "$status" -eq 0 ]
    [ -f "$MAGIC_TUNING_ROOT/etc/passwd" ]
    [ -f "$SD/a b.service" ]
}

@test "a state-file name with a leading dash is never acted on (systemctl would read it as an option)" {
    stub_systemctl
    installed "-x.service" "dash"
    mkdir -p "$(dirname "$STATE")"
    printf '%s\n' -x.service > "$STATE"
    converge
    [ "$status" -eq 0 ]
    [ -f "$SD/-x.service" ]
    refute grep -qF -- '-x.service' "$CALLS"
    [[ "$output" != *"-x.service"* ]]
}

@test "a tree file with a leading dash is ignored, with a note on stderr only" {
    stub_systemctl
    tree_unit systemd/-bad.service "bad v2"
    installed "-bad.service" "bad v1"
    converge_split
    [ "$status" -eq 0 ]
    # Not kept current, not recorded, never handed to systemctl.
    [ "$(cat "$SD/-bad.service")" = "$(printf '[Unit]\nDescription=bad v1')" ]
    refute grep -qxF -- '-bad.service' "$STATE"
    [ -z "$(mutations)" ]
    [[ "$errout" == *"ignoring"*"-bad.service"* ]]
    [[ "$output" != *"-bad.service"* ]]
}

@test "a masked stale unit is left alone" {
    ln -s /dev/null "$SD/old-feature.timer"
    mkdir -p "$(dirname "$STATE")"
    echo old-feature.timer > "$STATE"
    converge
    [ "$status" -eq 0 ]
    [ -L "$SD/old-feature.timer" ]
}

@test "--check reports STALE without touching it" {
    installed old-feature.timer "stale"
    mkdir -p "$(dirname "$STATE")"
    echo old-feature.timer > "$STATE"
    converge --check
    [ "$status" -eq 1 ]
    [ "$output" = "STALE old-feature.timer" ]
    [ -f "$SD/old-feature.timer" ]
}

@test "a redundant fix drop-in is removed once its unit matches the tree" {
    tree_unit magic_dingus_box_cpp/systemd/magic-dingus-storage-attach.service "attach 600"
    installed magic-dingus-storage-attach.service "attach 180"
    old_dropin magic-dingus-storage-attach.service mdb-timeout.conf "TimeoutStartSec=600"
    converge
    [ "$status" -eq 0 ]
    [ ! -e "$SD/magic-dingus-storage-attach.service.d/mdb-timeout.conf" ]
    [ ! -d "$SD/magic-dingus-storage-attach.service.d" ]
    [[ "$output" == *"removed redundant drop-in magic-dingus-storage-attach.service.d/mdb-timeout.conf"* ]]
}

@test "a fix drop-in stays while its unit is not converged" {
    tree_unit magic_dingus_box_cpp/systemd/magic-dingus-storage-attach.service "attach 600"
    ln -s /dev/null "$SD/magic-dingus-storage-attach.service"
    old_dropin magic-dingus-storage-attach.service mdb-timeout.conf "TimeoutStartSec=600"
    converge
    [ -f "$SD/magic-dingus-storage-attach.service.d/mdb-timeout.conf" ]
}

@test "a drop-in without our marker is never removed" {
    installed magic-dingus-box-cpp.service "kiosk v2"
    mkdir -p "$SD/magic-dingus-box-cpp.service.d"
    printf '[Service]\nTimeoutStopSec=20\n' > "$SD/magic-dingus-box-cpp.service.d/stop-timeout.conf"
    converge
    [ "$status" -eq 0 ]
    [ -f "$SD/magic-dingus-box-cpp.service.d/stop-timeout.conf" ]
}

@test "other drop-ins of a converged unit stay" {
    old_dropin magic-dingus-box-cpp.service stop-timeout.conf "TimeoutStopSec=20"
    old_dropin magic-dingus-box-cpp.service memory-protect.conf "MemoryLow=512M"
    converge
    [ "$status" -eq 0 ]
    [ ! -e "$SD/magic-dingus-box-cpp.service.d/stop-timeout.conf" ]
    [ -f "$SD/magic-dingus-box-cpp.service.d/memory-protect.conf" ]
}

@test "--check reports DROPIN for a leftover drop-in on a converged unit" {
    old_dropin magic-dingus-box-cpp.service stop-timeout.conf "TimeoutStopSec=20"
    converge --check
    [ "$status" -eq 1 ]
    [ "$output" = "DROPIN $SD/magic-dingus-box-cpp.service.d/stop-timeout.conf" ]
    [ -f "$SD/magic-dingus-box-cpp.service.d/stop-timeout.conf" ]
}

@test "--check prints a leftover DROPIN once even when a shadow copy of its unit exists" {
    tree_unit systemd/magic-dingus-box-cpp.service "kiosk staged by an old deploy"
    old_dropin magic-dingus-box-cpp.service stop-timeout.conf "TimeoutStopSec=20"
    converge_split --check
    [ "$status" -eq 1 ]
    [ "$output" = "DROPIN $SD/magic-dingus-box-cpp.service.d/stop-timeout.conf" ]
}

# --- guards the retirement/cleanup steps must keep (deleting any of these
# --- checks used to leave the suite green) ----------------------------------

@test "a unit the tree still ships is never retired, even when the state file lists it" {
    stub_systemctl
    tree_unit systemd/usb-gadget-network.service "gadget"
    installed usb-gadget-network.service "gadget"
    mkdir -p "$(dirname "$STATE")"
    printf '%s\n' magic-dingus-box-cpp.service usb-gadget-network.service > "$STATE"
    converge
    [ "$status" -eq 0 ]
    [ -f "$SD/usb-gadget-network.service" ]
    refute grep -q disable "$CALLS"
}

@test "--check: a fix drop-in on a drifted unit is not reported as redundant" {
    tree_unit magic_dingus_box_cpp/systemd/magic-dingus-storage-attach.service "attach 600"
    installed magic-dingus-storage-attach.service "attach 180"
    old_dropin magic-dingus-storage-attach.service mdb-timeout.conf "TimeoutStartSec=600"
    converge --check
    [ "$status" -eq 1 ]
    [ "$output" = "DRIFT magic-dingus-storage-attach.service" ]
}

@test "a fix drop-in stays when its unit could not be brought to the tree's copy" {
    tree_unit magic_dingus_box_cpp/systemd/magic-dingus-storage-attach.service "attach 600"
    installed magic-dingus-storage-attach.service "attach 180"
    old_dropin magic-dingus-storage-attach.service mdb-timeout.conf "TimeoutStartSec=600"
    # The rename onto the unit cannot succeed (a directory sits on the temp
    # name), so the unit keeps its old text and the fix must keep carrying it.
    mkdir "$SD/.magic-dingus-storage-attach.service.mdb-tmp"
    converge
    [ "$status" -eq 2 ]
    grep -qx "Description=attach 180" "$SD/magic-dingus-storage-attach.service"
    [ -f "$SD/magic-dingus-storage-attach.service.d/mdb-timeout.conf" ]
}

# in_list <newline-separated list> <name>: a whole-line, literal match with no
# pipe (a `| grep -q` under pipefail reports failure when the reader exits
# early and the writer dies of SIGPIPE).
in_list_t() { bash -c 'source "$1"; shift; in_list "$@"' _ "$CONVERGE" "$@"; }

@test "in_list matches whole lines only and takes the name literally" {
    list=$'a.service\nb.timer\nc.service'
    in_list_t "$list" a.service
    in_list_t "$list" b.timer
    in_list_t "$list" c.service
    refute in_list_t "$list" aa.service
    refute in_list_t "$list" a.service.d
    refute in_list_t "$list" a
    refute in_list_t "$list" .service
    refute in_list_t "$list" 'a.servic?'
    refute in_list_t "$list" '*.service'
    refute in_list_t "$list" '[ab].service'
}

@test "in_list on an empty list matches nothing" {
    refute in_list_t "" a.service
    refute in_list_t $'\n' a.service
}

# --- apply phase -----------------------------------------------------------

@test "a converged box makes no systemctl changes" {
    stub_systemctl
    converge
    [ "$status" -eq 0 ]
    [ -z "$(mutations)" ]
}

@test "any write triggers exactly one daemon-reload, before every enable and restart" {
    stub_systemctl
    tree_unit systemd/usb-gadget-network.service "gadget"
    tree_unit magic_dingus_box_cpp/systemd/kiosk-standby-watcher.service "watcher v2"
    installed kiosk-standby-watcher.service "watcher v1"
    converge
    [ "$status" -eq 0 ]
    [ "$(grep -cx 'systemctl daemon-reload' "$CALLS")" -eq 1 ]
    [ "$(mutations | head -1)" = "systemctl daemon-reload" ]
}

@test "fresh installs: enable, enable-now and install policies" {
    stub_systemctl
    mkdir -p "$TREE/services" && touch "$TREE/services/.env"
    tree_unit systemd/usb-gadget-network.service "gadget"
    tree_unit systemd/magic-first-boot.service "first boot"
    tree_unit magic_dingus_box_cpp/scripts/missing_search/magic-dingus-missing-search.timer "timer"
    tree_unit magic_dingus_box_cpp/scripts/missing_search/magic-dingus-missing-search.service "sweep"
    converge
    [ "$status" -eq 0 ]
    grep -qx "systemctl enable usb-gadget-network.service" "$CALLS"
    grep -qx "systemctl enable --now --no-block magic-dingus-missing-search.timer" "$CALLS"
    refute grep -q "enable.*magic-first-boot.service" "$CALLS"
    refute grep -q "enable.*magic-dingus-missing-search.service" "$CALLS"
}

@test "an installed but disabled unit is never enabled" {
    stub_systemctl
    tree_unit systemd/usb-gadget-network.service "gadget v2"
    installed usb-gadget-network.service "gadget v1"
    converge
    [ "$status" -eq 0 ]
    refute grep -qE "^systemctl enable" "$CALLS"
}

@test "the kiosk is never restarted, stopped or try-restarted, even when its unit changes" {
    stub_systemctl
    tree_unit magic_dingus_box_cpp/systemd/magic-dingus-box-cpp.service "kiosk v3"
    echo magic-dingus-box-cpp.service > "$STUB_DIR/enabled"
    converge
    [ "$status" -eq 0 ]
    [ -z "$(mutations | grep -E '(restart|stop|disable|--now).*magic-dingus-box-cpp' || true)" ]
}

@test "try-restart only for a changed try-restart unit" {
    stub_systemctl
    tree_unit magic_dingus_box_cpp/systemd/kiosk-standby-watcher.service "watcher v2"
    installed kiosk-standby-watcher.service "watcher v1"
    tree_unit magic_dingus_box_cpp/systemd/content-manager-redirect.service "redirect"
    installed content-manager-redirect.service "redirect"
    converge
    [ "$status" -eq 0 ]
    grep -qx "systemctl --no-block try-restart kiosk-standby-watcher.service" "$CALLS"
    refute grep -q "content-manager-redirect" "$CALLS"
}

@test "a Media Browser try-restart unit is left alone without services/.env" {
    stub_systemctl
    tree_unit magic_dingus_box_cpp/systemd/gluetun-cascade-restart.service "watcher v2"
    installed gluetun-cascade-restart.service "watcher v1"
    converge
    [ "$status" -eq 0 ]
    cmp -s "$TREE/magic_dingus_box_cpp/systemd/gluetun-cascade-restart.service" "$SD/gluetun-cascade-restart.service"
    refute grep -q "try-restart gluetun-cascade-restart" "$CALLS"
}

@test "an enabled elapsed timer is re-armed; waiting or disabled ones are not" {
    stub_systemctl
    mkdir -p "$TREE/services" && touch "$TREE/services/.env"
    tree_unit magic_dingus_box_cpp/scripts/missing_search/magic-dingus-missing-search.timer "ms"
    installed magic-dingus-missing-search.timer "ms"
    tree_unit magic_dingus_box_cpp/scripts/episode_priority/magic-dingus-episode-priority.timer "ep"
    installed magic-dingus-episode-priority.timer "ep"
    tree_unit magic_dingus_box_cpp/systemd/magic-dingus-smoke-test.timer "smoke"
    installed magic-dingus-smoke-test.timer "smoke"
    printf '%s\n' magic-dingus-missing-search.timer magic-dingus-episode-priority.timer > "$STUB_DIR/enabled"
    echo elapsed > "$STUB_DIR/substate.magic-dingus-missing-search.timer"
    echo elapsed > "$STUB_DIR/substate.magic-dingus-smoke-test.timer"
    converge
    [ "$status" -eq 0 ]
    grep -qx "systemctl --no-block restart magic-dingus-missing-search.timer" "$CALLS"
    refute grep -q "restart magic-dingus-episode-priority.timer" "$CALLS"
    refute grep -q "restart magic-dingus-smoke-test.timer" "$CALLS"
}

@test "an elapsed Media Browser timer is not re-armed without services/.env" {
    stub_systemctl
    tree_unit magic_dingus_box_cpp/scripts/missing_search/magic-dingus-missing-search.timer "t"
    installed magic-dingus-missing-search.timer "t"
    echo magic-dingus-missing-search.timer > "$STUB_DIR/enabled"
    echo elapsed > "$STUB_DIR/substate.magic-dingus-missing-search.timer"
    converge
    [ "$status" -eq 0 ]
    refute grep -q "restart magic-dingus-missing-search.timer" "$CALLS"
}

@test "every start, stop and restart is --no-block (safe at boot)" {
    stub_systemctl
    mkdir -p "$TREE/services" && touch "$TREE/services/.env"
    tree_unit magic_dingus_box_cpp/scripts/missing_search/magic-dingus-missing-search.timer "t"
    tree_unit magic_dingus_box_cpp/systemd/kiosk-standby-watcher.service "watcher v2"
    installed kiosk-standby-watcher.service "watcher v1"
    tree_unit magic_dingus_box_cpp/scripts/episode_priority/magic-dingus-episode-priority.timer "e"
    installed magic-dingus-episode-priority.timer "e"
    echo magic-dingus-episode-priority.timer > "$STUB_DIR/enabled"
    echo elapsed > "$STUB_DIR/substate.magic-dingus-episode-priority.timer"
    installed old-feature.timer "stale"
    mkdir -p "$(dirname "$STATE")" && echo old-feature.timer > "$STATE"
    converge
    [ "$status" -eq 0 ]
    [ "$(mutations | grep -cE ' (start|stop|restart|try-restart) |--now')" -eq 4 ]
    [ -z "$(mutations | grep -E ' (start|stop|restart|try-restart) |--now' | grep -v -- '--no-block')" ]
}

@test "retirement alone triggers the daemon-reload" {
    stub_systemctl
    installed old-feature.timer "stale"
    mkdir -p "$(dirname "$STATE")" && echo old-feature.timer > "$STATE"
    converge
    [ "$status" -eq 0 ]
    grep -qx "systemctl daemon-reload" "$CALLS"
}

@test "test mode never calls systemctl" {
    stub_systemctl
    export MAGIC_SKIP_SYSTEMCTL=true
    tree_unit systemd/usb-gadget-network.service "gadget"
    converge
    [ "$status" -eq 0 ]
    [ ! -s "$CALLS" ]
}
