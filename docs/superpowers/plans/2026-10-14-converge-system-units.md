# Converge System Units Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every delivery path (OTA, deploy, first boot, source-box sync, and all three rollbacks) makes the systemd units installed in `/etc/systemd/system` equal to the box's tree, with a declared per-unit policy, so unit fixes ship as unit-file edits instead of hand-written drop-ins.

**Architecture:** One new bash script, `magic_dingus_box_cpp/scripts/converge_system_units.sh`, holds a policy table and runs plan → write → retire → drop-in cleanup → daemon-reload → apply → state. `setup_memory_tuning.sh` (the hook every path runs) calls it first; `update.sh`'s rollbacks call the restored tree's copy (or a stashed copy with `--tree`). `verify_box.sh` FAILs drift via the script's read-only `--check`.

**Tech Stack:** bash (must run under macOS `/bin/bash` 3.2 for the Mac test suites: no associative arrays, no `mapfile`, no `${x,,}`; macOS has no `flock`), bats-core 1.13, shellcheck, systemd on Raspberry Pi OS Trixie.

**Spec:** `docs/superpowers/specs/2026-10-14-converge-system-units-design.md` (approved 2026-10-14 with all four recommended decisions: install missing MB units whose ExecStart is in the tree; state-file retirement; verify_box FAIL on drift; skip `wifi-powersave-off` / `magic-cpu-performance` when absent).

## Global Constraints

- **No unit-file edits in this release.** Every `*.service` / `*.timer` must stay byte-identical to v1.10.0 (release commit `5eb1769`). Stale comments inside units ("OTA never replaces installed units") are fixed in the NEXT release. Check: `git diff --stat 5eb1769 HEAD -- systemd magic_dingus_box_cpp/systemd 'magic_dingus_box_cpp/scripts/missing_search/*.service' 'magic_dingus_box_cpp/scripts/missing_search/*.timer' 'magic_dingus_box_cpp/scripts/episode_priority/*.service' 'magic_dingus_box_cpp/scripts/episode_priority/*.timer'` must list no unit file.
- The converge **never fails its caller**: `setup_memory_tuning.sh`, `update.sh` and `first_boot.sh` log a warning and continue.
- The converge **never restarts, stops or retires** `magic-dingus-box-cpp.service`, `magic-dingus-web.service`, `magic-dingus-ota-recovery.service`.
- An installed unit's **enablement is never changed**; a symlinked (masked) destination is **never written**.
- Every `start`/`stop`/`restart`/`try-restart`/`--now` call carries `--no-block` (the script also runs at boot).
- Media Browser (`mb`) units are installed, enabled, started or re-armed only when `<tree>/services/.env` exists.
- Test seams follow `setup_memory_tuning.sh`: `MAGIC_TUNING_ROOT` (fake `/etc`, `/var/lib`, `/run/lock`; skips the root check), `MAGIC_SKIP_SYSTEMCTL=true` (no systemctl at all).
- `update.sh` writes JSON to stdout for the web admin: anything new it runs prints to stderr.
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **A tree path with spaces** (the dev checkout lives under `magic_dingus_box /`) — every compare, write and `--tree` must work. Pinned by Task 1's fixture tree path (`"$TEST_TEMP_DIR/a tree"`).
2. **A wrong or half-restored `--tree`** (empty dir, failed rsync) must not retire every installed unit — refuse a tree without the kiosk unit. Pinned in Task 1.
3. **A corrupt or hostile state-file line** (`../../passwd`, spaces) must never reach `rm`. Pinned in Task 2.
4. **Running at boot** (first boot, power-cut recovery): no blocking systemctl job. Pinned in Task 3 (every mutating call carries `--no-block`).
5. **A rollback whose converge fails or prints** must still succeed and keep stdout JSON-clean. Pinned in Task 6.

---

### Task 1: The converge script — file convergence, `--check`, state

**Files:**
- Create: `magic_dingus_box_cpp/scripts/converge_system_units.sh`
- Create: `magic_dingus_box_cpp/scripts/tests/test_converge_system_units.bats`

**Interfaces:**
- Produces (globals, set in `main`): `TREE`, `CHECK` (`true|false`), `SKIP_SYSTEMCTL`, `SD` (`$MAGIC_TUNING_ROOT/etc/systemd/system`), `STATE` (`$MAGIC_TUNING_ROOT/var/lib/magic-dingus/system_units`), `LIST` (lines `name<TAB>path`), `DUPS`, `MB` (`true|false`), `FINDINGS`, `FAILURES`, `RELOAD`, `WRITTEN`, `INSTALLED` (newline-terminated name lists).
- Produces (functions): `unit_policy NAME` → `"gate if-absent on-change drop-in"`; `policy_names`; `tree_units`; `is_protected NAME`; `in_list LIST NAME`; `write_unit SRC DEST`; `converge_files`; `write_state`; `main`.
- Produces (CLI): `converge_system_units.sh [--check] [--tree DIR]`; exit 0 / 1 (`--check` findings, usage) / 2 (failures, refused tree). `--check` prints `DRIFT <unit>` / `MISSING <unit>` on stdout.
- The script is sourceable: `main` runs only when executed (`[[ "${BASH_SOURCE[0]}" == "$0" ]]`).

- [ ] **Step 1: Write the failing tests**

Create `magic_dingus_box_cpp/scripts/tests/test_converge_system_units.bats`:

```bash
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd magic_dingus_box_cpp/scripts/tests && bats test_converge_system_units.bats`
Expected: every test FAILs (the script does not exist: `bash: .../converge_system_units.sh: No such file or directory`).

- [ ] **Step 3: Write the script**

Create `magic_dingus_box_cpp/scripts/converge_system_units.sh` (then `chmod 755` it):

```bash
#!/bin/bash
#
# Magic Dingus Box — converge the installed systemd units to the tree's.
#
# The tree ships its units as files (systemd/, magic_dingus_box_cpp/systemd/
# and the per-feature dirs under magic_dingus_box_cpp/scripts/). Until
# 2026-10 nothing copied them into /etc/systemd/system after provisioning,
# so every unit fix had to ride a hand-written drop-in from
# setup_memory_tuning.sh — and one of those (an empty OnBootSec=, which
# resets EVERY trigger) stopped the missing-search timer for 10 days on the
# owner's box. This script is the one place that installs the tree's units,
# decides what a change means on a live box (POLICY below), removes the
# drop-ins that used to carry unit fixes, and retires units the tree no
# longer ships, which is what makes a rollback complete. Design:
# docs/superpowers/specs/2026-10-14-converge-system-units-design.md
#
# Callers (as root): setup_memory_tuning.sh step 1a — the hook every
# delivery path runs (deploy, OTA including the OLD update.sh on the first
# OTA that ships a change, first boot, source-box sync) — and update.sh's
# rollbacks (converge_units_after_restore). Idempotent: a converged box
# gets no writes and no systemctl changes. Never restarts the kiosk.
#
#   converge_system_units.sh              converge the tree this script is in
#   converge_system_units.sh --tree DIR   converge DIR's units (a rollback to
#                                         a tree that predates this script)
#   converge_system_units.sh --check      read-only, no root: one line per
#                                         finding (DRIFT|MISSING|STALE <unit>,
#                                         DROPIN <path>). verify_box.sh runs it.
#
# Exit: 0 converged (--check: clean), 1 --check findings or bad usage,
# 2 something could not be converged or the tree was refused (logged;
# callers warn and carry on).
#
# Test seams (same conventions as setup_memory_tuning.sh):
#   MAGIC_TUNING_ROOT    - prefix for /etc, /var/lib and /run/lock (BATS fake
#                          root; also skips the EUID root check)
#   MAGIC_SKIP_SYSTEMCTL - "true" skips every systemctl call

set -uo pipefail

# One row per in-tree unit. tests/local/converge_units_table.bats fails when
# a unit has no row or a row has no file, so a new unit cannot ship without
# someone choosing its policy. Columns:
#   gate       base: every box. mb: install, enable, start and re-arm only
#              when <tree>/services/.env exists (an installed mb unit is
#              still kept current without it; refreshing a file starts
#              nothing, and every mb unit has ConditionPathExists=.env).
#              ignore: legacy, never touched.
#   if-absent  skip | install (file only) | enable | enable-now. Applied
#              ONLY to a unit that is not installed. An installed unit's
#              enablement is never changed (disabled stays disabled).
#              skip wherever the unit needs something the tree does not
#              carry: a /usr/local/bin helper only setup_services.sh
#              creates, dnsmasq config, harness wiring.
#   on-change  none: applies at its next start (boot, OTA, deploy).
#              try-restart: restart if running, when its unit changed.
#              rearm: a timer that is enabled but 'elapsed' (no future
#              run; daemon-reload never re-arms one) is restarted.
#   drop-in    a <unit>.d/ file the tree unit makes redundant: removed once
#              the unit matches the tree, if it carries our marker. Or -.
POLICY='
magic-dingus-box-cpp.service            base   enable      none         stop-timeout.conf
magic-dingus-web.service                base   enable      none         -
magic-dingus-audio.service              base   enable      none         -
magic-dingus-ota-recovery.service       base   enable      none         -
usb-gadget-network.service              base   enable      none         -
magic-first-boot.service                base   install     none         -
magic-cpu-performance.service           base   skip        none         -
wifi-powersave-off.service              base   skip        none         -
content-manager-redirect.service        base   skip        try-restart  -
kiosk-standby-watcher.service           base   skip        try-restart  -
led-boot-sequence.service               base   skip        none         -
led-shutdown-animation.service          base   skip        none         -
power-switch-check.service              base   skip        none         -
magic-dingus-services.service           mb     enable      none         -
gluetun-cascade-restart.service         mb     skip        try-restart  -
magic-dingus-clear-cooldowns.service    mb     skip        none         -
magic-dingus-sync-qbit-password.service mb     skip        none         -
qbit-port-sync.service                  mb     skip        none         -
qbit-port-sync.timer                    mb     skip        rearm        -
magic-dingus-auto-blocklist.service     mb     skip        none         -
magic-dingus-auto-blocklist.timer       mb     skip        rearm        -
magic-dingus-storage-attach.service     mb     enable      none         mdb-timeout.conf
magic-dingus-library-import.service     mb     enable      none         -
magic-dingus-smoke-test.service         mb     install     none         mdb-timeout.conf
magic-dingus-smoke-test.timer           mb     enable-now  rearm        -
magic-dingus-missing-search.service     mb     install     none         -
magic-dingus-missing-search.timer       mb     enable-now  rearm        mdb-boot-delay.conf
magic-dingus-episode-priority.service   mb     install     none         -
magic-dingus-episode-priority.timer     mb     enable-now  rearm        -
magic-mpv.service                       ignore -           -            -
magic-mpv.hardened.service              ignore -           -            -
magic-mpv-x11.service                   ignore -           -            -
magic-ui.service                        ignore -           -            -
magic-ui.hardened.service               ignore -           -            -
magic-ui-x11.service                    ignore -           -            -
'

# Never restarted, stopped or retired here, whatever the table says: the
# kiosk (an OTA or a deploy restarts it), the web admin (it runs the OTA,
# which restarts it last) and the OTA power-cut recovery (it may be the
# unit running this script).
PROTECTED="magic-dingus-box-cpp.service magic-dingus-web.service magic-dingus-ota-recovery.service"
UNIT_DIRS="systemd magic_dingus_box_cpp/systemd magic_dingus_box_cpp/scripts/missing_search magic_dingus_box_cpp/scripts/episode_priority"
# The comment line every drop-in setup_memory_tuning.sh wrote carries.
DROPIN_MARKER="(setup_memory_tuning.sh)"
UNIT_NAME_RE='^[A-Za-z0-9@._-]+\.(service|timer)$'

log() { echo "[units] $1"; }

policy_names() { printf '%s\n' "$POLICY" | awk 'NF { print $1 }'; }

# unit_policy <name> -> "<gate> <if-absent> <on-change> <drop-in>". A unit
# with no row (only possible when converging an older tree) is kept current
# but never installed or restarted.
unit_policy() {
    local row
    row="$(printf '%s\n' "$POLICY" | awk -v u="$1" '$1 == u { print $2, $3, $4, $5; exit }')"
    echo "${row:-base skip none -}"
}

# tree_units -> "<name><TAB><path>" for every unit file in the tree, by name.
tree_units() {
    local d f
    for d in $UNIT_DIRS; do
        for f in "$TREE/$d"/*.service "$TREE/$d"/*.timer; do
            [[ -f "$f" ]] && printf '%s\t%s\n' "$(basename "$f")" "$f"
        done
    done | sort
}

is_protected() { case " $PROTECTED " in *" $1 "*) return 0 ;; esac; return 1; }

in_list() { printf '%s' "$1" | grep -qxF -- "$2"; }

# write_unit <src> <dest>: temp file in the same directory, then rename, so
# systemd never reads a half-written unit (the temp name is not a unit name).
write_unit() {
    local tmp
    tmp="$(dirname "$2")/.$(basename "$2").mdb-tmp"
    if mkdir -p "$(dirname "$2")" && install -m 0644 "$1" "$tmp" && mv -f "$tmp" "$2"; then
        return 0
    fi
    rm -f "$tmp"
    return 1
}

# Steps 1-2: compare every tree unit with its installed copy; write the
# changed ones and the absent ones the policy installs. --check reports.
converge_files() {
    local name src gate absent dest
    while IFS=$'\t' read -r name src; do
        [[ -n "$name" ]] || continue
        if in_list "$DUPS" "$name"; then
            log "ERROR: $name ships in two tree directories; not converged"
            FAILURES=$((FAILURES + 1))
            continue
        fi
        read -r gate absent _ <<< "$(unit_policy "$name")"
        [[ "$gate" == ignore ]] && continue
        dest="$SD/$name"
        if [[ -L "$dest" ]]; then
            [[ "$CHECK" == true ]] || log "skip $name: $dest is a symlink (masked or linked), never written"
            continue
        fi
        if [[ -e "$dest" ]]; then
            if [[ -f "$dest" ]] && cmp -s "$src" "$dest"; then
                continue
            fi
            if [[ "$CHECK" == true ]]; then
                echo "DRIFT $name"; FINDINGS=$((FINDINGS + 1)); continue
            fi
            if [[ ! -f "$dest" ]]; then
                log "ERROR: $dest is not a regular file; not converged"
                FAILURES=$((FAILURES + 1))
            elif write_unit "$src" "$dest"; then
                log "updated $name to the tree's copy"
                WRITTEN="${WRITTEN}${name}"$'\n'; RELOAD=true
            else
                log "ERROR: could not write $dest"
                FAILURES=$((FAILURES + 1))
            fi
        else
            [[ "$absent" == skip ]] && continue
            [[ "$gate" == mb && "$MB" != true ]] && continue
            if [[ "$CHECK" == true ]]; then
                echo "MISSING $name"; FINDINGS=$((FINDINGS + 1)); continue
            fi
            if write_unit "$src" "$dest"; then
                log "installed $name ($absent)"
                INSTALLED="${INSTALLED}${name}"$'\n'; RELOAD=true
            else
                log "ERROR: could not install $dest"
                FAILURES=$((FAILURES + 1))
            fi
        fi
    done <<< "$LIST"
}

# Step 7: remember which tree units are installed, so a later run (the
# restored tree's, after a rollback) can retire one its tree does not ship.
# Losing this file only pauses retirement until the next run.
write_state() {
    local name gate
    [[ "$CHECK" == true ]] && return 0
    if ! mkdir -p "$(dirname "$STATE")"; then
        log "WARNING: cannot create $(dirname "$STATE"); retirement waits for the next run"
        return 0
    fi
    while IFS=$'\t' read -r name _; do
        [[ -n "$name" ]] || continue
        read -r gate _ <<< "$(unit_policy "$name")"
        [[ "$gate" == ignore ]] && continue
        if [[ -f "$SD/$name" && ! -L "$SD/$name" ]]; then
            echo "$name"
        fi
    done <<< "$LIST" > "${STATE}.tmp" && mv -f "${STATE}.tmp" "$STATE" \
        || log "WARNING: could not write $STATE; retirement waits for the next run"
}

main() {
    CHECK=false
    TREE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --check) CHECK=true ;;
            --tree)
                if [[ -z "${2:-}" || ! -d "${2:-}" ]]; then
                    echo "[units] ERROR: --tree needs an existing directory" >&2
                    exit 1
                fi
                TREE="$(cd "$2" && pwd)"
                shift ;;
            *) echo "usage: $(basename "$0") [--check] [--tree DIR]" >&2; exit 1 ;;
        esac
        shift
    done

    local root="${MAGIC_TUNING_ROOT:-}"
    SKIP_SYSTEMCTL="${MAGIC_SKIP_SYSTEMCTL:-false}"
    SD="${root}/etc/systemd/system"
    STATE="${root}/var/lib/magic-dingus/system_units"
    if [[ "$CHECK" != true && -z "$root" && "$EUID" -ne 0 ]]; then
        echo "[units] ERROR: must run as root (sudo), or use --check" >&2
        exit 1
    fi

    LIST="$(tree_units)"
    # A wrong or half-restored tree must never look like "the tree ships
    # nothing": that would retire every installed unit.
    if ! printf '%s\n' "$LIST" | cut -f1 | grep -qxF magic-dingus-box-cpp.service; then
        echo "[units] ERROR: $TREE has no magic-dingus-box-cpp.service; not a Magic Dingus Box tree, nothing changed" >&2
        exit 2
    fi
    DUPS="$(printf '%s\n' "$LIST" | cut -f1 | uniq -d)"

    # Serialize writers (deploy and an OTA hook can overlap on a dev box).
    # macOS (the test suites) has no flock; the box always does.
    if [[ "$CHECK" != true ]] && command -v flock >/dev/null 2>&1; then
        mkdir -p "${root}/run/lock"
        exec 9> "${root}/run/lock/mdb-units.lock"
        flock -w 60 9 || log "WARNING: another converge held the lock for 60 s; continuing"
    fi

    MB=false
    [[ -f "$TREE/services/.env" ]] && MB=true
    FINDINGS=0 FAILURES=0 RELOAD=false WRITTEN="" INSTALLED=""

    converge_files
    write_state

    if [[ "$CHECK" == true ]]; then
        [[ "$FAILURES" -eq 0 ]] || exit 2
        [[ "$FINDINGS" -eq 0 ]] || exit 1
        exit 0
    fi
    log "done: $(printf '%s' "$WRITTEN" | grep -c .) updated, $(printf '%s' "$INSTALLED" | grep -c .) installed, ${FAILURES} failed"
    [[ "$FAILURES" -eq 0 ]] || exit 2
    exit 0
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
```

Note: the `--check` expected order in the test (`MISSING magic-first-boot…` before `DRIFT usb-gadget…`) is the sorted unit-name order `tree_units` produces.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `chmod 755 magic_dingus_box_cpp/scripts/converge_system_units.sh && cd magic_dingus_box_cpp/scripts/tests && bats test_converge_system_units.bats`
Expected: 18 tests, 0 failures.

- [ ] **Step 5: shellcheck**

Run: `shellcheck -S warning magic_dingus_box_cpp/scripts/converge_system_units.sh`
Expected: no output, exit 0.

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box_cpp/scripts/converge_system_units.sh magic_dingus_box_cpp/scripts/tests/test_converge_system_units.bats
git commit -m "feat(units): converge_system_units.sh — install the tree's unit files

Compares every in-tree unit with /etc/systemd/system, rewrites drifted
units by same-directory rename, installs absent ones per a policy table
(MB units only with services/.env), never writes a masked unit, records
the installed set for retirement, and has a read-only --check. Not yet
wired into any delivery path.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Retirement and drop-in cleanup

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/converge_system_units.sh` (add two functions before `main`; call them from `main`)
- Modify: `magic_dingus_box_cpp/scripts/tests/test_converge_system_units.bats` (append)

**Interfaces:**
- Consumes: globals and helpers from Task 1 (`LIST`, `SD`, `STATE`, `CHECK`, `FINDINGS`, `FAILURES`, `RELOAD`, `UNIT_NAME_RE`, `DROPIN_MARKER`, `unit_policy`, `is_protected`).
- Produces: `retire_stale_units`, `retire_redundant_dropins`; `sc ARGS…` (systemctl wrapper honoring `SKIP_SYSTEMCTL`); `--check` lines `STALE <unit>` and `DROPIN <path>`; test helpers `stub_systemctl`, `mutations`, `$CALLS`, `$STUB_DIR`.

- [ ] **Step 1: Write the failing tests**

Append to `magic_dingus_box_cpp/scripts/tests/test_converge_system_units.bats`:

```bash
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
```

- [ ] **Step 2: Run the new tests to verify they fail**

Run: `cd magic_dingus_box_cpp/scripts/tests && bats test_converge_system_units.bats -f 'retired|retire|machinery|garbage|stale|STALE|drop-in|DROPIN'`
Expected: the retirement tests FAIL (unit still present, no `disable` call); the drop-in removal tests FAIL (drop-in still present). "never retired", "garbage", "masked stale", "without our marker", "stays while" may pass already (nothing is removed yet) — that is fine.

- [ ] **Step 3: Implement**

In `converge_system_units.sh`, add after `in_list()`:

```bash
sc() { [[ "$SKIP_SYSTEMCTL" == "true" ]] && return 0; systemctl "$@"; }
```

Add before `main()`:

```bash
# Step 3: a unit an earlier run saw installed that this tree does not ship —
# a rollback to a release without it, or a release that dropped it — is
# disabled, stopped and removed. Only well-formed names from the state file,
# never the update machinery (PROTECTED), never a symlink.
retire_stale_units() {
    local name dest
    [[ -f "$STATE" ]] || return 0
    while IFS= read -r name; do
        [[ "$name" =~ $UNIT_NAME_RE ]] || continue
        printf '%s\n' "$LIST" | cut -f1 | grep -qxF -- "$name" && continue
        is_protected "$name" && continue
        dest="$SD/$name"
        [[ -f "$dest" && ! -L "$dest" ]] || continue
        if [[ "$CHECK" == true ]]; then
            echo "STALE $name"; FINDINGS=$((FINDINGS + 1)); continue
        fi
        sc disable --now --no-block "$name" >/dev/null 2>&1 \
            || log "WARNING: could not disable $name"
        if rm -f "$dest"; then
            log "retired $name (no longer in the tree)"; RELOAD=true
        else
            log "ERROR: could not remove $dest"; FAILURES=$((FAILURES + 1))
        fi
    done < "$STATE"
}

# Step 4: the drop-ins setup_memory_tuning.sh wrote while OTA could not
# replace units. Removed only once the unit matches the tree (a unit whose
# write failed keeps its fix) and only if the file carries our marker.
retire_redundant_dropins() {
    local name src dropin path
    while IFS=$'\t' read -r name src; do
        [[ -n "$name" ]] || continue
        read -r _ _ _ dropin <<< "$(unit_policy "$name")"
        [[ "$dropin" != "-" ]] || continue
        path="$SD/$name.d/$dropin"
        [[ -f "$path" ]] || continue
        if ! { [[ -f "$SD/$name" && ! -L "$SD/$name" ]] && cmp -s "$src" "$SD/$name"; }; then
            continue
        fi
        grep -qF -- "$DROPIN_MARKER" "$path" || continue
        if [[ "$CHECK" == true ]]; then
            echo "DROPIN $path"; FINDINGS=$((FINDINGS + 1)); continue
        fi
        if rm -f "$path"; then
            rmdir "$SD/$name.d" 2>/dev/null || true
            log "removed redundant drop-in $name.d/$dropin (the unit carries it now)"; RELOAD=true
        else
            log "ERROR: could not remove $path"; FAILURES=$((FAILURES + 1))
        fi
    done <<< "$LIST"
}
```

In `main`, replace

```bash
    converge_files
    write_state
```

with

```bash
    converge_files
    retire_stale_units
    retire_redundant_dropins
    write_state
```

- [ ] **Step 4: Run the whole suite**

Run: `cd magic_dingus_box_cpp/scripts/tests && bats test_converge_system_units.bats && shellcheck -S warning ../converge_system_units.sh`
Expected: 28 tests, 0 failures; shellcheck silent.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/scripts/converge_system_units.sh magic_dingus_box_cpp/scripts/tests/test_converge_system_units.bats
git commit -m "feat(units): retire units the tree stopped shipping and redundant fix drop-ins

Retirement reads only well-formed names from the state file and never
touches the kiosk, the web admin or the OTA recovery unit. A fix drop-in
goes only once its unit matches the tree and only if it carries the
setup_memory_tuning.sh marker. --check reports STALE and DROPIN.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Apply phase — daemon-reload, enable, try-restart, re-arm

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/converge_system_units.sh` (add `apply_policies` before `main`; call it)
- Modify: `magic_dingus_box_cpp/scripts/tests/test_converge_system_units.bats` (append)

**Interfaces:**
- Consumes: `LIST`, `WRITTEN`, `INSTALLED`, `RELOAD`, `MB`, `SKIP_SYSTEMCTL`, `FAILURES`, `unit_policy`, `is_protected`, `in_list`, test helpers `stub_systemctl`, `mutations`, `$CALLS`, `$STUB_DIR` (Task 2).
- Produces: `apply_policies`.

- [ ] **Step 1: Write the failing tests**

Append:

```bash
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
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cd magic_dingus_box_cpp/scripts/tests && bats test_converge_system_units.bats`
Expected: the new reload / enable / try-restart / re-arm / `--no-block` count tests FAIL (no `daemon-reload`, `enable`, `try-restart` or `restart` calls are made yet); earlier tests still pass.

- [ ] **Step 3: Implement**

Add before `main()`:

```bash
# Steps 5-6: one daemon-reload, then each policy's effect. Every start, stop
# and restart is --no-block: this also runs at boot (first boot, the OTA
# power-cut recovery), where waiting on a job can deadlock the boot.
apply_policies() {
    local name gate absent change
    if [[ "$SKIP_SYSTEMCTL" == "true" ]]; then
        log "SKIP: daemon-reload and unit policies (test mode)"
        return 0
    fi
    if [[ "$RELOAD" == true ]] && ! systemctl daemon-reload; then
        log "ERROR: daemon-reload failed"; FAILURES=$((FAILURES + 1))
    fi
    while IFS=$'\t' read -r name _; do
        [[ -n "$name" ]] || continue
        read -r gate absent change _ <<< "$(unit_policy "$name")"
        [[ "$gate" == ignore ]] && continue
        if in_list "$INSTALLED" "$name"; then
            case "$absent" in
                enable)
                    if systemctl enable "$name" >/dev/null 2>&1; then log "enabled $name"
                    else log "ERROR: could not enable $name"; FAILURES=$((FAILURES + 1)); fi ;;
                enable-now)
                    if systemctl enable --now --no-block "$name" >/dev/null 2>&1; then log "enabled and started $name"
                    else log "ERROR: could not enable $name"; FAILURES=$((FAILURES + 1)); fi ;;
            esac
            continue
        fi
        is_protected "$name" && continue
        [[ "$gate" == mb && "$MB" != true ]] && continue
        [[ -f "$SD/$name" && ! -L "$SD/$name" ]] || continue
        case "$change" in
            try-restart)
                in_list "$WRITTEN" "$name" || continue
                if systemctl --no-block try-restart "$name"; then log "try-restarted $name (its unit changed)"
                else log "WARNING: could not restart $name (the new unit applies at its next start)"; fi ;;
            rearm)
                systemctl is-enabled --quiet "$name" 2>/dev/null || continue
                [[ "$(systemctl show -p SubState --value "$name" 2>/dev/null)" == elapsed ]] || continue
                if systemctl --no-block restart "$name"; then log "re-armed $name (was elapsed)"
                else log "WARNING: could not re-arm $name (re-arms at the next boot)"; fi ;;
        esac
    done <<< "$LIST"
}
```

In `main`, replace

```bash
    retire_redundant_dropins
    write_state
```

with

```bash
    retire_redundant_dropins
    [[ "$CHECK" == true ]] || apply_policies
    write_state
```

- [ ] **Step 4: Run the suite**

Run: `cd magic_dingus_box_cpp/scripts/tests && bats test_converge_system_units.bats && shellcheck -S warning ../converge_system_units.sh`
Expected: 40 tests, 0 failures; shellcheck silent.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/scripts/converge_system_units.sh magic_dingus_box_cpp/scripts/tests/test_converge_system_units.bats
git commit -m "feat(units): apply each unit's policy after one daemon-reload

Fresh installs are enabled per policy (timers armed --now), changed
long-running units with no user-visible state are try-restarted, and an
enabled timer left 'elapsed' is re-armed. The kiosk, web admin and OTA
recovery are never restarted; every start/stop/restart is --no-block.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Policy-table invariants

**Files:**
- Create: `tests/local/converge_units_table.bats`

**Interfaces:**
- Consumes: sourcing `converge_system_units.sh` (main guarded) → `POLICY`, `PROTECTED`, `UNIT_DIRS`, `policy_names`, `unit_policy`, `tree_units` (needs `TREE`).

- [ ] **Step 1: Write the tests**

```bash
#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# converge_system_units.sh's POLICY table is the one place a unit's
# delivery is decided. Every unit the tree ships needs a row (no unit
# ships without someone choosing its policy) and every row a unit. Origin:
# missing_search/'s units lived outside systemd/, the only dir the old
# install blocks read, and were written + documented but never installed.

setup() {
    # shellcheck disable=SC1091
    source "$CPP_DIR/scripts/converge_system_units.sh"
    set +u +o pipefail
    TREE="$TESTS_REPO_ROOT"
}

@test "every unit file in the repo lives in a converged directory" {
    while IFS= read -r f; do
        d="$(dirname "$f")"
        [[ " $UNIT_DIRS " == *" $d "* ]] || { echo "unit outside UNIT_DIRS: $f"; return 1; }
    done < <(cd "$TESTS_REPO_ROOT" && git ls-files '*.service' '*.timer')
}

@test "every in-tree unit has a policy row" {
    while IFS=$'\t' read -r name _; do
        policy_names | grep -qxF "$name" || { echo "no POLICY row: $name"; return 1; }
    done < <(tree_units)
}

@test "every policy row names exactly one in-tree unit file" {
    while IFS= read -r name; do
        [ "$(tree_units | cut -f1 | grep -cxF "$name")" -eq 1 ] || { echo "row without exactly one file: $name"; return 1; }
    done < <(policy_names)
}

@test "rows are well-formed" {
    while read -r name gate absent change dropin extra; do
        [ -n "$name" ] || continue
        [ -z "$extra" ] || { echo "too many columns: $name"; return 1; }
        case "$gate" in
            ignore) [ "$absent $change $dropin" = "- - -" ] || { echo "ignore row with policy: $name"; return 1; } ;;
            base|mb)
                [[ "$absent" =~ ^(skip|install|enable|enable-now)$ ]] || { echo "bad if-absent: $name"; return 1; }
                [[ "$change" =~ ^(none|try-restart|rearm)$ ]] || { echo "bad on-change: $name"; return 1; }
                [[ "$dropin" == "-" || "$dropin" == *.conf ]] || { echo "bad drop-in: $name"; return 1; } ;;
            *) echo "bad gate: $name"; return 1 ;;
        esac
    done <<< "$POLICY"
}

@test "only the six legacy units are ignored" {
    [ "$(printf '%s\n' "$POLICY" | awk '$2 == "ignore" { print $1 }' | sort)" = "$(printf '%s\n' \
        magic-mpv-x11.service magic-mpv.hardened.service magic-mpv.service \
        magic-ui-x11.service magic-ui.hardened.service magic-ui.service | sort)" ]
}

@test "every timer re-arms and only timers re-arm" {
    while read -r name _ _ change _; do
        [ -n "$name" ] || continue
        case "$name" in
            *.timer)   [ "$change" = rearm ] || { echo "timer not rearm: $name"; return 1; } ;;
            *)         [ "$change" != rearm ] || { echo "non-timer rearm: $name"; return 1; } ;;
        esac
    done <<< "$POLICY"
}

@test "the update machinery never restarts on change" {
    for u in $PROTECTED; do
        read -r _ _ change _ <<< "$(unit_policy "$u")"
        [ "$change" = none ] || { echo "$u: $change"; return 1; }
    done
}

@test "a unit run by a /usr/local/bin helper is never installed when absent" {
    # Only setup_services.sh creates those helpers (update.sh refreshes but
    # never creates them): installing the unit alone arms a unit that fails.
    while IFS=$'\t' read -r name path; do
        read -r gate absent _ <<< "$(unit_policy "$name")"
        [ "$gate" = ignore ] && continue
        if grep -qE '^ExecStart=(-)?/usr/local/bin/' "$path"; then
            [ "$absent" = skip ] || { echo "$name installs without its helper"; return 1; }
        fi
    done < <(tree_units)
}

@test "each retired drop-in's setting is carried by its tree unit" {
    u="$CPP_DIR/systemd"
    grep -qx "TimeoutStopSec=20" "$u/magic-dingus-box-cpp.service"
    grep -qx "TimeoutStartSec=600" "$u/magic-dingus-storage-attach.service"
    grep -qx "TimeoutStartSec=300" "$u/magic-dingus-smoke-test.service"
    t="$CPP_DIR/scripts/missing_search/magic-dingus-missing-search.timer"
    grep -qx "OnBootSec=11min" "$t"
    grep -qx "OnUnitActiveSec=4h" "$t"
    [ "$(printf '%s\n' "$POLICY" | awk '$5 != "-" && NF { print $1 "/" $5 }' | sort)" = "$(printf '%s\n' \
        magic-dingus-box-cpp.service/stop-timeout.conf \
        magic-dingus-missing-search.timer/mdb-boot-delay.conf \
        magic-dingus-smoke-test.service/mdb-timeout.conf \
        magic-dingus-storage-attach.service/mdb-timeout.conf | sort)" ]
}

@test "every delivery path reaches the converge through setup_memory_tuning.sh" {
    grep -q 'converge_system_units.sh' "$CPP_DIR/scripts/setup_memory_tuning.sh"
    grep -q 'scripts/setup_memory_tuning.sh' "$CPP_DIR/scripts/update.sh"
    grep -q 'scripts/setup_memory_tuning.sh' "$CPP_DIR/scripts/deploy_cpp.sh"
    grep -q 'scripts/setup_memory_tuning.sh' "$TESTS_REPO_ROOT/scripts/golden_image/first_boot.sh"
    grep -q 'scripts/setup_memory_tuning.sh' "$TESTS_REPO_ROOT/scripts/golden_image/sync_source_box.sh"
}
```

- [ ] **Step 2: Run**

Run: `bats tests/local/converge_units_table.bats`
Expected: all PASS except "every delivery path reaches the converge…" (FAILS until Task 5 wires `setup_memory_tuning.sh`). If any other test fails, the POLICY table in Task 1 disagrees with the tree: fix the table, not the test.

- [ ] **Step 3: Commit**

```bash
git add tests/local/converge_units_table.bats
git commit -m "test(units): pin the converge policy table to the tree's units

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Wire the converge into `setup_memory_tuning.sh`; stop writing fix drop-ins

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/setup_memory_tuning.sh` (header lines 15-20; sections 1b + 1c, lines ~95-150; step 5's missing-search re-arm, lines ~252-265)
- Modify: `magic_dingus_box_cpp/scripts/tests/test_memory_tuning.bats` (replace the drop-in and re-arm tests)

**Interfaces:**
- Consumes: `converge_system_units.sh` CLI (Task 1-3) with `MAGIC_TUNING_ROOT` / `MAGIC_SKIP_SYSTEMCTL` inherited from the environment.

- [ ] **Step 1: Replace the obsolete tests with failing new ones**

In `test_memory_tuning.bats` delete these tests: "installs the service timing drop-ins (storage-attach, smoke-test, missing-search)", "missing-search drop-in keeps the 4 h repeat (an empty OnBootSec= resets EVERY trigger)", the `reload_stubs()` helper with "an elapsed missing-search timer is re-armed after the reload, without blocking" and "a waiting missing-search timer is never restarted" (now covered by `test_converge_system_units.bats`), "timing drop-ins agree with the in-tree units" (now in `tests/local/converge_units_table.bats`), and "installs kiosk TimeoutStopSec=20 drop-in, idempotently". Keep `effective_timer_triggers`. Add, after "kiosk drop-in carries OOMScoreAdjust=-500…":

```bash
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
    # The converge is run by path, not via PATH, so a failing stub cannot be
    # injected; pin the guard instead (the converge's own suite covers exit 2).
    grep -A3 'converge_system_units.sh"$' "$TUNING_SCRIPT" | grep -q '|| log "WARNING'
}
```

- [ ] **Step 2: Run to verify the new tests fail**

Run: `cd magic_dingus_box_cpp/scripts/tests && bats test_memory_tuning.bats`
Expected: "no longer writes the unit-fix drop-ins", "a field box…", "the unit converge runs first…" and "a failing unit converge…" FAIL (the script still writes the drop-ins and never calls the converge).

- [ ] **Step 3: Edit `setup_memory_tuning.sh`**

Header — replace the lines

```
#   1b. /etc/systemd/system/magic-dingus-box-cpp.service.d/stop-timeout.conf
#        -> TimeoutStopSec=20 so a mid-game stop lets RetroArch auto-save
#           (the OTA delivery path for a unit-file change; see step 1b)
#   1c. Media Browser unit timing drop-ins (storage-attach TimeoutStartSec,
#        smoke-test TimeoutStartSec, missing-search timer OnBootSec) — same
#        OTA delivery path as 1b
```

with

```
#   1a. the tree's systemd units, via converge_system_units.sh: installs
#        the units that differ from the tree, removes the drop-ins that used
#        to carry unit fixes (stop-timeout.conf + the three Media Browser
#        timing drop-ins, until 2026-10), re-arms elapsed timers. Unit fixes
#        ship as unit-file edits now, never as drop-ins written here.
```

and in the 1d-1f header lines change "same OTA delivery path as 1b" to "same delivery reasoning as 1a" (three occurrences).

Body — delete everything from the line `# --- 1b. kiosk stop timeout drop-in` through the line `log "service timing drop-ins installed (storage-attach, smoke-test, missing-search)"` (sections 1b and 1c), and put in their place:

```bash
# --- 1a. the tree's systemd units --------------------------------------------
# This script is the root-run hook every delivery path executes (deploy,
# OTA — including the OLD update.sh on the first OTA that ships a change —
# first boot, source-box sync), so it is where the installed units converge
# to the tree's. Until 2026-10 OTA never installed unit files and unit fixes
# rode drop-ins written here; an empty OnBootSec= in one of them stopped
# the missing-search timer for 10 days. The converge removes those drop-ins
# once each unit matches the tree. Never fails this script.
CONVERGE_UNITS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/converge_system_units.sh"
if [[ -f "$CONVERGE_UNITS" ]]; then
    bash "$CONVERGE_UNITS" 2>&1 | sed 's/^/  /' \
        || log "WARNING: unit convergence reported failures (verify_box.sh names them)"
else
    log "converge_system_units.sh not found; skipping unit convergence"
fi
```

Step 5 — delete the block that starts with the comment `# A missing-search timer that went 'elapsed' under the old drop-in (no` and ends with the `fi` closing `if [[ "$(systemctl show -p SubState --value magic-dingus-missing-search.timer ...` (the converge re-arms every elapsed timer now). Leave the `daemon-reload` and `sysctl -p` lines.

- [ ] **Step 4: Run both suites**

Run: `cd magic_dingus_box_cpp/scripts/tests && bats test_memory_tuning.bats test_converge_system_units.bats && cd ../../.. && bats tests/local/converge_units_table.bats tests/local/audio_service_unit.bats && shellcheck -S warning magic_dingus_box_cpp/scripts/setup_memory_tuning.sh`
Expected: all PASS, shellcheck silent.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/scripts/setup_memory_tuning.sh magic_dingus_box_cpp/scripts/tests/test_memory_tuning.bats
git commit -m "feat(units): every delivery path converges units; no more fix drop-ins

setup_memory_tuning.sh (deploy, OTA incl. the old updater's first hop,
first boot, source-box sync) now runs converge_system_units.sh first and
no longer writes stop-timeout.conf or the three Media Browser timing
drop-ins; the converge removes them once each unit matches the tree. The
one-off missing-search re-arm is gone: the converge re-arms every elapsed
timer.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Rollback converges back to the restored tree (`update.sh`)

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/update.sh` (new functions after `retire_audio_service_if_absent`, ~line 1611; calls in `rollback_internal` ~2433-2500 and `rollback` ~2600-2640)
- Modify: `magic_dingus_box_cpp/scripts/tests/test_update.bats` (append after the audio rollback tests, ~line 1465)

**Interfaces:**
- Consumes: `INSTALL_DIR`, `SKIP_SYSTEMCTL`, `log`, `log_warn` (update.sh); `converge_system_units.sh [--tree DIR]`.
- Produces: global `CONVERGE_STASH`; `stash_converge_script` (call before the restore rsync); `converge_units_after_restore` (call after `refresh_out_of_tree_files`). Both always return 0.

- [ ] **Step 1: Write the failing tests**

Append to `test_update.bats` (after "audio rollback: both rollback paths call retire_audio_service_if_absent"):

```bash
# =============================================================================
# Rollback converges the systemd units back to the RESTORED tree
# =============================================================================

converge_fixture() {  # <path> — a converge script that records how it was run
    mkdir -p "$(dirname "$1")"
    printf '#!/bin/bash\necho "converge-ran $*"\n' > "$1"
}

converge_sudo_stub() {
    SUDO_LOG="$TEST_TEMP_DIR/sudo.log"
    : > "$SUDO_LOG"
    sudo() { echo "$*" >> "$SUDO_LOG"; echo "noise on stdout"; return "${SUDO_RC:-0}"; }
}

@test "units after restore: the restored tree's own converge script is preferred" {
    load_update_functions
    SKIP_SYSTEMCTL=false
    own="$MAGIC_BASE_PATH/magic_dingus_box_cpp/scripts/converge_system_units.sh"
    converge_fixture "$own"
    CONVERGE_STASH="$TEST_TEMP_DIR/stash"; converge_fixture "$CONVERGE_STASH"
    converge_sudo_stub
    run converge_units_after_restore
    [ "$status" -eq 0 ]
    [ "$(cat "$SUDO_LOG")" = "-n bash $own" ]
    [ ! -e "$TEST_TEMP_DIR/stash" ]
}

@test "units after restore: a tree without the script is converged by the stash, with --tree" {
    load_update_functions
    SKIP_SYSTEMCTL=false
    CONVERGE_STASH="$TEST_TEMP_DIR/stash"; converge_fixture "$CONVERGE_STASH"
    converge_sudo_stub
    run converge_units_after_restore
    [ "$status" -eq 0 ]
    [ "$(cat "$SUDO_LOG")" = "-n bash $TEST_TEMP_DIR/stash --tree $MAGIC_BASE_PATH" ]
}

@test "units after restore: no script in either tree warns and carries on" {
    load_update_functions
    SKIP_SYSTEMCTL=false
    CONVERGE_STASH=""
    converge_sudo_stub
    run converge_units_after_restore
    [ "$status" -eq 0 ]
    [ ! -s "$SUDO_LOG" ]
    [[ "$output" == *"installed units left as they are"* ]]
}

@test "units after restore: a failing converge never fails the rollback, and stdout stays JSON-clean" {
    load_update_functions
    SKIP_SYSTEMCTL=false
    converge_fixture "$MAGIC_BASE_PATH/magic_dingus_box_cpp/scripts/converge_system_units.sh"
    converge_sudo_stub
    SUDO_RC=2
    stdout="$(converge_units_after_restore 2>/dev/null)"
    [ -z "$stdout" ]
    run converge_units_after_restore
    [ "$status" -eq 0 ]
}

@test "units after restore: test mode skips it" {
    load_update_functions
    converge_fixture "$MAGIC_BASE_PATH/magic_dingus_box_cpp/scripts/converge_system_units.sh"
    converge_sudo_stub
    run converge_units_after_restore
    [ "$status" -eq 0 ]
    [ ! -s "$SUDO_LOG" ]
}

@test "both rollback paths stash the rolled-back-from script BEFORE the restore and converge after it" {
    seed_audio_rollback with
    converge_fixture "$MAGIC_BASE_PATH/magic_dingus_box_cpp/scripts/converge_system_units.sh"
    echo "# the rolled-back-from release" >> "$MAGIC_BASE_PATH/magic_dingus_box_cpp/scripts/converge_system_units.sh"
    load_update_functions
    converge_units_after_restore() {
        { [ -n "$CONVERGE_STASH" ] && cat "$CONVERGE_STASH"
          echo "restored-has-script=$([ -f "$INSTALL_DIR/magic_dingus_box_cpp/scripts/converge_system_units.sh" ] && echo yes || echo no)"; } \
            >> "$TEST_TEMP_DIR/converge.log"
        [ -n "$CONVERGE_STASH" ] && rm -f "$CONVERGE_STASH"
        CONVERGE_STASH=""
    }
    rollback_internal 2>/dev/null
    rollback >/dev/null 2>&1
    [ "$(grep -c 'the rolled-back-from release' "$TEST_TEMP_DIR/converge.log")" -eq 1 ]
    [ "$(grep -c '^restored-has-script=no$' "$TEST_TEMP_DIR/converge.log")" -eq 2 ]
}
```

Notes for the implementer: `seed_audio_rollback with` copies the install tree to the backup BEFORE the test adds `converge_system_units.sh` to the install tree, so the backup (the restored tree) has no converge script and the install tree (the rolled-back-from tree) does — the first rollback stashes it; the second rollback (`rollback`) runs against an install tree that is already the restored one, so its stash is empty (nothing to print — hence 1, not 2, "rolled-back-from" lines). Check `seed_installed_tree`'s layout if a path differs.

- [ ] **Step 2: Run to verify they fail**

Run: `cd magic_dingus_box_cpp/scripts/tests && bats test_update.bats -f 'units after restore|stash the rolled-back-from'`
Expected: FAIL with `converge_units_after_restore: command not found` (and the stash test fails: no stash is made).

- [ ] **Step 3: Implement in `update.sh`**

After the closing `}` of `retire_audio_service_if_absent` add:

```bash
# Rollback: converge the installed systemd units back to the RESTORED tree.
#
# An install converges /etc/systemd/system to the NEW tree's units
# (setup_memory_tuning.sh -> converge_system_units.sh, before the verified
# start). A rollback must undo that, or the box runs the rolled-back
# release's units against the restored tree. The restored tree's own
# script is preferred (its units, its policy table). A tree that predates
# the script is converged by a copy of the rolled-back-from tree's script,
# stashed BEFORE the restore rsync deletes it and pointed at the restored
# tree with --tree. Units the rolled-back release added are retired from
# the converge's state file. Never fails the rollback; skipped in test mode
# like every root hook; prints to stderr (stdout carries the job's JSON).
# A rollback run by an updater older than this cannot converge back — see
# docs/superpowers/specs/2026-10-14-converge-system-units-design.md
# "Rollback" for why the release that ships it changes no unit file.
CONVERGE_STASH=""
stash_converge_script() {
    CONVERGE_STASH=""
    local src="${INSTALL_DIR}/magic_dingus_box_cpp/scripts/converge_system_units.sh"
    [ -f "$src" ] || return 0
    CONVERGE_STASH="$(mktemp "${TMPDIR:-/tmp}/mdb-converge.XXXXXX" 2>/dev/null)" || { CONVERGE_STASH=""; return 0; }
    cp "$src" "$CONVERGE_STASH" 2>/dev/null || { rm -f "$CONVERGE_STASH"; CONVERGE_STASH=""; }
    return 0
}

converge_units_after_restore() {
    local own="${INSTALL_DIR}/magic_dingus_box_cpp/scripts/converge_system_units.sh"
    if [ "$SKIP_SYSTEMCTL" = "true" ]; then
        log "SKIP: unit convergence after restore (test mode)"
    elif [ -f "$own" ]; then
        log "Converging systemd units to the restored tree..."
        sudo -n bash "$own" >&2 \
            || log_warn "unit convergence after restore reported failures (verify_box.sh names them)"
    elif [ -n "$CONVERGE_STASH" ] && [ -f "$CONVERGE_STASH" ]; then
        log "Converging systemd units to the restored tree (its release predates converge_system_units.sh)..."
        sudo -n bash "$CONVERGE_STASH" --tree "$INSTALL_DIR" >&2 \
            || log_warn "unit convergence after restore reported failures (verify_box.sh names them)"
    else
        log_warn "no converge_system_units.sh in either tree; installed units left as they are"
    fi
    if [ -n "$CONVERGE_STASH" ]; then
        rm -f "$CONVERGE_STASH"
    fi
    CONVERGE_STASH=""
    return 0
}
```

In `rollback_internal`: immediately before the line `normalize_tree_ownership` add `stash_converge_script`; immediately after the line `refresh_out_of_tree_files` (the one after the "Put the /usr/local/bin helpers + usb0 dnsmasq conf back" comment) add:

```bash

    # Units back to the restored tree's (see converge_units_after_restore).
    converge_units_after_restore
```

In `rollback`: immediately before its `normalize_tree_ownership` add `stash_converge_script`; in its rsync-failure branch (`if ! rsync_exit_ok "$rsync_exit"; then`) add `rm -f "$CONVERGE_STASH"; CONVERGE_STASH=""` as the first line; after its `refresh_out_of_tree_files` add:

```bash

    # See rollback_internal: units back to the restored tree's.
    converge_units_after_restore
```

- [ ] **Step 4: Run the update suite**

Run: `cd magic_dingus_box_cpp/scripts/tests && bats test_update.bats && shellcheck -S error ../update.sh`
Expected: all tests PASS (the new 6 included); shellcheck silent.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/scripts/update.sh magic_dingus_box_cpp/scripts/tests/test_update.bats
git commit -m "feat(ota): rollbacks converge systemd units back to the restored tree

All three rollback runners (failed install, user rollback, boot recovery)
run the restored tree's converge_system_units.sh, or a copy of the
rolled-back-from tree's, stashed before the restore, with --tree. Never
fails the rollback; output goes to stderr so the job JSON stays clean.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: `verify_box.sh` — "System units" section (FAIL on drift)

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/verify_box.sh` (insert after the repeating-timers block, i.e. after the line `(( ARMED )) && pass "${ARMED} repeating timer(s) armed (next run scheduled)"`, ~line 652)
- Create: `tests/local/verify_box_units.bats`

**Interfaces:**
- Consumes: `converge_system_units.sh --check` output lines and exit codes (Tasks 1-2); verify_box's `header`, `pass`, `fail`, `warn`, `BASE`.
- Produces: `unit_finding_verdict LINE` → `"<fail|warn><TAB><message>"`.

- [ ] **Step 1: Write the failing tests**

Create `tests/local/verify_box_units.bats`:

```bash
#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# verify_box.sh FAILs a box whose installed systemd units differ from its
# tree (converge_system_units.sh --check). Every delivery path converges
# them, so drift means the converge is failing on the box or /etc was
# edited by hand — the class that silenced missing-search for 10 days in
# 2026-10 while verify_box said SHIPPABLE. The verdict function and the
# section are extracted because verify_box.sh runs top-to-bottom on a Pi.

setup() {
    local def
    def=$(sed -n '/^unit_finding_verdict() {/,/^}/p' "$CPP_DIR/scripts/verify_box.sh")
    [ -n "$def" ] || { echo "unit_finding_verdict not found in verify_box.sh"; return 1; }
    eval "$def"
    BLOCK=$(sed -n '/^CONVERGE_UNITS=/,/^fi  # System units$/p' "$CPP_DIR/scripts/verify_box.sh")
    [ -n "$BLOCK" ] || { echo "System units block not found in verify_box.sh"; return 1; }
    BASE="$BATS_TEST_TMPDIR/box"
    mkdir -p "$BASE/magic_dingus_box_cpp/scripts"
}

verdict() { unit_finding_verdict "$1" | cut -f1; }

@test "DRIFT, MISSING and STALE fail; DROPIN warns; anything else warns" {
    [ "$(verdict 'DRIFT magic-dingus-missing-search.timer')" = fail ]
    [ "$(verdict 'MISSING magic-dingus-ota-recovery.service')" = fail ]
    [ "$(verdict 'STALE old-feature.timer')" = fail ]
    [ "$(verdict 'DROPIN /etc/systemd/system/x.service.d/mdb-timeout.conf')" = warn ]
    [ "$(verdict 'what is this')" = warn ]
}

@test "the message names the unit" {
    [[ "$(unit_finding_verdict 'DRIFT magic-dingus-missing-search.timer' | cut -f2)" == *magic-dingus-missing-search.timer* ]]
}

# run_section <converge stub body> — the section against a stub converge
run_section() {
    printf '#!/bin/bash\n%s\n' "$1" > "$BASE/magic_dingus_box_cpp/scripts/converge_system_units.sh"
    run bash -c '
        pass() { echo "[PASS] $1"; }
        fail() { echo "[FAIL] $1"; }
        warn() { echo "[WARN] $1"; }
        BASE="$2"
        eval "$(printf "%s\n" "$3")"
        eval "$1"
    ' _ "$BLOCK" "$BASE" "$(sed -n '/^unit_finding_verdict() {/,/^}/p' "$CPP_DIR/scripts/verify_box.sh")"
}

@test "a converged box passes" {
    run_section 'exit 0'
    [ "$output" = "[PASS] installed systemd units match the tree" ]
}

@test "each finding is its own FAIL/WARN line with the fix command" {
    run_section 'printf "DRIFT a.timer\nDROPIN /x/y.conf\n"; exit 1'
    [ "${#lines[@]}" -eq 2 ]
    [[ "${lines[0]}" == "[FAIL] a.timer"*"converge_system_units.sh"* ]]
    [[ "${lines[1]}" == "[WARN] /x/y.conf"* ]]
}

@test "a converge that cannot check (exit 2) is a WARN, not a pass" {
    run_section 'echo "[units] ERROR: not a Magic Dingus Box tree" >&2; exit 2'
    [[ "$output" == "[WARN] could not compare installed units"* ]]
}

@test "a tree without the script warns" {
    run bash -c 'warn() { echo "[WARN] $1"; }; pass() { :; }; fail() { :; }; BASE="$2"; eval "$1"' _ "$BLOCK" "$BASE"
    [[ "$output" == "[WARN] converge_system_units.sh is not in this tree"* ]]
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `bats tests/local/verify_box_units.bats`
Expected: FAIL in setup: `unit_finding_verdict not found in verify_box.sh`.

- [ ] **Step 3: Add the section to `verify_box.sh`**

Insert after `(( ARMED )) && pass "${ARMED} repeating timer(s) armed (next run scheduled)"`:

```bash

# ---------------------------------------------------------------------
# System units: the installed units must be the tree's. Every delivery
# path converges them (setup_memory_tuning.sh -> converge_system_units.sh,
# and every rollback), so a mismatch means the converge is failing on this
# box or /etc/systemd/system was edited by hand — the class that silenced
# missing-search for 10 days (2026-10) while this script said SHIPPABLE.
# Hence FAIL. A leftover fix drop-in on a converged unit only restates the
# unit's own value: WARN. Customizations belong in other drop-ins, which
# this never looks at.
# ---------------------------------------------------------------------
header "System units"
# Pure, pinned by tests/local/verify_box_units.bats: one --check line ->
# "<fail|warn><TAB><message>".
unit_finding_verdict() {
  local kind="${1%% *}" what="${1#* }"
  case "$kind" in
    DRIFT)   printf '%s\t%s\n' fail "${what}: the installed copy differs from the tree's" ;;
    MISSING) printf '%s\t%s\n' fail "${what}: not installed, but this box should have it" ;;
    STALE)   printf '%s\t%s\n' fail "${what}: still installed, but the tree no longer ships it" ;;
    DROPIN)  printf '%s\t%s\n' warn "${what}: leftover fix drop-in on a converged unit" ;;
    *)       printf '%s\t%s\n' warn "unexpected converge output: $1" ;;
  esac
}
CONVERGE_UNITS="${BASE}/magic_dingus_box_cpp/scripts/converge_system_units.sh"
if [[ ! -f "$CONVERGE_UNITS" ]]; then
  warn "converge_system_units.sh is not in this tree — installed units not compared"
else
  UNITS_OUT="$(bash "$CONVERGE_UNITS" --check 2>&1)"; UNITS_RC=$?
  if [[ $UNITS_RC -eq 0 ]]; then
    pass "installed systemd units match the tree"
  elif [[ $UNITS_RC -eq 1 ]]; then
    while IFS= read -r _line; do
      [[ -n "$_line" ]] || continue
      IFS=$'\t' read -r _verdict _msg <<<"$(unit_finding_verdict "$_line")"
      if [[ "$_verdict" == fail ]]; then
        fail "${_msg} (fix: sudo bash ${CONVERGE_UNITS})"
      else
        warn "${_msg} (fix: sudo bash ${CONVERGE_UNITS})"
      fi
    done <<<"$UNITS_OUT"
  else
    warn "could not compare installed units: $(printf '%s' "$UNITS_OUT" | tail -1)"
  fi
fi  # System units
```

- [ ] **Step 4: Run**

Run: `bats tests/local/verify_box_units.bats tests/local/verify_box_timers.bats tests/local/verify_box_error_lines.bats tests/local/verify_box_policy_override.bats && shellcheck -S error magic_dingus_box_cpp/scripts/verify_box.sh`
Expected: all PASS; shellcheck silent.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/scripts/verify_box.sh tests/local/verify_box_units.bats
git commit -m "feat(verify_box): FAIL a box whose installed units differ from its tree

Runs converge_system_units.sh --check: DRIFT, MISSING and STALE are FAIL
(the converge runs on every path, so drift means it is failing or /etc
was hand-edited), a leftover fix drop-in is a WARN. Each line names the
one fix command.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: `sync_source_box.sh` — image carries converged units

**Files:**
- Modify: `scripts/golden_image/sync_source_box.sh` (before the `push magic_dingus_box_cpp/scripts/setup_memory_tuning.sh` line ~144; after the memory-tuning run's `fi`, ~line 158)
- Modify: `tests/local/converge_units_table.bats` (append)

**Interfaces:**
- Consumes: `push SRC DEST MODE LABEL` (sync_source_box.sh), `SSH_OPTS`, `PI_HOST`, `REPO`, `FAILED`, `GREEN`/`RED`/`NC`; converge `--check`.

- [ ] **Step 1: Write the failing test**

Append to `tests/local/converge_units_table.bats`:

```bash
@test "sync_source_box pushes the converge script and the units, and gates the image on --check" {
    f="$TESTS_REPO_ROOT/scripts/golden_image/sync_source_box.sh"
    grep -q 'push magic_dingus_box_cpp/scripts/converge_system_units.sh' "$f"
    grep -q 'converge_system_units.sh --check' "$f"
    push_line="$(grep -n 'push magic_dingus_box_cpp/scripts/converge_system_units.sh' "$f" | cut -d: -f1)"
    run_line="$(grep -n '"sudo /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/setup_memory_tuning.sh"' "$f" | head -1 | cut -d: -f1)"
    check_line="$(grep -n 'converge_system_units.sh --check' "$f" | cut -d: -f1)"
    [ "$push_line" -lt "$run_line" ]
    [ "$run_line" -lt "$check_line" ]
}
```

Run: `bats tests/local/converge_units_table.bats -f sync_source_box` — Expected: FAIL.

- [ ] **Step 2: Edit `sync_source_box.sh`**

Immediately before the line `push magic_dingus_box_cpp/scripts/setup_memory_tuning.sh \` insert:

```bash
# The unit converge (setup_memory_tuning.sh step 1a) installs the box TREE's
# unit files, so the tree must hold the repo's: an image cut from a box
# whose /opt units are stale would ship those to every unit.
push magic_dingus_box_cpp/scripts/converge_system_units.sh \
     /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/converge_system_units.sh 755 \
     "converge_system_units.sh (installed units = the tree's; setup_memory_tuning step 1a)"
while IFS= read -r unit_file; do
    push "$unit_file" "/opt/magic_dingus_box/${unit_file}" 644 "${unit_file} (repo copy)"
done < <(cd "$REPO" && git ls-files '*.service' '*.timer')
```

Immediately after the `fi` that closes `if ssh ... setup_memory_tuning.sh" ...; then ... else ... FAILED=1` insert:

```bash
# The image must carry converged units: every installed tree unit equal to
# the tree, nothing MISSING or STALE, no leftover fix drop-in.
if ssh "${SSH_OPTS[@]}" "$PI_HOST" \
    "bash /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/converge_system_units.sh --check" 2>&1 | sed 's/^/          /'; then
    echo -e "  ${GREEN}OK${NC}      installed systemd units match the tree"
else
    echo -e "  ${RED}FAILED${NC}  installed systemd units differ from the tree (lines above)"; FAILED=1
fi
```

(`sync_source_box.sh` runs under `set -euo pipefail`, so the pipeline's status is the converge's.)

- [ ] **Step 3: Run**

Run: `bats tests/local/converge_units_table.bats && shellcheck -S error scripts/golden_image/sync_source_box.sh && bash -n scripts/golden_image/sync_source_box.sh`
Expected: all PASS, no shellcheck output.

- [ ] **Step 4: Commit**

```bash
git add scripts/golden_image/sync_source_box.sh tests/local/converge_units_table.bats
git commit -m "feat(golden-image): sync pushes the tree's units and gates on converged units

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: Documentation — CLAUDE.md, OTA guarantees, CHANGELOG, installer comments

**Files:**
- Modify: `CLAUDE.md` (the "Box-side half" passage ~line 556; the gunicorn passage ~line 319; add a subsection in "## OTA Updates")
- Modify: `OTA_UPDATE_GUARANTEES.md` (the systemd row in "What does NOT flow through OTA"; "Reaching field boxes — the rule"; a new row in "What gets UPDATED")
- Modify: `CHANGELOG.md` (`## [Unreleased]` → `### Fixed`)
- Modify: `magic_dingus_box_cpp/scripts/setup_audio_service.sh` (header lines 11-14), `magic_dingus_box_cpp/scripts/setup_ota_recovery.sh` (header line 14) — comments only

- [ ] **Step 1: CLAUDE.md — the setup_memory_tuning passage**

Replace the sentences from `Because OTA` + `never re-installs unit files, this script is also the delivery path` through `(until 2026-10-14 it didn't, and every box swept once per boot, then never again).` with:

```
It is also where installed systemd units converge to the tree (step 1a,
  `converge_system_units.sh`, 2026-10 — see "System units converge" under
  OTA Updates): unit fixes ship as unit-file edits and every delivery path
  installs them. The hand-written fix drop-ins it used to write
  (`stop-timeout.conf`, the storage-attach and smoke-test
  `mdb-timeout.conf`, the missing-search `mdb-boot-delay.conf`) are removed
  by the converge once each unit matches the tree — one of them, an empty
  `OnBootSec=` that resets EVERY trigger, stopped the missing-search timer
  for 10 days (2026-10-14).
```

- [ ] **Step 2: CLAUDE.md — the gunicorn passage**

Replace `that line is frozen in the field` + newline + `  because an OTA never rewrites unit files — and` with:

```
that line stays: boxes ran it frozen until the 2026-10 unit converge,
  and an older updater's failed-install rollback can still leave any
  installed unit in place — and
```

- [ ] **Step 3: CLAUDE.md — new subsection**

In `## OTA Updates`, after the bullet that begins `- **Never leaves a box without a kiosk** (2026-10)`, add:

```
- **System units converge** (2026-10, `scripts/converge_system_units.sh`):
  every delivery path makes `/etc/systemd/system` equal to the tree's unit
  files — `setup_memory_tuning.sh` step 1a runs it (deploy, OTA incl. the
  OLD updater's first hop, first boot, source-box sync) and every rollback
  runs the RESTORED tree's copy (or a stashed copy with `--tree`). Rules:
  - **A unit fix is a unit-file edit, never a drop-in.** Drop-ins are only
    for per-box values (`audio-service.conf`'s real uid) and posture tied
    to other state (`memory-protect.conf` + the slice companion). An
    operator's own drop-ins are never touched.
  - **A new unit needs a `POLICY` row** (gate base/mb, if-absent
    skip/install/enable/enable-now, on-change none/try-restart/rearm,
    redundant drop-in); `tests/local/converge_units_table.bats` fails
    without one, and refuses `install`/`enable` for a unit whose ExecStart
    is a `/usr/local/bin` helper (only `setup_services.sh` creates those).
    Unit files live in `systemd/`, `magic_dingus_box_cpp/systemd/` or
    `magic_dingus_box_cpp/scripts/{missing_search,episode_priority}/` only.
  - Installed units' enablement is never changed; masked (symlinked)
    units are never written; the kiosk, web admin and OTA recovery are
    never restarted or retired; MB units are installed/started only with
    `services/.env`; every start/stop is `--no-block` (it runs at boot).
  - A unit the tree stops shipping is disabled and removed (from
    `/var/lib/magic-dingus/system_units`, the installed set of the last
    run) — that is what makes a rollback remove a unit the rolled-back
    release added.
  - `verify_box.sh` FAILs drift (`--check`: DRIFT/MISSING/STALE; a leftover
    fix drop-in is a WARN); `sync_source_box.sh` refuses an image without
    converged units.
  - A failed-install or power-cut rollback is run by the OLDER updater, so
    on the hop into a release it cannot converge back: a release that
    changes a unit must be safe with the previous tree (the release that
    shipped the converge changed no unit at all).
```

- [ ] **Step 4: OTA_UPDATE_GUARANTEES.md**

(a) In "What gets UPDATED on every OTA", add this row after the `/usr/local/bin/...` refresh row:

```
| `/etc/systemd/system/*.service`, `*.timer` — the tree's units (`systemd/`, `magic_dingus_box_cpp/systemd/`, `magic_dingus_box_cpp/scripts/{missing_search,episode_priority}/`) | **Converged on every OTA as of the release after v1.10.0** by `converge_system_units.sh`, which the NEW tree's `setup_memory_tuning.sh` runs — so even the OLD `update.sh` delivers it on its first hop. Changed units are replaced by same-directory rename; absent ones are installed only when their policy row says so (Media Browser units only with `services/.env`); masked units, operators' drop-ins and installed units' enablement are never touched; the kiosk is never restarted (it is stopped while the hook runs and the OTA starts it). Every rollback converges back to the restored tree, retiring units the rolled-back release added. Rules and policy table: CLAUDE.md "System units converge". |
```

(b) Replace the whole `/etc/systemd/system/*.service`, `*.timer` row of "What does NOT flow through OTA" with:

```
| Unit **enablement**, masked units, and units whose policy is `skip` when absent | The converge (row above) keeps installed units current but never enables, disables or unmasks an installed unit, and never installs a unit that needs provisioning it cannot do (a `/usr/local/bin` helper, dnsmasq config, harness wiring). Before the converge (≤ v1.10.0) **no** unit file was ever reinstalled: unit files were frozen at provisioning time and unit fixes rode drop-ins from `setup_memory_tuning.sh`. |
```

(c) Replace the paragraph that starts `**Reaching field boxes — the rule.**` up to (not including) `The reverse direction:` with:

```
**Reaching field boxes — the rule.** Boxes in the field update with the
`update.sh` they already have, not the one in the new release. That
version rsyncs the tree, then runs the NEW tree's
`setup_network_hardening.sh` and `setup_memory_tuning.sh` as root, then
`daemon-reload` and starts the kiosk. Those two scripts are therefore the
only root-level delivery path a fix has on the update that ships it. A
**unit-file fix** is now an ordinary edit of the unit in the tree (plus its
`POLICY` row in `converge_system_units.sh` for a new unit), which
`setup_memory_tuning.sh`'s converge installs — never a drop-in. Any other
system-side fix (a sysctl, a service restart, anything else under `/etc`)
must ride one of the two scripts, idempotent, and never able to fail the
script. Examples: the port-80 redirect is `try-restart`ed by
`setup_network_hardening.sh`; the PulseAudio unit's per-box kiosk drop-in
(the real uid) is written by `setup_audio_service.sh`, which
`setup_memory_tuning.sh` calls.
```

- [ ] **Step 5: CHANGELOG.md**

Under `## [Unreleased]` → `### Fixed`, add as the first entry:

```
- **Updates now keep the box's background jobs exactly as designed.**
  Improvements to how the box schedules its background work (like the
  automatic retry for movies that found nothing to download) used to need
  special patches to reach boxes that were already set up, and one of
  those patches stopped that retry from running at all. Updates now put
  the jobs' own settings in place directly, undo them with the rest of an
  update if it is rolled back, and the Box Health check flags a box whose
  jobs don't match.
```

- [ ] **Step 6: Installer comments**

`setup_audio_service.sh` header — replace

```
#     kill the audio unit's PulseAudio on every kiosk stop. OTA never
#     replaces the kiosk unit file itself (OTA_UPDATE_GUARANTEES.md
#     "Reaching field boxes"), so the drop-in is the fix's only route to
#     fielded boxes;
```

with

```
#     kill the audio unit's PulseAudio on every kiosk stop. Since the
#     2026-10 unit converge the kiosk unit itself carries both, so this
#     part is redundant on converged boxes; it stays for boxes whose kiosk
#     unit is masked or not yet converged;
```

`setup_ota_recovery.sh` header — replace `(unit files are otherwise never refreshed by an OTA, so the update.sh` with `(it must exist before the install writes its marker — earlier than the unit converge in setup_memory_tuning.sh — so the update.sh`.

- [ ] **Step 7: Check and commit**

Run: `bats tests/local/changelog_format.bats && git diff --stat`
Expected: PASS; only the five files above changed.

```bash
git add CLAUDE.md OTA_UPDATE_GUARANTEES.md CHANGELOG.md magic_dingus_box_cpp/scripts/setup_audio_service.sh magic_dingus_box_cpp/scripts/setup_ota_recovery.sh
git commit -m "docs: unit fixes ship as unit-file edits; the converge delivers them

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: OTA rehearsal assertions + hardware-harness unit dirs; run the container rehearsal

**Files:**
- Modify: `tests/ota_rehearsal/box/scenario.sh` (helpers near line 38; `check_hooks_ran_old` lines ~305 and ~321-322; `scenario_old_cli` after the OLD rollback ~line 437; `scenario_old_web` after the NEW-admin rollback ~line 526; `scenario_new_path` after the V1 install ~line 48 and after the NEW rollback ~line 602)
- Modify: `tests/ota_rehearsal/hw_rehearsal.sh` (`derive_lists`, lines ~534-536)

**Interfaces:**
- Consumes: scenario.sh's `pass`/`fail`/`note`/`check`/`step`, `extract_release <ver>` → `/tmp/rel_<ver>`, `$INSTALL`, `$NEW_VER`, `$OLD_VER`; converge `--check --tree`.
- Produces: `units_match LABEL TREE [SCRIPT]`.

- [ ] **Step 1: Add the helper**

After the line `ver() { tr -d '[:space:]' < "$INSTALL/VERSION"; }` add:

```bash
# units_match <label> <tree> [converge script] — the installed units are
# <tree>'s (converge_system_units.sh --check; default: <tree>'s own script).
units_match() {
    local script="${3:-$2/magic_dingus_box_cpp/scripts/converge_system_units.sh}" out rc=0
    out=$(bash "$script" --check --tree "$2" 2>&1) || rc=$?
    if [[ $rc -eq 0 ]]; then pass "$1: installed units match the tree"
    else fail "$1: installed units differ from the tree (rc=$rc): $(echo "$out" | tr '\n' ' ' | cut -c1-300)"; fi
}
```

- [ ] **Step 2: Update `check_hooks_ran_old`**

Replace

```bash
    check "stop-timeout drop-in installed" test -f /etc/systemd/system/magic-dingus-box-cpp.service.d/stop-timeout.conf
```

with

```bash
    check "no stop-timeout fix drop-in (the converged kiosk unit carries TimeoutStopSec=20)" \
        test ! -e /etc/systemd/system/magic-dingus-box-cpp.service.d/stop-timeout.conf
    check "the converged kiosk unit is the tree's" \
        cmp -s "$INSTALL/magic_dingus_box_cpp/systemd/magic-dingus-box-cpp.service" /etc/systemd/system/magic-dingus-box-cpp.service
    units_match "after the OLD updater's install" "$INSTALL"
```

Replace

```bash
    check "OLD updater does not install magic-dingus-ota-recovery.service (arrives with the NEXT update)" \
        test ! -e /etc/systemd/system/magic-dingus-ota-recovery.service
```

with

```bash
    check "the NEW tree's unit converge installs magic-dingus-ota-recovery.service on the OLD updater's hop" \
        test -f /etc/systemd/system/magic-dingus-ota-recovery.service
```

- [ ] **Step 3: Rollback assertions**

In `scenario_old_cli`, after the `note "after rollback the ${NEW_VER} audio drop-in ..."` line add:

```bash
    extract_release "$NEW_VER"
    local uo; uo=$(bash "/tmp/rel_${NEW_VER}/magic_dingus_box_cpp/scripts/converge_system_units.sh" --check --tree "$INSTALL" 2>&1 | tr '\n' ' ')
    note "after the OLD updater's rollback the ${NEW_VER} units stay (it predates converge-back; the release changed no unit): ${uo:-none differ}"
```

In `scenario_old_web`, after `assert_rolled_back_to s0 s2 "after NEW-admin rollback" "$OLD_VER" "$STUB_SHA"` add:

```bash
    extract_release "$NEW_VER"
    units_match "after the NEW admin's rollback (converged back to ${OLD_VER}'s tree by the stashed script)" \
        "$INSTALL" "/tmp/rel_${NEW_VER}/magic_dingus_box_cpp/scripts/converge_system_units.sh"
```

In `scenario_new_path`, after `check "...and enabled" grep -qx magic-dingus-ota-recovery.service "$SHIM/enabled"` add `units_match "after the ${V1} install" "$INSTALL"`; after the line `run_updater rollback_new "$SCRIPTS/update.sh" rollback && pass "NEW rollback exit 0" || fail "NEW rollback failed"` add `units_match "after the NEW rollback to ${NEW_VER}" "$INSTALL"`.

- [ ] **Step 4: Hardware harness unit dirs**

In `tests/ota_rehearsal/hw_rehearsal.sh` `derive_lists`, in the three `git -C "$REPO" ...` commands that end with `-- systemd/ magic_dingus_box_cpp/systemd/ > ...`, change the pathspec to `-- systemd/ magic_dingus_box_cpp/systemd/ magic_dingus_box_cpp/scripts/missing_search/ magic_dingus_box_cpp/scripts/episode_priority/` (the converge now owns those units too, so an OLD field box carries OLD copies of them).

- [ ] **Step 5: Syntax + harness tests**

Run: `bash -n tests/ota_rehearsal/box/scenario.sh && bash -n tests/ota_rehearsal/hw_rehearsal.sh && bats tests/local/ota_hw_rehearsal.bats`
Expected: no syntax errors; all PASS.

- [ ] **Step 6: Commit**

```bash
git add tests/ota_rehearsal/box/scenario.sh tests/ota_rehearsal/hw_rehearsal.sh
git commit -m "test(ota-rehearsal): assert units converge on install and back on rollback

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 7: Run the container rehearsal (background, ~15 min; Docker must be running)**

Run (in the background): `tests/ota_rehearsal/run.sh > "$TMPDIR/ota-rehearsal.log" 2>&1`
Expected: exit 0; `grep -c '^FAIL' <logdir>/*/results.txt` is 0 for every scenario (logs under `$TMPDIR/mdb-ota-rehearsal/logs`). Read every `units_match` PASS line and the `old_cli` NOTE. A FAIL is a real defect: fix it in the owning task's code, re-run its tests, re-run this step.

---

### Task 11: Whole-suite verification, then the hardware gate

**Files:** none (verification only).

- [ ] **Step 1: Every suite and lint**

Run:
```bash
(cd magic_dingus_box_cpp/scripts/tests && bats ./*.bats)
bats tests/local/*.bats
shellcheck -S warning magic_dingus_box_cpp/scripts/converge_system_units.sh
shellcheck -S error magic_dingus_box_cpp/scripts/update.sh magic_dingus_box_cpp/scripts/setup_memory_tuning.sh magic_dingus_box_cpp/scripts/verify_box.sh scripts/golden_image/sync_source_box.sh
```
Expected: 0 failures, no shellcheck output. (Pi-tier tests in `tests/local` skip themselves when no Pi is reachable.)

- [ ] **Step 2: The release rule**

Run: `git diff --stat 5eb1769 HEAD -- systemd magic_dingus_box_cpp/systemd 'magic_dingus_box_cpp/scripts/missing_search/*.service' 'magic_dingus_box_cpp/scripts/missing_search/*.timer' 'magic_dingus_box_cpp/scripts/episode_priority/*.service' 'magic_dingus_box_cpp/scripts/episode_priority/*.timer'`
Expected: no `.service` / `.timer` file listed.

- [ ] **Step 3: Read-only preview on the owner's box (no changes)**

Run: `scp magic_dingus_box_cpp/scripts/converge_system_units.sh magic@10.0.0.227:/tmp/converge_preview.sh && ssh magic@10.0.0.227 'bash /tmp/converge_preview.sh --check --tree /opt/magic_dingus_box; echo "rc=$?"; rm -f /tmp/converge_preview.sh'`
Expected (spec section 7): `DRIFT magic-dingus-missing-search.timer`, `DRIFT magic-dingus-smoke-test.service`, `DRIFT magic-dingus-storage-attach.service`, `DROPIN /etc/systemd/system/magic-dingus-box-cpp.service.d/stop-timeout.conf` (the kiosk unit already matches, so its fix drop-in is redundant today; the three timing drop-ins are not reported because their units still drift), `rc=1` — nothing else. Anything more is a policy-table or design gap: stop and report it.

- [ ] **Step 4: STOP — ask the owner before any hardware rehearsal**

Do not run `hw_rehearsal.sh` or deploy to magicpi5 without an explicit yes. When approved, the run is:

```bash
PI_HOST=magic@10.0.0.227 tests/ota_rehearsal/hw_rehearsal.sh --dry-run
PI_HOST=magic@10.0.0.227 tests/ota_rehearsal/hw_rehearsal.sh --old v1.9.14 --new HEAD --yes
```

(The existing `field_state_1.9.14.txt` plus the git-derived `old-unit` copies reproduce a field box's stale units and drop-ins; its preflight requires deploying this checkout first, which converges the box.) Success: `results.txt` has no FAIL, `verify_box_*.txt` after NEW shows "installed systemd units match the tree", and the restore's system-state diff is clean.
