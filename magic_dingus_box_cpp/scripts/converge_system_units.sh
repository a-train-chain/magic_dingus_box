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
# Precedence order: a unit name found in more than one of these resolves to the
# FIRST (see tree_units). The root systemd/ is last on purpose — it is where
# deploy_cpp.sh stages its copies of units that live in magic_dingus_box_cpp/systemd/.
UNIT_DIRS="magic_dingus_box_cpp/systemd magic_dingus_box_cpp/scripts/missing_search magic_dingus_box_cpp/scripts/episode_priority systemd"
# The comment line every drop-in setup_memory_tuning.sh wrote carries.
DROPIN_MARKER="(setup_memory_tuning.sh)"
# No leading '-': a name is handed to systemctl as an argument, where a
# leading dash would be read as an option.
UNIT_NAME_RE='^[A-Za-z0-9@_][A-Za-z0-9@._-]*\.(service|timer)$'

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

# tree_units -> "<name><TAB><path>", one row per unit name, sorted by name.
# A name found in several UNIT_DIRS directories resolves to the first one in
# that order. Deployed boxes have such shadows: deploy_cpp.sh stages copies of
# magic-dingus-box-cpp.service and magic-cpu-performance.service into the
# box's root systemd/ (untracked), so refusing a duplicate would refuse the
# kiosk unit on every deployed box. A shadow whose content differs from the
# winner gets one note on STDERR (never stdout: --check stdout is findings
# only); an identical shadow is silent. A file whose name is not a well-formed
# unit name (UNIT_NAME_RE — e.g. a leading '-', which systemctl would take for
# an option) is skipped with a stderr note. Membership uses in_list, not a pipe.
tree_units() {
    local d f name winner seen="" rows=""
    {
        for d in $UNIT_DIRS; do
            for f in "$TREE/$d"/*.service "$TREE/$d"/*.timer; do
                [[ -f "$f" ]] || continue
                name="${f##*/}"
                if [[ ! "$name" =~ $UNIT_NAME_RE ]]; then
                    echo "[units] note: ignoring $f (not a well-formed unit name)" >&2
                    continue
                fi
                if in_list "$seen" "$name"; then
                    winner="$(awk -F'\t' -v n="$name" '$1 == n { print $2; exit }' <<< "$rows")"
                    cmp -s "$f" "$winner" \
                        || echo "[units] note: $f differs from $winner; using the latter" >&2
                    continue
                fi
                seen="${seen}${name}"$'\n'
                rows="${rows}${name}"$'\t'"${f}"$'\n'
                printf '%s\t%s\n' "$name" "$f"
            done
        done
    } | sort
}

is_protected() { case " $PROTECTED " in *" $1 "*) return 0 ;; esac; return 1; }

# in_list <newline-separated list> <name>: whole-line, literal match. Pure
# bash on purpose: `printf | grep -q` under pipefail reports failure whenever
# grep exits early and the writer dies of SIGPIPE, and callers act on "not in
# the list" by retiring a unit.
in_list() { case $'\n'"$1"$'\n' in *$'\n'"$2"$'\n'*) return 0 ;; esac; return 1; }

sc() { [[ "$SKIP_SYSTEMCTL" == "true" ]] && return 0; systemctl "$@"; }

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

# Step 3: a unit an earlier run saw installed that this tree does not ship —
# a rollback to a release without it, or a release that dropped it — is
# disabled, stopped and removed. Only well-formed names from the state file,
# never the update machinery (PROTECTED), never a symlink.
retire_stale_units() {
    local name dest
    [[ -f "$STATE" ]] || return 0
    while IFS= read -r name; do
        [[ "$name" =~ $UNIT_NAME_RE ]] || continue
        in_list "$TREE_NAMES" "$name" && continue
        is_protected "$name" && continue
        dest="$SD/$name"
        [[ -f "$dest" && ! -L "$dest" ]] || continue
        if [[ "$CHECK" == true ]]; then
            echo "STALE $name"; FINDINGS=$((FINDINGS + 1)); continue
        fi
        sc disable --now --no-block "$name" >/dev/null 2>&1 </dev/null \
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
    TREE_NAMES="$(printf '%s\n' "$LIST" | cut -f1)"
    # A wrong or half-restored tree must never look like "the tree ships
    # nothing": that would retire every installed unit.
    if ! in_list "$TREE_NAMES" magic-dingus-box-cpp.service; then
        echo "[units] ERROR: $TREE has no magic-dingus-box-cpp.service; not a Magic Dingus Box tree, nothing changed" >&2
        exit 2
    fi

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
    retire_stale_units
    retire_redundant_dropins
    [[ "$CHECK" == true ]] || apply_policies
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
