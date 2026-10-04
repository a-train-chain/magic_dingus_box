#!/bin/bash
#
# Magic Dingus Box - OTA Update Script
#
# This script handles over-the-air updates for Magic Dingus Box devices.
# It runs on the Raspberry Pi and communicates with GitHub to check for
# and install updates.
#
# Usage:
#   ./update.sh check              # Check for updates (returns JSON)
#   ./update.sh install <ver> <url> # Install specific version
#   ./update.sh rollback           # Rollback to previous version
#   ./update.sh channel [stable|beta]  # Show or set the update channel
#
# ============================================================================
# OPERATOR-CONTENT PRESERVATION CONTRACT — READ BEFORE EDITING THE rsync CALLS
# ============================================================================
# The four `rsync --exclude` lists in this file (backup, install, internal
# rollback, user-initiated rollback) are the implementation of the contract
# documented at /OTA_UPDATE_GUARANTEES.md (repo root).
#
# That doc enumerates every path operators rely on surviving an OTA update:
# their videos, ROMs, saves, settings, Media Browser VPN credentials, etc.
# Pre-v1.4.3 a missing exclude wiped operator's services/.env (VPN creds)
# and services/config/* (Radarr library DB) on every update — a bug we
# only caught during physical update-flow verification.
#
# When you add a new category of operator content anywhere under
# INSTALL_DIR, you MUST:
#   1. Add the exclude entry to ALL FOUR rsync invocations in this script.
#      Inconsistency between them produces partial-update data loss.
#   2. Update OTA_UPDATE_GUARANTEES.md's "preserved" table in the same
#      commit so the contract stays in sync with the implementation.
# ============================================================================
#
set -euo pipefail

# Configuration
INSTALL_DIR="${MAGIC_BASE_PATH:-/opt/magic_dingus_box}"
BACKUP_DIR="${MAGIC_BACKUP_DIR:-${HOME}/.magic_dingus_box_backup}"
TEMP_DIR="${MAGIC_TEMP_DIR:-/tmp/magic_update}"
GITHUB_REPO="${MAGIC_GITHUB_REPO:-a-train-chain/magic_dingus_box}"  # same override admin.py honors (forks)
GITHUB_API="${MAGIC_GITHUB_API:-https://api.github.com/repos/${GITHUB_REPO}/releases/latest}"
VERSION_FILE="${INSTALL_DIR}/VERSION"

# Update channel — "stable" (default) or "beta". See
# magic_dingus_box_cpp/docs/RELEASING.md and OTA_UPDATE_GUARANTEES.md
# "Update channels".
#
#   stable  asks GitHub for releases/latest, which by GitHub's own rules
#           NEVER returns a prerelease or a draft. Byte-for-byte the request
#           every updater before the channel feature makes.
#   beta    asks for the recent-releases LIST and takes the highest SemVer
#           among stable AND prerelease (-beta.N) releases, so a beta box
#           moves onto the final stable the moment it ships.
#
# The file lives under <install>/config/, which every OTA rsync excludes
# (/config/*) and deploy_cpp.sh never syncs — so an update or a rollback
# can never flip it. Absent, unreadable or anything but exactly "beta"
# means stable. Setting stable REMOVES the file: absence is the default, so
# there is nothing left to ship in an image. first_boot.sh deletes it on
# every clone and prepare_for_cloning.sh refuses to clone a beta box.
CHANNEL_FILE="${MAGIC_CHANNEL_FILE:-${INSTALL_DIR}/config/update_channel}"
GITHUB_RELEASES_API="${MAGIC_GITHUB_RELEASES_API:-https://api.github.com/repos/${GITHUB_REPO}/releases?per_page=20}"

# The ONE version grammar the OTA path accepts: X.Y.Z, optionally followed
# by a SemVer prerelease of exactly the form -beta.N. admin.py's
# _OTA_VERSION_RE and release.yml's tag check carry the same rule. [0-9],
# not [[:digit:]], so the locale cannot widen it.
VERSION_RE='^[0-9]+\.[0-9]+\.[0-9]+(-beta\.[0-9]+)?$'

# "An install is mid-flight" marker. Written once the backup is complete and
# BEFORE the kiosk is stopped; removed only after a verified start (or a
# completed rollback). If the box loses power in between, the marker
# survives and magic-dingus-ota-recovery.service (ordered before the kiosk)
# runs `update.sh recover` at the next boot, which restores the backup.
#
# It sits NEXT TO the backup dir — outside INSTALL_DIR, so no rsync
# --delete ever touches it, and in the same home directory as the backup it
# pairs with. The name deliberately starts with ".magic_dingus_box_backup"
# so prepare_for_cloning.sh's secret tripwire (/home/magic/
# .magic_dingus_box_backup*) refuses to clone a box that still carries one:
# the marker can never ship in a golden image. first_boot.sh also deletes it.
# The path is duplicated in systemd/magic-dingus-ota-recovery.service
# (ConditionPathExists=) — change both.
OTA_MARKER="${MAGIC_OTA_MARKER:-${BACKUP_DIR%/}.ota_in_progress}"

# Kiosk start verification (see verify_kiosk_started).
KIOSK_UNIT="magic-dingus-box-cpp.service"
# "No connected display" — must match platform::kExitNoDisplay in
# src/platform/kiosk_exit.h. The binary loaded and ran; there is no TV.
KIOSK_EXIT_NO_DISPLAY=69
# The kiosk waits on magic-dingus-audio.service (HDMI card wait + PulseAudio
# ready) and init_audio.sh (ExecStartPre), so the first start can
# legitimately take a while; 90 s matches systemd's default start timeout.
KIOSK_START_TIMEOUT="${MAGIC_KIOSK_START_TIMEOUT:-90}"
# A kiosk that reaches READY and then crashes looked healthy to the old
# "is-active after 2 s" check. It must stay up, same PID, no restarts, for
# this long before an update is committed.
KIOSK_STABLE_SECS="${MAGIC_KIOSK_STABLE_SECS:-10}"

# Set by `update.sh recover` (boot-time recovery). Runs inside a oneshot
# ordered BEFORE the kiosk unit, so it must never start/stop the kiosk or
# block on any other unit's job — that would deadlock the boot.
BOOT_RECOVERY="false"

# Testing mode overrides
# Set these environment variables to enable test mode:
#   MAGIC_SKIP_SYSTEMCTL=true  - Skip all systemctl calls
#   MAGIC_SKIP_BUILD=true      - Skip cmake/make build steps
#   MAGIC_DRY_RUN=true         - Skip all destructive operations
SKIP_SYSTEMCTL="${MAGIC_SKIP_SYSTEMCTL:-false}"
SKIP_BUILD="${MAGIC_SKIP_BUILD:-false}"
DRY_RUN="${MAGIC_DRY_RUN:-false}"

# Colors for terminal output (when not outputting JSON)
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Log to stderr (so JSON output on stdout is clean)
log() {
    echo -e "${GREEN}[UPDATE]${NC} $1" >&2
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1" >&2
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1" >&2
}

# Output JSON response
json_response() {
    local ok="$1"
    local message="$2"
    shift 2

    if [ "$ok" = "true" ]; then
        echo "{\"ok\": true, \"message\": \"$message\"$*}"
    else
        echo "{\"ok\": false, \"error\": {\"message\": \"$message\"}$*}"
    fi
}

# Output JSON progress (for streaming updates during install)
json_progress() {
    local stage="$1"
    local progress="$2"
    local message="${3:-}"
    echo "{\"ok\": true, \"stage\": \"$stage\", \"progress\": $progress, \"message\": \"$message\"}"
}

# Wrapper for systemctl calls (skipped in test mode)
run_systemctl() {
    if [ "$SKIP_SYSTEMCTL" = "true" ]; then
        log "SKIP: systemctl $*"
        return 0
    fi
    sudo systemctl "$@"
}

# Compile + verify + swap in one call (skipped in test mode). install_update
# does NOT use this: it calls compile_build and promote_build_dir separately
# so it can stop the kiosk between the two (see build_from_source). Kept for
# direct callers and the BATS suite. $1 = make -j (default 2).
run_build() {
    if [ "$SKIP_BUILD" = "true" ]; then
        log "SKIP: Build step"
        return 0
    fi
    compile_build "${1:-2}" || return $?
    promote_build_dir "$INSTALL_DIR/magic_dingus_box_cpp/build.new"
}

# Compile a clean tree into build.new/ and verify its binary. Never touches
# build/ — the running kiosk's binary AND its working directory (the unit's
# WorkingDirectory= is build/, and the kiosk resolves "../assets/..." and
# path_resolver candidates relative to it), which is why the OLD kiosk can
# keep running while this compiles.
#   $1 = make -j (1 or 2; see build_memory_plan)
# Returns 0 = build.new/ holds a verified binary, 1 = failed (build.new/
# removed), 3 = the compiler was KILLED, almost certainly by the OOM killer
# (build.new/ removed) — the caller may retry with more memory.
compile_build() {
    local jobs="${1:-2}"
    local build_dir="$INSTALL_DIR/magic_dingus_box_cpp/build"

    # ALWAYS build clean on an OTA. build/ is excluded from the install rsync
    # (see the --exclude below), so without this it is a long-lived directory
    # carrying objects compiled against the PREVIOUS release's headers. If a
    # release changes a struct layout, incremental make happily links old
    # objects against new ones: the build succeeds and the kiosk then segfaults
    # at startup inside an unrelated destructor. On a fielded box that is a
    # brick, unattended, with no one watching and no SSH.
    #
    # Observed for real on 2026-07-29 via deploy_cpp.sh (same defect, same
    # long-lived build dir) — 13 objects predated the headers they depended on.
    #
    # The cost is a full compile instead of a partial one. That is minutes on a
    # process that already downloads a tarball and restarts the kiosk, and it
    # is the correct trade against the failure it prevents. Rollback is
    # unaffected: create_backup() runs before this and deliberately includes
    # build/, so the previous working binary is still recoverable.
    #
    # The clean build happens in build.new/, NOT in build/. This used to
    # `rm -rf build` first and then compile for 8-10 minutes: a power cut or
    # a killed job anywhere in that window left a box with NO kiosk binary
    # at all — the unit then crash-looped forever (StartLimitIntervalSec=0)
    # while VERSION still named the old release. Now the old build/ stays
    # in place, runnable, until the new tree has compiled AND its binary has
    # passed verify_kiosk_binary; only then is it swapped in with renames
    # (promote_build_dir). A failed build leaves build/ exactly as it was.
    local new_dir="${build_dir}.new"
    log "Building clean in $(basename "$new_dir")/ (the current build/ stays in place until the new one is verified)"
    rm -rf "$new_dir" "${build_dir}.old"
    if ! mkdir -p "$new_dir"; then
        return 1
    fi

    # ENABLE_MEDIA_BROWSER defaults OFF in CMakeLists; production boxes
    # always build with it ON (runtime triple-gating hides it until
    # unlocked + VPN-provisioned). Before the clean-build fix above, the
    # long-lived build dir's CMake cache carried the ON from the last
    # deploy_cpp.sh run and masked this; with a fresh build dir, a plain
    # `cmake ..` would compile the movie kiosk OUT on every OTA.
    # BUILD_TESTS=OFF: the OTA rebuild used to compile the ENTIRE Catch2
    # test suite it never runs — real minutes on a Pi, plus a needless
    # GitHub fetch (Catch2) in the update path.
    # Subshells: the cd must not leak into the rest of install_update.
    # Both steps log to build.log beside build/ (outside the build dirs, so
    # it survives the cleanup below), and a failure prints the tail into the
    # job log. cmake's output used to go to /dev/null, so a failed configure
    # reported only "Build failed" — found by the OTA rehearsal.
    local build_log="${build_dir}.log"
    if ! (cd "$new_dir" && cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_MEDIA_BROWSER=ON -DBUILD_TESTS=OFF ..) > "$build_log" 2>&1; then
        log_error "cmake failed; last lines of $build_log:"
        tail -n 40 "$build_log" >&2 || true
        rm -rf "$new_dir"
        return 1
    fi

    # tee keeps make's progress streaming to the job log as before. make's
    # own exit code is recorded in a file rather than read from the
    # pipeline: the pipeline's status depends on pipefail (on here, off in
    # the bats harness), and an unguarded failing pipeline under set -e
    # would abort the whole update before the rollback could run.
    #
    # -j comes from build_memory_plan (1 or 2, never more: each cc1plus
    # peaks near 600 MB and a Pi 4B has 1.5 GB). The compile may now run
    # NEXT TO the live kiosk, so it is made the polite tenant:
    #   - oom_score_adj 1000: if memory does run out (the viewer starts a
    #     game mid-build), the kernel kills cc1plus, never the kiosk or
    #     RetroArch. Raising your own score needs no privilege; children
    #     inherit it. The caller retries a killed build with the kiosk
    #     stopped (return 3 below).
    #   - nice 19 + best-effort/lowest I/O: the menu and video keep the
    #     CPU and the disk. Not ionice's idle class — with qBittorrent
    #     streaming to the same disk, idle could starve the build for hours.
    #     -t: if the kernel refuses the I/O class, run make anyway.
    local -a prio=(nice -n 19)
    if command -v ionice >/dev/null 2>&1; then
        prio+=(ionice -c 2 -n 7 -t)
    fi
    local make_rc_file="${new_dir}/.make_rc"
    (cd "$new_dir" || { echo 1 > "$make_rc_file"; exit 0; }
     { echo 1000 > /proc/self/oom_score_adj; } 2>/dev/null || true
     "${prio[@]}" make -j"$jobs" 2>&1; echo $? > "$make_rc_file") | tee -a "$build_log" || true
    local make_rc
    make_rc="$(cat "$make_rc_file" 2>/dev/null || echo 1)"
    if [ "$make_rc" != "0" ]; then
        log_error "make failed; last lines of $build_log:"
        tail -n 60 "$build_log" >&2 || true
        rm -rf "$new_dir"
        # 137 = make itself SIGKILLed; the rest are how gcc/ld report a
        # child the OOM killer took. A real compile error matches none.
        if [ "$make_rc" = "137" ] || grep -qE 'Killed signal terminated program|internal compiler error: Killed|terminated with signal 9|virtual memory exhausted' "$build_log" 2>/dev/null; then
            log_warn "The compiler was killed (out of memory) at -j${jobs}"
            return 3
        fi
        return 1
    fi

    if ! verify_kiosk_binary "$new_dir/magic_dingus_box_cpp"; then
        log_error "Build produced no usable kiosk binary; keeping the current build/"
        rm -rf "$new_dir"
        return 1
    fi
    return 0
}

# Swap a freshly built, verified build tree into place.
#
# Two renames (build -> build.old, build.new -> build) rather than an rm -rf
# of the live tree: the gap between them is microseconds instead of a
# 10-minute compile, and an interruption even there is covered by the
# boot-time recovery (the OTA marker is still set, so the backup — which
# includes build/ — is restored). The data is flushed first so a power cut
# just after the rename cannot leave a renamed-but-empty binary behind.
promote_build_dir() {
    local new_dir="$1"
    local build_dir="$INSTALL_DIR/magic_dingus_box_cpp/build"
    local old_dir="${build_dir}.old"

    sync 2>/dev/null || true
    rm -rf "$old_dir"
    if [ -d "$build_dir" ] && ! mv "$build_dir" "$old_dir"; then
        log_error "Could not move the current build/ aside"
        return 1
    fi
    if ! mv "$new_dir" "$build_dir"; then
        log_error "Could not move the new build into place; restoring the previous build/"
        [ -d "$old_dir" ] && mv "$old_dir" "$build_dir"
        return 1
    fi
    rm -rf "$old_dir"
    sync 2>/dev/null || true
    return 0
}

# ---------------------------------------------------------------------------
# Keeping the TV on during a source build
# ---------------------------------------------------------------------------
# A source build (no usable pre-compiled binary) takes 8-10 min at -j2 on a
# Pi 4B. It used to run with the kiosk stopped — a black TV for the whole
# compile. The compile only ever writes build.new/ (compile_build), so the
# OLD kiosk can keep running until the swap, IF the box has the memory for
# both. That is decided per box, from memory measured with the kiosk
# running, by build_memory_plan below.
#
# Memory figures. Budget per make job: 600 MiB — CLAUDE.md's "each cc1plus
# peaks near 600 MB" (deploy_cpp.sh measured ~430 MB on this tree and
# budgets 600; the final link runs alone, inside one job's budget). Reserve:
# 100 MiB for make/cmake, the page cache the compile itself churns, and
# kernel slack. The reserve is deliberately small because the per-job
# budget already carries ~170 MiB/job over the measured peak, i.e. a -j2
# build started exactly at its floor still has ~440 MiB of real slack
# (-j1: ~270 MiB) for the viewer to start a movie or a PS1 game. If they
# overrun it anyway, the compiler — at oom_score_adj 1000 — is what dies,
# and build_from_source retries with the kiosk stopped. Validate on a
# Pi 4B before trusting these numbers further (OTA_UPDATE_GUARANTEES.md).
OTA_BUILD_JOB_KIB=$((600 * 1024))
OTA_BUILD_RESERVE_KIB=$((100 * 1024))
# MemAvailable (kiosk running) needed to compile at -j2 beside it: 1300 MiB.
OTA_KEEP_J2_FLOOR_KIB=$((2 * OTA_BUILD_JOB_KIB + OTA_BUILD_RESERVE_KIB))
# ... and at -j1 (twice as slow, but the TV works): 700 MiB.
OTA_KEEP_J1_FLOOR_KIB=$((OTA_BUILD_JOB_KIB + OTA_BUILD_RESERVE_KIB))
# Docker stop returns once the containers have exited; give the kernel a
# moment to settle the freed pages before measuring again.
OTA_PAUSE_SETTLE_SECS="${MAGIC_PAUSE_SETTLE_SECS:-2}"

# The Media Browser playback pause (playback_services_pause.sh) and its
# marker — the same script and marker the kiosk's quiet modes use, so the
# cascade watcher and the Content Manager already honor a pause made here.
PLAYBACK_PAUSE_MARKER="${MAGIC_PLAYBACK_PAUSE_MARKER:-/tmp/mdb_playback_services_paused}"

# State for the current install (set by plan_source_build / the helpers).
BUILD_JOBS=2
BUILD_KEEP_KIOSK="false"
KIOSK_STOPPED_FOR_UPDATE="false"
OTA_SERVICES_PAUSED_BY_US="false"

# Which board is this? Same rule as platform::PlatformProfile (model string
# prefix WITH the trailing space, so a "Raspberry Pi 400" is not a Pi 4).
# Prints pi4 | pi5 | unknown. MAGIC_DEVICE_MODEL_FILE is the test seam.
detect_board() {
    local model
    model="$(tr -d '\0' < "${MAGIC_DEVICE_MODEL_FILE:-/proc/device-tree/model}" 2>/dev/null)" || true
    case "$model" in
        "Raspberry Pi 4 "*) echo "pi4" ;;
        "Raspberry Pi 5 "*) echo "pi5" ;;
        *)                  echo "unknown" ;;
    esac
}

# MemAvailable in KiB, 0 when it cannot be read (0 = "no evidence", which
# build_memory_plan treats as too little). MAGIC_MEMINFO_FILE = test seam.
read_mem_available_kib() {
    local kib
    kib="$(awk '/^MemAvailable:/ {print $2; exit}' "${MAGIC_MEMINFO_FILE:-/proc/meminfo}" 2>/dev/null)" || true
    [[ "$kib" =~ ^[0-9]+$ ]] || kib=0
    echo "$kib"
}

# THE decision. Pure (no I/O) — pinned by a table test in test_update.bats.
#   $1 board (pi4|pi5|unknown)  $2 MemAvailable KiB, kiosk running
#   $3 kiosk_active (1|0)       $4 can_pause (1 = Media Browser services
#                                  can still be paused to free memory)
# Prints one of:
#   keep_j2         compile at -j2 with the kiosk up
#   keep_j1         compile at -j1 with the kiosk up (slower; TV works)
#   pause_services  not enough yet — pause the services, re-measure, ask
#                   again with can_pause=0
#   stop_kiosk      the old behaviour: stop the kiosk first, compile -j2
# Why each rule:
#   - unknown board: the thresholds come from Pi measurements; anything
#     else keeps the behaviour that has always worked.
#   - kiosk not active: there is no picture to keep (TV off = exit-69
#     restart loop, or already stopped), and every restart of a looping
#     kiosk runs its startup "unpause" — it would undo a services pause.
#   - never above -j2, even with memory to spare: the kiosk needs CPU too,
#     and two of four cores is what the Pi 4B has always built with.
#   - pause only to reach -j1 (keeping the TV), never to upgrade -j1 to
#     -j2: paused services hide Movies on the TV for the whole build
#     (the kiosk's tunnel monitor stops seeing Radarr).
build_memory_plan() {
    local board="$1" avail="$2" active="$3" can_pause="$4"
    [[ "$avail" =~ ^[0-9]+$ ]] || avail=0
    case "$board" in
        pi4|pi5) ;;
        *) echo "stop_kiosk"; return 0 ;;
    esac
    if [ "$active" != "1" ]; then
        echo "stop_kiosk"
    elif [ "$avail" -ge "$OTA_KEEP_J2_FLOOR_KIB" ]; then
        echo "keep_j2"
    elif [ "$avail" -ge "$OTA_KEEP_J1_FLOOR_KIB" ]; then
        echo "keep_j1"
    elif [ "$can_pause" = "1" ]; then
        echo "pause_services"
    else
        echo "stop_kiosk"
    fi
}

# Is the kiosk up and showing a picture right now? Type=notify, so "active"
# means it reached READY (a no-display exit 69 never does). In test mode the
# state comes from MAGIC_KIOSK_STATE (default active), never from systemd.
kiosk_is_active() {
    local state
    if [ "$SKIP_SYSTEMCTL" = "true" ]; then
        state="${MAGIC_KIOSK_STATE:-active}"
    else
        state="$(kiosk_prop ActiveState)"
    fi
    [ "$state" = "active" ]
}

# The pause script to use: the /usr/local/bin copy the kiosk itself calls
# (it exists only on Media Browser boxes). Never a CI runner's docker:
# test mode needs the explicit MAGIC_PLAYBACK_PAUSE_SCRIPT seam.
playback_pause_script() {
    if [ -n "${MAGIC_PLAYBACK_PAUSE_SCRIPT:-}" ]; then
        echo "$MAGIC_PLAYBACK_PAUSE_SCRIPT"
    elif [ "$SKIP_SYSTEMCTL" != "true" ] && [ -x /usr/local/bin/playback_services_pause.sh ]; then
        echo /usr/local/bin/playback_services_pause.sh
    fi
}

# Can the build free memory by pausing the Media Browser services? Only on
# a provisioned box (services/.env — the same gate the kiosk uses), and NOT
# when they are already paused: the marker means the kiosk paused them for
# a movie or game in progress. That memory is already free (it is in the
# measurement), and those services are the kiosk's to resume, not ours.
services_pausable() {
    [ -f "$INSTALL_DIR/services/.env" ] || return 1
    [ -n "$(playback_pause_script)" ] || return 1
    [ ! -e "$PLAYBACK_PAUSE_MARKER" ]
}

# Stop Radarr/Sonarr/Prowlarr/Byparr for the build (qBittorrent and Gluetun
# are untouched: downloads continue). The script is bounded (20 s compose-
# lock wait, 2 s stop timeout). fd 9 — our single-flight update lock — is
# closed for it: it opens its own fd 9 for the compose lock.
pause_services_for_build() {
    local script
    script="$(playback_pause_script)"
    [ -n "$script" ] || return 1
    log "Pausing Media Browser services to free memory for the build"
    OTA_SERVICES_PAUSED_BY_US="true"
    bash "$script" pause 9>&- >&2 || log_warn "playback_services_pause.sh pause reported a failure"
}

# Undo pause_services_for_build — and ONLY that (consent record, like the
# kiosk's own pause): idempotent, so it is safe on every exit path. Called
# after the compile, from fail_install, and from the EXIT trap the install
# dispatcher sets (killed job, set -e abort). A SIGKILL skips even the
# trap; then the next kiosk start's crash-recovery unpause, or the next
# boot (magic-dingus-services runs compose up -d; /tmp is tmpfs, so the
# marker is gone too), brings them back.
resume_build_paused_services() {
    [ "$OTA_SERVICES_PAUSED_BY_US" = "true" ] || return 0
    OTA_SERVICES_PAUSED_BY_US="false"
    local script
    script="$(playback_pause_script)"
    [ -n "$script" ] || return 0
    log "Resuming the Media Browser services paused for the build"
    bash "$script" unpause 9>&- >&2 || log_warn "playback_services_pause.sh unpause reported a failure"
}

# Stop the kiosk for the swap + restart. Exactly once per install: the
# final guard before verify_kiosk_started calls this too, because that
# check STARTS the unit — on a unit that is still running the old binary,
# `start` is a no-op and the old process would be "verified".
#   $1 = progress percentage for the stage line
stop_kiosk_for_swap() {
    [ "$KIOSK_STOPPED_FOR_UPDATE" = "true" ] && return 0
    json_progress "stopping_services" "${1:-80}" "Stopping the kiosk to switch to the new version..."
    log "Stopping C++ service..."
    run_systemctl stop "$KIOSK_UNIT" 2>/dev/null || true
    KIOSK_STOPPED_FOR_UPDATE="true"
    sleep 1
}

# Measure, decide, and (if that is the plan) pause services, then decide
# again. Sets BUILD_JOBS and BUILD_KEEP_KIOSK.
plan_source_build() {
    local board avail active=0 can_pause=0 plan
    board="$(detect_board)"
    kiosk_is_active && active=1
    services_pausable && can_pause=1
    avail="$(read_mem_available_kib)"
    plan="$(build_memory_plan "$board" "$avail" "$active" "$can_pause")"
    log "Build memory plan: board=${board} MemAvailable=$((avail / 1024))MiB kiosk_active=${active} can_pause=${can_pause} -> ${plan}"

    if [ "$plan" = "pause_services" ]; then
        json_progress "building" 58 "Pausing Movies services to make room for the build..."
        pause_services_for_build || true
        sleep "$OTA_PAUSE_SETTLE_SECS"
        avail="$(read_mem_available_kib)"
        plan="$(build_memory_plan "$board" "$avail" "$active" 0)"
        log "Build memory plan after pausing services: MemAvailable=$((avail / 1024))MiB -> ${plan}"
    fi

    case "$plan" in
        keep_j2) BUILD_KEEP_KIOSK="true";  BUILD_JOBS=2 ;;
        keep_j1) BUILD_KEEP_KIOSK="true";  BUILD_JOBS=1 ;;
        *)       BUILD_KEEP_KIOSK="false"; BUILD_JOBS=2 ;;   # today's proven setting
    esac
}

# The source-build path of install_update: plan, compile (the old kiosk
# running when the plan allows), stop the kiosk, swap. Returns non-zero on
# failure with build/ untouched and any paused services already resumed.
build_from_source() {
    if [ "$SKIP_BUILD" = "true" ]; then
        log "SKIP: Build step"
        return 0
    fi
    local build_dir="$INSTALL_DIR/magic_dingus_box_cpp/build"

    plan_source_build
    if [ "$BUILD_KEEP_KIOSK" = "true" ]; then
        local eta="8-10"
        [ "$BUILD_JOBS" = "1" ] && eta="15-20"
        json_progress "building" 60 "Compiling from source - the TV keeps working meanwhile (about ${eta} minutes)..."
        log "Compiling at -j${BUILD_JOBS} with the current kiosk still running"
    else
        stop_kiosk_for_swap 58
        json_progress "building" 60 "Compiling from source (this may take 8-10 minutes)..."
        log "Compiling at -j${BUILD_JOBS} with the kiosk stopped"
    fi

    local rc=0
    compile_build "$BUILD_JOBS" || rc=$?

    # Killed (OOM): one retry in the most frugal setup there is — kiosk
    # stopped, -j1. A real compile error (rc 1) is not retried.
    if [ "$rc" -eq 3 ] && { [ "$BUILD_KEEP_KIOSK" = "true" ] || [ "$BUILD_JOBS" != "1" ]; }; then
        log_warn "Retrying the build with the kiosk stopped at -j1"
        stop_kiosk_for_swap 62
        json_progress "building" 65 "The box ran short of memory; retrying the build with the TV paused..."
        BUILD_KEEP_KIOSK="false"
        BUILD_JOBS=1
        rc=0
        compile_build 1 || rc=$?
    fi

    # The memory is no longer needed. Resume before the hooks below:
    # converge_custom_formats.sh needs Radarr/Sonarr answering.
    resume_build_paused_services

    [ "$rc" -eq 0 ] || return 1

    # The swap. The kiosk must be down for it: renaming build/ away from
    # under a running kiosk deletes its working directory.
    stop_kiosk_for_swap 80
    promote_build_dir "${build_dir}.new"
}

# The ELF e_machine low byte the kiosk binary must carry on this box
# (EM_AARCH64 = 0xb7 on the Pis, EM_X86_64 = 0x3e on an x86 dev machine).
# Empty = do not check the machine (unknown host; the ELF checks still
# apply). MAGIC_EXPECT_ELF_MACHINE overrides for tests.
expected_elf_machine() {
    if [ -n "${MAGIC_EXPECT_ELF_MACHINE:-}" ]; then
        echo "$MAGIC_EXPECT_ELF_MACHINE"
        return 0
    fi
    case "$(uname -m)" in
        aarch64) echo "b7" ;;
        x86_64)  echo "3e" ;;
        *)       echo "" ;;
    esac
}

# Is this file a kiosk binary this box can actually run? Non-empty, a
# 64-bit little-endian ELF for this CPU, and executable. Reads the ELF
# header directly (od) rather than trusting `file`, which is not guaranteed
# to be installed and whose wording varies between versions.
verify_kiosk_binary() {
    local bin="$1"
    if [ ! -f "$bin" ] || [ ! -s "$bin" ]; then
        log_error "Kiosk binary missing or empty: $bin"
        return 1
    fi

    local -a h
    read -r -a h <<<"$(od -An -tx1 -N20 "$bin" 2>/dev/null | tr '\n' ' ')"
    if [ "${#h[@]}" -lt 20 ] || [ "${h[0]}${h[1]}${h[2]}${h[3]}" != "7f454c46" ]; then
        log_error "Kiosk binary is not an ELF executable: $bin"
        return 1
    fi
    if [ "${h[4]}" != "02" ] || [ "${h[5]}" != "01" ]; then
        log_error "Kiosk binary is not a 64-bit little-endian ELF: $bin"
        return 1
    fi
    local machine
    machine="$(expected_elf_machine)"
    if [ -n "$machine" ] && { [ "${h[18]}" != "$machine" ] || [ "${h[19]}" != "00" ]; }; then
        log_error "Kiosk binary is built for a different CPU (e_machine ${h[19]}${h[18]}, expected 00${machine}): $bin"
        return 1
    fi
    if [ ! -x "$bin" ]; then
        log_error "Kiosk binary is not executable: $bin"
        return 1
    fi
    return 0
}

# Put a single (pre-compiled) kiosk binary into build/ without ever leaving
# a truncated or missing binary behind. The old code `cp`'d straight over
# the live file: cp truncates the destination first, so a full SD card
# (ENOSPC) or a power cut mid-copy left a zero-length or half-written
# kiosk. Copy to <binary>.new, verify, flush, then rename over the live
# path — rename is atomic, so the live path always holds a whole binary,
# old or new.
install_kiosk_binary() {
    local src="$1"
    local dest_dir="$INSTALL_DIR/magic_dingus_box_cpp/build"
    local dest="$dest_dir/magic_dingus_box_cpp"
    local tmp="${dest}.new"

    if ! mkdir -p "$dest_dir"; then
        log_error "Could not create $dest_dir"
        return 1
    fi
    rm -f "$tmp"
    if ! cp "$src" "$tmp" || ! chmod 0755 "$tmp"; then
        log_error "Could not stage the new kiosk binary (disk full?)"
        rm -f "$tmp"
        return 1
    fi
    if ! verify_kiosk_binary "$tmp"; then
        rm -f "$tmp"
        return 1
    fi
    sync 2>/dev/null || true
    if ! mv -f "$tmp" "$dest"; then
        log_error "Could not move the new kiosk binary into place"
        rm -f "$tmp"
        return 1
    fi
    return 0
}

# Make every file in the install tree owned by the user that runs updates.
#
# Root-owned files land in the tree whenever a sudo/systemd process writes
# there (magic-first-boot.service and import_library_movies.sh both have).
# update.sh runs as the unprivileged web-service user, so such a file
# (a) fails the BACKUP rsync outright when it is root 0600 — unreadable —
# which blocked every OTA on that box forever, and (b) makes the INSTALL
# rsync exit 23 when it cannot replace it, which used to be accepted as
# "OK" and reported a half-applied update as a success. deploy_cpp.sh's
# Step 0.9 does the same normalization before its rsync.
#
# services/config (Docker-owned service state) and services/.env (root-
# owned secrets read by systemd's EnvironmentFile=) MUST keep their
# ownership and are pruned. -xdev keeps the walk off any drive mounted
# inside the tree. Only files that are actually wrong are touched, so a
# clean box costs one read-only walk. Best-effort: a failure is logged and
# the rsync exit-code checks below still catch anything left unfixable.
normalize_tree_ownership() {
    local uid gid
    if [ "$(id -u)" -eq 0 ]; then
        # Run by hand as root: normalize to whoever owns the tree.
        uid=$(stat -c '%u' "$INSTALL_DIR" 2>/dev/null || stat -f '%u' "$INSTALL_DIR" 2>/dev/null) || return 0
        gid=$(stat -c '%g' "$INSTALL_DIR" 2>/dev/null || stat -f '%g' "$INSTALL_DIR" 2>/dev/null) || return 0
        [ "$uid" -ne 0 ] || return 0
    else
        uid=$(id -u)
        gid=$(id -g)
    fi

    local -a prune=( \( -path "$INSTALL_DIR/services/config" -o -path "$INSTALL_DIR/services/.env" \) -prune )
    local -a wrong=( \( ! -user "$uid" -o ! -group "$gid" \) )

    if [ -z "$(find "$INSTALL_DIR" -xdev "${prune[@]}" -o "${wrong[@]}" -print 2>/dev/null | head -1)" ]; then
        return 0
    fi

    log "Normalizing ownership of $INSTALL_DIR (files left by root-run tools)"
    local cmd=(find "$INSTALL_DIR" -xdev "${prune[@]}" -o "${wrong[@]}" -exec chown -h "${uid}:${gid}" {} +)
    if [ "$(id -u)" -eq 0 ]; then
        "${cmd[@]}" || log_warn "ownership normalization incomplete"
    else
        sudo -n "${cmd[@]}" 2>/dev/null || log_warn "ownership normalization failed (no passwordless sudo?)"
    fi
}

# rsync exit codes: 0 = done; 24 = some SOURCE files vanished mid-transfer
# (e.g. the running web admin's atomic-rename temp files) — harmless. 23 =
# "partial transfer due to error": some files were NOT written. It used to
# be accepted as OK, which reported a half-applied update as a success.
rsync_exit_ok() {
    [ "$1" -eq 0 ] || [ "$1" -eq 24 ]
}

# ---------------------------------------------------------------------------
# Interrupted-install marker (see OTA_MARKER at the top)
# ---------------------------------------------------------------------------
write_ota_marker() {
    local target="$1" from="$2"
    mkdir -p "$(dirname "$OTA_MARKER")" 2>/dev/null || true
    printf 'target=%s\nfrom=%s\nstarted=%s\n' "$target" "$from" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        > "${OTA_MARKER}.tmp" && mv -f "${OTA_MARKER}.tmp" "$OTA_MARKER"
    sync 2>/dev/null || true
}

clear_ota_marker() {
    rm -f "$OTA_MARKER" "${OTA_MARKER}.tmp"
    sync 2>/dev/null || true
}

ota_marker_field() {
    sed -n "s/^$1=//p" "$OTA_MARKER" 2>/dev/null | head -1
}

# Install (or refresh) the boot-time recovery unit. Runs at the start of
# every install, before the marker is written, so a box gets the unit from
# the update.sh that will rely on it — unit files are otherwise never
# refreshed by an OTA (see OTA_UPDATE_GUARANTEES.md). Root-only work, so
# `sudo -n`, skipped in test mode like every other system-touching call,
# and never fatal: without the unit, an interrupted install is still
# repaired by the NEXT install (install_update recovers first).
ensure_ota_recovery_unit() {
    if [ "$SKIP_SYSTEMCTL" = "true" ]; then
        log "SKIP: OTA recovery unit install (test mode)"
        return 0
    fi
    local installer="${INSTALL_DIR}/magic_dingus_box_cpp/scripts/setup_ota_recovery.sh"
    if [ -f "$installer" ]; then
        sudo -n bash "$installer" >&2 \
            || log_warn "could not install the OTA recovery unit (power-loss recovery falls back to the next update)"
    fi
}

# Retry download with exponential backoff
# Args: $1=output_file, $2=url, $3=max_time, $4=description
retry_download() {
    local output_file="$1"
    local url="$2"
    local max_time="${3:-600}"
    local description="${4:-file}"

    local max_attempts=3
    local wait_times=(5 15 45)

    for attempt in $(seq 1 $max_attempts); do
        log "Download attempt $attempt/$max_attempts for $description"

        local curl_exit=0
        curl -L -o "$output_file" \
            --connect-timeout 30 \
            --max-time "$max_time" \
            --progress-bar \
            "$url" 2>&2 || curl_exit=$?

        if [ "$curl_exit" -eq 0 ]; then
            return 0
        fi

        # Permanent errors (don't retry): malformed URL, protocol errors
        local permanent_errors="3 4 5 23 27"
        if echo " $permanent_errors " | grep -q " $curl_exit "; then
            log_error "Permanent download error (curl exit: $curl_exit)"
            return 1
        fi

        if [ "$attempt" -lt "$max_attempts" ]; then
            local wait_time="${wait_times[$((attempt-1))]}"
            log_warn "Download failed (curl exit: $curl_exit), retrying in ${wait_time}s..."
            json_progress "downloading" 15 "Retry in ${wait_time}s..."
            sleep "$wait_time"
        fi
    done

    log_error "Download failed after $max_attempts attempts"
    return 1
}

# Get device architecture for binary matching
get_device_arch() {
    local arch=$(uname -m)
    case "$arch" in
        aarch64) echo "arm64" ;;
        armv7l) echo "arm32" ;;
        x86_64) echo "x64" ;;
        *) echo "unknown" ;;
    esac
}

# Check if pre-compiled binary exists for this release.
#
# Returns a binary asset download URL on stdout, or empty string if no
# matching pre-compiled binary is published for this version+arch (the
# common case — most v1.x.y releases ship source tarballs only and the
# install path falls through to compile-from-source).
#
# This function is in the install hot path. Two bugs in the v1.5.0
# version of this code caused a silent mid-install exit that left the
# kiosk non-running. Both fixed in v1.5.1:
#
#   1. Double-"v" URL bug. Callers may pass either "v1.5.0" or "1.5.0";
#      the function unconditionally prepended "v" via `v${version}`,
#      producing /releases/tags/vv1.5.0 → 404. Fix: strip a single
#      leading "v" from the input before building the URL.
#
#   2. grep-no-match-kills-script. The grep|head|sed pipeline returns
#      exit code 1 when grep finds no matches (which is the COMMON
#      case — only releases with custom-attached binary assets have a
#      browser_download_url ending in -arm64*.tar.gz; most don't).
#      Under the script-level `set -euo pipefail`, that nonzero
#      pipeline propagated up and silently terminated the entire
#      install — VERSION had already been rsync'd to the new value,
#      but the rebuild + service-restart steps never ran, leaving
#      the kiosk inactive with no error reported to the caller.
#      Fix: run the parse in a subshell with relaxed shell flags.
#
# The subshell pattern (rather than `set +e` directly inside the
# function) is deliberate — it scopes the flag relaxation to this
# function only, so the install path's strict mode remains in force
# for the rest of the script and a rollback still triggers cleanly
# on any real install failure that occurs later.
get_binary_url() {
    local version="${1#v}"   # Strip optional leading "v" (bug #1 fix)
    local arch=$(get_device_arch)

    # Never interpolate anything but X.Y.Z[-beta.N] into the API URL below
    # (curl would normalize a "/../" in it onto another repo).
    # install_update() already enforces this; an empty result means "build
    # from source". releases/tags/<tag> serves prereleases too.
    version_valid "$version" || return 0

    (
        set +e +o pipefail   # Bug #2 fix: tolerate grep no-match
        local response
        response=$(curl -s "https://api.github.com/repos/${GITHUB_REPO}/releases/tags/v${version}" 2>/dev/null)
        [ -z "$response" ] && exit 0

        echo "$response" \
            | grep -o "\"browser_download_url\": *\"[^\"]*-${arch}[^\"]*\.tar\.gz\"" \
            | head -1 \
            | sed 's/.*"\(http[^"]*\)".*/\1/'
    ) || true
}

# Get current installed version
get_current_version() {
    if [ -f "$VERSION_FILE" ]; then
        cat "$VERSION_FILE" | tr -d '[:space:]'
    else
        echo "0.0.0"
    fi
}

# ---------------------------------------------------------------------------
# Versions — SemVer precedence, pure bash (no sort -V, no subprocess tools).
#
# `sort -V` was the comparator until the beta channel. It is wrong for
# prereleases in exactly the direction that matters: it sorts 1.10.1 BEFORE
# 1.10.1-beta.1 (a bare string sorts before its own extensions), so a beta
# box would never have been offered the final release it was testing. SemVer
# says a prerelease has LOWER precedence than its release:
#   1.10.0 < 1.10.1-beta.1 < 1.10.1-beta.2 < 1.10.1-beta.10 < 1.10.1
# Every field compares numerically, at any length (no 64-bit overflow, no
# octal surprise on a leading zero). Pinned case-by-case in test_update.bats.
# ---------------------------------------------------------------------------

# version_valid V — true iff V is X.Y.Z or X.Y.Z-beta.N.
version_valid() {
    [[ "${1:-}" =~ $VERSION_RE ]]
}

# version_is_prerelease V — true iff V carries a -beta.N suffix.
version_is_prerelease() {
    [[ "${1:-}" == *-beta.* ]]
}

# _num_cmp A B — compare two digit strings numerically; prints -1, 0 or 1.
# Leading zeros are stripped and lengths compared first, so the result is
# exact for any length and never touches shell arithmetic.
_num_cmp() {
    local a="${1#"${1%%[!0]*}"}" b="${2#"${2%%[!0]*}"}"
    a="${a:-0}"; b="${b:-0}"
    if [ "${#a}" -ne "${#b}" ]; then
        if [ "${#a}" -lt "${#b}" ]; then echo -1; else echo 1; fi
    elif [[ "$a" < "$b" ]]; then echo -1
    elif [[ "$a" > "$b" ]]; then echo 1
    else echo 0
    fi
}

# version_cmp A B — SemVer precedence of two VALID versions; prints -1 (A<B),
# 0 (equal) or 1 (A>B). Returns 2 and prints nothing if either is invalid.
version_cmp() {
    version_valid "${1:-}" && version_valid "${2:-}" || return 2
    local a_core="${1%%-*}" b_core="${2%%-*}" a_pre="" b_pre="" c i
    version_is_prerelease "$1" && a_pre="${1##*-beta.}"
    version_is_prerelease "$2" && b_pre="${2##*-beta.}"
    local -a a_f b_f
    IFS=. read -r -a a_f <<<"$a_core"
    IFS=. read -r -a b_f <<<"$b_core"
    for i in 0 1 2; do
        c=$(_num_cmp "${a_f[$i]}" "${b_f[$i]}")
        if [ "$c" != 0 ]; then echo "$c"; return 0; fi
    done
    # Same X.Y.Z: a release outranks any of its prereleases.
    if [ -z "$a_pre" ] && [ -z "$b_pre" ]; then echo 0
    elif [ -z "$a_pre" ]; then echo 1
    elif [ -z "$b_pre" ]; then echo -1
    else _num_cmp "$a_pre" "$b_pre"
    fi
}

# version_lt CURRENT CANDIDATE — true (0) iff CANDIDATE is an upgrade.
#   - An unparseable CANDIDATE is never an upgrade (never offer garbage).
#   - An unparseable CURRENT (VERSION missing reads as 0.0.0; a corrupted
#     VERSION reads as junk) IS upgradeable to any valid candidate — an
#     update is the repair for a box that cannot say what it runs.
#   - Equal versions are not an upgrade, and neither is a lower one: no
#     channel ever offers a downgrade.
version_lt() {
    version_valid "${2:-}" || return 1
    version_valid "${1:-}" || return 0
    [ "$(version_cmp "$1" "$2")" = "-1" ]
}

# ---------------------------------------------------------------------------
# Update channel (see CHANNEL_FILE above)
# ---------------------------------------------------------------------------

# read_update_channel — prints "beta" or "stable". Only the exact word
# "beta" (surrounding whitespace ignored) selects beta; anything else —
# absent file, unreadable file, typo, garbage — is stable.
read_update_channel() {
    local c=""
    if [ -r "$CHANNEL_FILE" ]; then
        c=$(head -c 64 "$CHANNEL_FILE" 2>/dev/null | tr -d '[:space:]') || c=""
    fi
    if [ "$c" = "beta" ]; then echo beta; else echo stable; fi
}

# write_update_channel stable|beta — beta writes the file atomically;
# stable removes it (absence IS stable). Returns 2 on an unknown channel.
write_update_channel() {
    case "${1:-}" in
        stable)
            rm -f "$CHANNEL_FILE" "${CHANNEL_FILE}.tmp" 2>/dev/null || true
            [ ! -e "$CHANNEL_FILE" ]
            ;;
        beta)
            mkdir -p "$(dirname "$CHANNEL_FILE")" || return 1
            printf 'beta\n' > "${CHANNEL_FILE}.tmp" || return 1
            mv -f "${CHANNEL_FILE}.tmp" "$CHANNEL_FILE"
            ;;
        *)
            return 2
            ;;
    esac
}

# `update.sh channel [stable|beta]` — print the current channel, or set it
# and print the result. Human chatter goes to stderr; stdout is exactly one
# word, which is what admin.py and verify_box.sh parse.
channel_command() {
    if [ $# -eq 0 ] || [ -z "${1:-}" ]; then
        read_update_channel
        return 0
    fi
    case "$1" in
        stable|beta) ;;
        *)
            log_error "Unknown update channel '$1' (expected: stable or beta)"
            return 2
            ;;
    esac
    if ! write_update_channel "$1"; then
        log_error "Could not update $CHANNEL_FILE"
        return 1
    fi
    if [ "$1" = "beta" ]; then
        log_warn "Update channel: beta — this box will be offered pre-release builds. Never ship a unit on beta."
    else
        log "Update channel: stable"
    fi
    read_update_channel
}

# fetch_beta_release — prints ONE release object (GitHub's JSON shape) for
# the highest-precedence non-draft release in the recent-releases list, or
# emits a json_response error and returns 1. Selection is version_cmp's,
# never list order (a hotfix published after a beta must not outrank it by
# date). Python only decodes JSON here; it never compares versions.
fetch_beta_release() {
    local list
    list=$(curl -s -H "Accept: application/vnd.github.v3+json" \
        --connect-timeout 10 \
        --max-time 30 \
        "$GITHUB_RELEASES_API" 2>/dev/null) || {
        json_response "false" "Failed to connect to GitHub"
        return 1
    }
    if [ -z "$list" ]; then
        json_response "false" "Empty response from GitHub"
        return 1
    fi
    if echo "$list" | grep -q '"message".*API rate limit'; then
        json_response "false" "GitHub API rate limit exceeded. Try again later."
        return 1
    fi
    if ! command -v python3 >/dev/null 2>&1; then
        json_response "false" "The beta update channel needs python3 to read the release list"
        return 1
    fi

    local tags
    tags=$(printf '%s' "$list" | python3 -c '
import json, sys
try:
    rels = json.load(sys.stdin)
except ValueError:
    sys.exit(3)
if not isinstance(rels, list):
    sys.exit(3)
for r in rels:
    if isinstance(r, dict) and not r.get("draft") and isinstance(r.get("tag_name"), str):
        print(r["tag_name"])
' 2>/dev/null) || {
        json_response "false" "Could not parse the release list from GitHub"
        return 1
    }

    local best="" tag v
    while IFS= read -r tag; do
        [[ "$tag" == v* ]] || continue
        v="${tag#v}"
        version_valid "$v" || continue
        if [ -z "$best" ] || [ "$(version_cmp "$best" "$v")" = "-1" ]; then
            best="$v"
        fi
    done <<<"$tags"

    if [ -z "$best" ]; then
        json_response "false" "No releases found on GitHub"
        return 1
    fi
    log "Beta channel: highest release in the list is v$best"

    printf '%s' "$list" | python3 -c '
import json, sys
tag = sys.argv[1]
for r in json.load(sys.stdin):
    if isinstance(r, dict) and r.get("tag_name") == tag and not r.get("draft"):
        print(json.dumps(r, indent=2, ensure_ascii=False))
        break
' "v$best" 2>/dev/null
}

# Check for available updates from GitHub
check_update() {
    # Temporarily relax shell flags inside this function. The script's
    # top-level `set -euo pipefail` is great for the install path
    # (catches mid-extract failures so we rollback cleanly) but it's
    # too aggressive for the metadata-parsing happy-path here. The
    # function does a lot of `var=$(echo "$response" | grep -o ... |
    # head -1 | sed ...)` chains; in some shell/GitHub-payload
    # combinations a non-fatal pipe element exits non-zero (a SIGPIPE
    # from `head` killing upstream `grep` after 1 line is the usual
    # culprit) and the script silently dies before the `cat << EOF`
    # JSON output, leaving the web admin's "Check for Updates" with
    # no JSON body to parse → user sees "UPDATE_CHECK_FAILED" with
    # only the [UPDATE] log lines as the error message.
    #
    # All the parsing below already handles empty results via
    # `[ -z "$var" ]` checks and emits proper json_response error
    # output, so disabling fast-fail here doesn't hide real problems.
    set +e
    set +o pipefail

    local current_version
    current_version=$(get_current_version)

    local channel
    channel=$(read_update_channel)

    log "Current version: $current_version"
    log "Update channel: $channel"
    log "Checking GitHub for updates..."

    local response
    if [ "$channel" = "beta" ]; then
        # Beta: highest SemVer across the recent-releases list (stable AND
        # prerelease). fetch_beta_release emits its own JSON error.
        response=$(fetch_beta_release) || {
            [ -n "$response" ] && echo "$response"
            return 1
        }
    else
        # Stable: releases/latest — the exact request every updater before
        # the channel feature makes. GitHub never returns a prerelease or a
        # draft from this endpoint, which is what keeps betas off customer
        # boxes. The stable path never touches the list endpoint (pinned by
        # test_update.bats).
        response=$(curl -s -H "Accept: application/vnd.github.v3+json" \
            --connect-timeout 10 \
            --max-time 30 \
            "$GITHUB_API" 2>/dev/null) || {
            json_response "false" "Failed to connect to GitHub"
            return 1
        }
    fi

    if [ -z "$response" ]; then
        json_response "false" "Empty response from GitHub"
        return 1
    fi

    # Check for API rate limiting or errors
    if echo "$response" | grep -q '"message".*API rate limit'; then
        json_response "false" "GitHub API rate limit exceeded. Try again later."
        return 1
    fi

    # Parse version (remove 'v' prefix)
    local latest_version
    latest_version=$(echo "$response" | grep -o '"tag_name": *"v[^"]*"' | head -1 | sed 's/.*"v\([^"]*\)".*/\1/')

    if [ -z "$latest_version" ]; then
        json_response "false" "Could not parse latest version from GitHub"
        return 1
    fi

    # Only the OTA version grammar may go any further: the value lands in
    # this JSON verbatim and is what admin.py posts back to `install`.
    if ! version_valid "$latest_version"; then
        json_response "false" "Unrecognized release version on GitHub (expected X.Y.Z or X.Y.Z-beta.N)"
        return 1
    fi

    # Parse download URL for the SOURCE tarball specifically. Releases carry
    # two .tar.gz assets: the source tarball (magic-dingus-box-*.tar.gz) and
    # the pre-compiled binary (magic_dingus_box_cpp-arm64-*.tar.gz). A
    # generic first-match grep here depended on asset upload ORDER to pick
    # the right one — if the binary asset ever sorted first, install would
    # rsync --delete a binary-only tree over the whole install dir.
    local download_url
    download_url=$(echo "$response" | grep -o '"browser_download_url": *"[^"]*/magic-dingus-box-[^"/]*\.tar\.gz"' | head -1 | sed 's/.*"\(http[^"]*\)".*/\1/')

    # Fallback to GitHub's auto-generated source tarball if no release asset
    if [ -z "$download_url" ]; then
        download_url=$(echo "$response" | grep -o '"tarball_url": *"[^"]*"' | head -1 | sed 's/.*"\(http[^"]*\)".*/\1/')
    fi

    # Parse release notes (truncate to 500 chars)
    local release_notes
    release_notes=$(echo "$response" | grep -o '"body": *"[^"]*"' | head -1 | sed 's/"body": *"\(.*\)"/\1/' | head -c 500 | sed 's/"/\\"/g; s/\\n/ /g; s/\\r//g')

    # Parse published date
    local published_at
    published_at=$(echo "$response" | grep -o '"published_at": *"[^"]*"' | head -1 | sed 's/.*"\([^"]*\)".*/\1/')

    # Determine if update is available
    # version_lt never offers a downgrade on either channel: a box on
    # 1.10.1-beta.2 switched back to stable sees 1.10.0 as NOT an update and
    # simply waits for the next stable (1.10.1 or later).
    local update_available="false"
    if [ "$channel" = "stable" ] && version_is_prerelease "$latest_version"; then
        # releases/latest cannot return a prerelease unless someone
        # hand-cleared the flag on a -beta tag. Never offer it on stable.
        log_warn "releases/latest is a prerelease (v$latest_version) — not offered on the stable channel"
    elif version_lt "$current_version" "$latest_version"; then
        update_available="true"
        log "Update available: $current_version -> $latest_version"
    else
        log "Already up to date"
    fi

    # Check if backup exists (for rollback capability)
    local has_backup="false"
    if [ -d "$BACKUP_DIR" ] && [ -f "$BACKUP_DIR/VERSION" ]; then
        has_backup="true"
    fi

    # Output JSON response
    cat << EOF
{
    "ok": true,
    "data": {
        "current_version": "$current_version",
        "latest_version": "$latest_version",
        "update_available": $update_available,
        "download_url": "$download_url",
        "release_notes": "$release_notes",
        "published_at": "$published_at",
        "has_backup": $has_backup,
        "channel": "$channel"
    }
}
EOF
}

# Compose-file delivery guard.
#
# /opt/magic_dingus_box/services/docker-compose.yml is the flattened
# copy every consumer reads (magic-dingus-services.service's
# WorkingDirectory, gluetun_cascade_restart.sh, setup_services.sh).
# Releases before v1.9.7 shipped NO top-level services/ in the tarball, so
# the install rsync's --delete DELETED this file on the first OTA of every
# fielded box — while the services/.env and services/config/* excludes kept
# the directory alive, so the damage stayed invisible until something ran
# `docker compose` and got "no configuration file provided: not found"
# (exit 14).
#
# release.yml now stages services/ into the tarball so the rsync delivers
# it. This is the belt to that suspenders: the canonical copy is ALWAYS in
# the tarball at magic_dingus_box_cpp/services/, so a future packaging
# regression can never again leave a box without a compose file.
# Deliberately NOT implemented as an rsync exclude — an exclude would
# freeze a stale compose file on the box forever and break the
# OTA_UPDATE_GUARANTEES.md promise that compose fixes flow through
# automatically.
#
# Called from install AND from both rollback paths: the rollback rsyncs
# restore from a BACKUP_DIR that was captured before this repair existed,
# so on every fielded box the first rollback after the repairing OTA would
# otherwise re-delete the compose file it had just been given.
ensure_compose_file() {
    if [ -f "$INSTALL_DIR/services/docker-compose.yml" ]; then
        return 0
    fi
    if [ -f "$INSTALL_DIR/magic_dingus_box_cpp/services/docker-compose.yml" ]; then
        log_warn "services/docker-compose.yml absent — restoring from in-tree copy"
        mkdir -p "$INSTALL_DIR/services"
        cp "$INSTALL_DIR/magic_dingus_box_cpp/services/docker-compose.yml" \
           "$INSTALL_DIR/services/docker-compose.yml"
        if [ -f "$INSTALL_DIR/magic_dingus_box_cpp/services/.env.example" ]; then
            cp "$INSTALL_DIR/magic_dingus_box_cpp/services/.env.example" \
               "$INSTALL_DIR/services/.env.example"
        fi
    else
        log_error "services/docker-compose.yml is missing and no in-tree copy exists at $INSTALL_DIR/magic_dingus_box_cpp/services/docker-compose.yml — Media Browser stack cannot start"
    fi
}

# Refresh the copies of shipped files that live OUTSIDE $INSTALL_DIR.
#
# The install rsync only ever writes inside /opt/magic_dingus_box. The six
# helper scripts under /usr/local/bin and the usb0 dnsmasq conf are copied
# out of the tree by setup_services.sh / install_deps.sh at PROVISIONING
# time — and both of those only re-run from an OTA when the Phone Remote
# markers are missing, which is never on a box cut from the golden image.
# So every fix to those files was frozen at image-cut time on every fielded
# unit: most importantly the drive-absent guard in qbit_port_sync.sh, which
# is the only thing stopping torrents from writing to the OS partition when
# the movie drive is unplugged mid-seed.
#
# Bounded and REFRESH-ONLY by design:
#   - each target is copied only if it ALREADY exists, so an OTA can never
#     provision a box with a helper it was not set up with;
#   - gated on SKIP_SYSTEMCTL exactly like the network-hardening call
#     below, because CI runners have passwordless sudo and the BATS suite
#     must never write into a runner's /usr/local/bin or /etc;
#   - `sudo -n` because this runs as the unprivileged magic-dingus-web
#     user, and -n fails fast instead of hanging on a dev machine.
# systemd unit files are deliberately NOT refreshed here — see
# OTA_UPDATE_GUARANTEES.md.
refresh_out_of_tree_files() {
    if [ "$SKIP_SYSTEMCTL" = "true" ]; then
        log "SKIP: out-of-tree helper refresh (test mode)"
        return 0
    fi

    local script_dir="${INSTALL_DIR}/magic_dingus_box_cpp/scripts"
    local src dest watcher_changed=false
    # At boot (recovery oneshot) never wait on another unit's job.
    local block_flag=""
    [ "$BOOT_RECOVERY" = "true" ] && block_flag="--no-block"

    # Same name in both places.
    for helper in playback_services_pause.sh \
                  gluetun_cascade_restart.sh \
                  clear_radarr_cooldowns.py \
                  sync_qbit_password.sh \
                  auto_blocklist_stuck_warnings.py; do
        src="${script_dir}/${helper}"
        dest="/usr/local/bin/${helper}"
        if [ -f "$dest" ] && [ -f "$src" ] && ! cmp -s "$src" "$dest"; then
            if sudo -n install -m 0755 "$src" "$dest"; then
                if [ "$helper" = "gluetun_cascade_restart.sh" ]; then
                    watcher_changed=true
                fi
            else
                log_warn "could not refresh $dest"
            fi
        fi
    done

    # The cascade watcher is a long-running bash loop: it keeps executing
    # the OLD file (install(1) swaps the inode) until restarted, so a fix
    # to it would otherwise land only at the next reboot. try-restart is a
    # no-op on boxes where the watcher isn't running (no Media Browser).
    if [ "$watcher_changed" = "true" ]; then
        log "cascade watcher script changed; restarting it"
        run_systemctl $block_flag try-restart gluetun-cascade-restart.service 2>/dev/null \
            || log_warn "cascade watcher restart failed (new script applies on next restart)"
    fi

    # The one rename: qbit_port_sync.sh -> qbit-port-sync.sh.
    if [ -f /usr/local/bin/qbit-port-sync.sh ] && [ -f "${script_dir}/qbit_port_sync.sh" ]; then
        sudo -n install -m 0755 "${script_dir}/qbit_port_sync.sh" \
            /usr/local/bin/qbit-port-sync.sh \
            || log_warn "could not refresh /usr/local/bin/qbit-port-sync.sh"
    fi

    # usb0 gadget DNS. install_deps.sh is the only writer of this file and
    # it is invoked from exactly one place (the Phone Remote bootstrap
    # above), so this is the ONLY delivery path it has. Reload dnsmasq only
    # when the content actually changed.
    src="${script_dir}/data/dnsmasq-usb0.conf"
    dest="/etc/dnsmasq.d/usb0.conf"
    if [ -f "$dest" ] && [ -f "$src" ] && ! cmp -s "$src" "$dest"; then
        if sudo -n install -m 0644 "$src" "$dest"; then
            log "usb0 dnsmasq config updated; reloading dnsmasq"
            run_systemctl $block_flag reload-or-restart dnsmasq.service 2>/dev/null \
                || log_warn "dnsmasq reload failed (usb0 DNS applies on next restart)"
        else
            log_warn "could not refresh $dest"
        fi
    fi
}

# Read one property of the kiosk unit ("" when systemd cannot say).
kiosk_prop() {
    systemctl show -p "$1" --value "$KIOSK_UNIT" 2>/dev/null || true
}

# Classify ONE observation of the kiosk unit after an update started it.
# Pure (no I/O) so it is unit-tested directly. Echoes one of:
#   running     the unit is active
#   no_display  a NEW main process exited with KIOSK_EXIT_NO_DISPLAY: the
#               binary loaded and ran, there is just no TV connected
#   failed      it ran (or tried to) and is down for any other reason
#   pending     still starting; keep polling
# Args: ActiveState SubState ExecMainCode ExecMainStatus new_main(0|1)
#   new_main = 1 when ExecMainStartTimestampMonotonic changed since before
#   the start, i.e. the status fields describe THIS update's binary and not
#   a process from before the update (which may well have exited 69 too).
#   ExecMainCode 1 = CLD_EXITED (a normal exit with a status).
kiosk_start_verdict() {
    local active="$1" sub="$2" code="$3" status="$4" new_main="$5"
    if [ "$new_main" = "1" ] && [ "$active" != "active" ] \
        && [ "$code" = "1" ] && [ "$status" = "$KIOSK_EXIT_NO_DISPLAY" ]; then
        echo "no_display"
    elif [ "$active" = "active" ]; then
        echo "running"
    elif [ "$active" = "failed" ]; then
        echo "failed"
    elif [ "$new_main" = "1" ] && [ "$sub" = "auto-restart" ]; then
        echo "failed"
    elif [ "$new_main" = "1" ] && [ "$active" = "inactive" ]; then
        echo "failed"
    else
        echo "pending"
    fi
}

# Start the kiosk and decide whether this update's binary is good.
# Returns 0 when it is (running and stable, or no display connected), 1
# otherwise. Replaces a single `is-active` probe 2 s after start, which
# (a) rolled back every good update on a box with the TV off — the kiosk
# exits when no display is connected — and (b) passed a kiosk that reached
# READY and then crashed a moment later.
verify_kiosk_started() {
    if [ "$SKIP_SYSTEMCTL" = "true" ]; then
        log "SKIP: kiosk start verification (test mode)"
        return 0
    fi

    local start_before
    start_before="$(kiosk_prop ExecMainStartTimestampMonotonic)"

    run_systemctl reset-failed "$KIOSK_UNIT" 2>/dev/null || true
    # --no-block: poll ourselves with our own deadline instead of relying on
    # how a given systemd version reports a start job that hits auto-restart.
    if ! run_systemctl start --no-block "$KIOSK_UNIT" 2>/dev/null; then
        log_warn "Service start command failed, checking state..."
    fi

    local waited=0 verdict="pending" active sub code status start_now new_main
    while [ "$waited" -lt "$KIOSK_START_TIMEOUT" ]; do
        active="$(kiosk_prop ActiveState)"
        sub="$(kiosk_prop SubState)"
        code="$(kiosk_prop ExecMainCode)"
        status="$(kiosk_prop ExecMainStatus)"
        start_now="$(kiosk_prop ExecMainStartTimestampMonotonic)"
        new_main=0
        if [ -n "$start_now" ] && [ "$start_now" != "0" ] && [ "$start_now" != "$start_before" ]; then
            new_main=1
        fi
        verdict="$(kiosk_start_verdict "$active" "$sub" "$code" "$status" "$new_main")"
        [ "$verdict" = "pending" ] || break
        sleep 1
        waited=$((waited + 1))
    done

    case "$verdict" in
        no_display)
            log_warn "Kiosk started but found no connected display (exit ${KIOSK_EXIT_NO_DISPLAY}) — the update is good; it will show up as soon as a TV is connected"
            return 0
            ;;
        running)
            ;;
        *)
            log_error "Kiosk did not start (state: ${active:-?}/${sub:-?}, last exit: code ${code:-?} status ${status:-?})"
            return 1
            ;;
    esac

    # Stability window: same process, no automatic restarts, still active.
    local pid restarts
    pid="$(kiosk_prop MainPID)"
    restarts="$(kiosk_prop NRestarts)"
    sleep "$KIOSK_STABLE_SECS"
    active="$(kiosk_prop ActiveState)"
    if [ "$active" = "active" ] && [ "$(kiosk_prop MainPID)" = "$pid" ] \
        && [ "$(kiosk_prop NRestarts)" = "$restarts" ]; then
        return 0
    fi

    # It went down inside the window. A no-display exit is still a pass
    # (e.g. the TV was switched off at exactly this moment).
    if [ "$(kiosk_prop ExecMainCode)" = "1" ] \
        && [ "$(kiosk_prop ExecMainStatus)" = "$KIOSK_EXIT_NO_DISPLAY" ] \
        && [ "$active" != "active" ]; then
        log_warn "Kiosk lost its display during the stability check — accepting the update"
        return 0
    fi
    log_error "Kiosk started but did not stay up for ${KIOSK_STABLE_SECS}s (state: ${active:-?}, PID ${pid} -> $(kiosk_prop MainPID), restarts ${restarts} -> $(kiosk_prop NRestarts))"
    return 1
}

# Phone Remote: the /dev/uinput udev rule + `input` group membership the
# web service needs to create its virtual gamepad. Root work, delivered by
# setup_phone_remote_uinput.sh (setup_services.sh calls the same script).
# This used to run the WHOLE setup_services.sh, unprivileged — it died at
# Step 0 on every box (it writes /etc), so the rule never got installed and
# install_deps.sh re-ran on every OTA. setup_services.sh must never run
# from an OTA anyway: it restarts the web service out from under the
# update and brings Docker up on games-only boxes.
ensure_phone_remote_uinput() {
    if [ "$SKIP_SYSTEMCTL" = "true" ]; then
        log "SKIP: Phone Remote uinput setup (test mode)"
        return 0
    fi
    local helper="${INSTALL_DIR}/magic_dingus_box_cpp/scripts/setup_phone_remote_uinput.sh"
    if [ ! -f "$helper" ]; then
        log_warn "setup_phone_remote_uinput.sh not found in this release; skipping"
        return 0
    fi
    # The web service is restarted at the end of a successful install,
    # which is what picks up a newly added input group.
    sudo -n bash "$helper" >&2 \
        || log_warn "Phone Remote uinput setup failed (Phone Remote will be degraded)"
}

# Content Manager production server (gunicorn). Since the web admin moved
# off Werkzeug's development server, magic_dingus_box/web/serve.py runs
# gunicorn whenever python3 can import it and falls back to the old server
# when it can't — so a box without it still works, just on the old server.
# This is the ONLY way an already-fielded box ever gets the package:
# install_deps.sh is not re-run by an OTA once flask-sock exists (i.e. on
# every box cut from the golden image).
#
# Deliberately a narrow apt install of ONE package rather than
# install_deps.sh, which would apt-update, re-install the whole build
# toolchain and restart dnsmasq + the port-80 redirect mid-update. Never
# fatal: a failure (offline, dpkg lock held by unattended-upgrades) only
# means the box keeps the old server until the next update retries.
#
# Placement (install_update): AFTER the kiosk's verified start and the
# VERSION commit, BEFORE the web restart that brings the Content Manager up
# under gunicorn. It only concerns the web server, so it must not keep the
# screen dark (the kiosk is stopped earlier in the install) for however
# long a slow mirror takes, nor sit inside the window where a power cut
# would make the boot recovery roll back a good update.
#
# Two phases, because dpkg must NEVER be killed: a SIGTERM mid-unpack
# leaves "dpkg was interrupted, you must manually run dpkg --configure -a",
# which blocks every later apt run on the box.
#   1. --download-only, bounded by `timeout` — all the network time. Killing
#      a download is harmless.
#   2. the real install with --no-download and NO timeout: every .deb is
#      already in the cache, so this is a few seconds of dpkg and cannot
#      stall on the network; DPkg::Lock::Timeout bounds the lock wait.
ensure_web_server_dep() {
    if python3 -c "import gunicorn.workers.gthread" 2>/dev/null; then
        log "Content Manager: gunicorn present"
        return 0
    fi
    if [ "$SKIP_SYSTEMCTL" = "true" ]; then
        log "SKIP: python3-gunicorn install (test mode)"
        return 0
    fi
    log "Content Manager: gunicorn missing; installing python3-gunicorn"
    local fetch_secs="${MAGIC_WEB_DEP_FETCH_TIMEOUT:-300}"
    local apt=(env DEBIAN_FRONTEND=noninteractive
               apt-get -o DPkg::Lock::Timeout=60)
    local pkg=(-y --no-install-recommends python3-gunicorn)
    # Retry the download once after refreshing the package lists: a stale
    # list 404s on a package version the mirror has since replaced. Both
    # are network-only (no dpkg), so both may be bounded.
    if ! sudo -n timeout "$fetch_secs" "${apt[@]}" install --download-only "${pkg[@]}" >&2 \
       && ! { sudo -n timeout "$fetch_secs" "${apt[@]}" update >&2 \
              && sudo -n timeout "$fetch_secs" "${apt[@]}" install --download-only "${pkg[@]}" >&2; }; then
        log_warn "could not download python3-gunicorn (Content Manager keeps its built-in server until the next update)"
        return 0
    fi
    # No timeout here, on purpose (see above).
    if sudo -n "${apt[@]}" install --no-download "${pkg[@]}" >&2; then
        log "Content Manager: python3-gunicorn installed"
    else
        log_warn "could not install python3-gunicorn (Content Manager keeps its built-in server until the next update)"
    fi
    return 0
}

# Rollback hygiene for PulseAudio-as-its-own-unit (magic-dingus-audio,
# shipped 2026-10). Rolling back to a release that predates it restores an
# init_audio.sh that starts PulseAudio itself and relies on libpulse
# AUTOSPAWN to recover when it dies — but audio_service.sh's `run` wrote
# ~/.config/pulse/client.conf with `autospawn = no`, which nothing in the
# old tree ever removes, so the rolled-back box would lose that recovery.
# The unit and the kiosk drop-in that Wants= it would also linger (inert —
# the unit's ConditionPathExists= points at the now-missing audio_service.sh
# — but the restored kiosk unit should run exactly as it shipped).
#
# Runs only when the restored tree has no audio_service.sh; a rollback to
# any release that has it is a no-op. A rollback performed by an OLDER
# update.sh (e.g. v1.9.14's) cannot get this; it only reaches rollbacks run
# by this script or a later one. Never fails the rollback. The marker must
# equal CLIENT_CONF_MARKER in audio_service.sh (pinned by test_update.bats),
# so only our own file is ever removed.
#
# Test seams: MAGIC_AUDIO_HOME (the audio user's home; required in test
# mode, where nothing else is touched), MAGIC_SYSTEMD_DIR (/etc/systemd/system).
AUDIO_CLIENT_CONF_MARKER="# magic-dingus-audio: written by audio_service.sh"
retire_audio_service_if_absent() {
    if [ -f "${INSTALL_DIR}/magic_dingus_box_cpp/scripts/audio_service.sh" ]; then
        return 0
    fi

    local audio_home="${MAGIC_AUDIO_HOME:-}"
    if [ -z "$audio_home" ] && [ "$SKIP_SYSTEMCTL" != "true" ]; then
        audio_home="$(getent passwd magic 2>/dev/null | cut -d: -f6)" || true
        [ -n "$audio_home" ] || audio_home="/home/magic"
    fi
    if [ -n "$audio_home" ]; then
        local conf="${audio_home}/.config/pulse/client.conf"
        if [ -f "$conf" ] && grep -qF "$AUDIO_CLIENT_CONF_MARKER" "$conf" 2>/dev/null; then
            if rm -f "$conf" 2>/dev/null || sudo -n rm -f "$conf" 2>/dev/null; then
                log "Removed $conf (autospawn = no) — the restored release relies on PulseAudio autospawn"
            else
                log_warn "could not remove $conf (PulseAudio autospawn stays off)"
            fi
        fi
    fi

    if [ "$SKIP_SYSTEMCTL" = "true" ]; then
        log "SKIP: audio unit retirement (test mode)"
        return 0
    fi
    local sd_dir="${MAGIC_SYSTEMD_DIR:-/etc/systemd/system}"
    if [ ! -f "${sd_dir}/magic-dingus-audio.service" ]; then
        return 0
    fi
    log "Restored release predates magic-dingus-audio.service; disabling it"
    # --no-reload: both rollback paths daemon-reload before starting the
    # kiosk (and at boot nothing here may block on the manager).
    run_systemctl disable --no-reload magic-dingus-audio.service >/dev/null 2>&1 \
        || log_warn "could not disable magic-dingus-audio.service"
    local block_flag=""
    [ "$BOOT_RECOVERY" = "true" ] && block_flag="--no-block"
    run_systemctl $block_flag stop magic-dingus-audio.service 2>/dev/null \
        || log_warn "could not stop magic-dingus-audio.service"
    sudo -n rm -f "${sd_dir}/magic-dingus-box-cpp.service.d/audio-service.conf" 2>/dev/null \
        || log_warn "could not remove the kiosk's audio-service.conf drop-in"
    return 0
}

# A failed install: put the previous version back, then report the outcome
# as the job's final JSON (the web admin shows its message). Reported AFTER
# the rollback so the message says what actually happened.
fail_install() {
    local why="$1"
    # Normally already done after the compile; idempotent.
    resume_build_paused_services
    if rollback_internal; then
        json_response "false" "${why}; the previous version was restored"
    else
        json_response "false" "${why}; restoring the previous version did not finish and will be retried at the next restart or update"
    fi
}

# Install an update
install_update() {
    local target_version="$1"
    local download_url="$2"

    log "Starting update to version $target_version"

    # Validate inputs
    if [ -z "$target_version" ] || [ -z "$download_url" ]; then
        json_response "false" "Version and download URL are required"
        return 1
    fi

    # Validate the version BEFORE it is used anywhere. It is not an inert
    # label: get_binary_url() interpolates it into
    # api.github.com/repos/<repo>/releases/tags/v${version}, and curl
    # normalizes dot-segments — so "1.0.8/../../../../attacker/evil/..."
    # fetched ANOTHER repo's release metadata and installed its binary. It
    # is also written verbatim into VERSION. admin.py enforces the same
    # X.Y.Z[-beta.N] rule; this is the independent second check (defense in
    # depth — the script is also runnable by hand). VERSION_RE uses [0-9],
    # not [[:digit:]], so the locale cannot widen it.
    if ! version_valid "$target_version"; then
        json_response "false" "Invalid version (expected X.Y.Z or X.Y.Z-beta.N)"
        return 1
    fi

    # Validate URL: THIS repo on GitHub, nothing else. "Any github.com URL"
    # let a crafted request install an attacker-owned repo's tarball, and a
    # repo-prefix match alone is defeated by a /../ segment (curl -L
    # normalizes it away before the request), so dot-segments are refused
    # outright. admin.py additionally pins the exact release-asset shape.
    local repo_re="${GITHUB_REPO//./\\.}"
    if [[ "$download_url" == *"/./"* ]] || [[ "$download_url" == *"/../"* ]] \
        || [[ "$download_url" == *"/.." ]] || [[ "$download_url" == *"%"* ]] \
        || { [[ ! "$download_url" =~ ^https://(github\.com|codeload\.github\.com)/${repo_re}/ ]] \
             && [[ ! "$download_url" =~ ^https://api\.github\.com/repos/${repo_re}/ ]]; }; then
        json_response "false" "Invalid download URL (must be from the ${GITHUB_REPO} GitHub repo)"
        return 1
    fi

    # Pre-flight checks
    json_progress "preparing" 5 "Running pre-flight checks..."

    # A previous install that never finished (killed, or power lost before
    # the boot-time recovery could run) left the tree half-updated. Restore
    # it BEFORE anything else: the backup step below would otherwise
    # overwrite the only good backup with a copy of the half-installed tree.
    if [ -f "$OTA_MARKER" ]; then
        log_warn "A previous update did not finish ($(ota_marker_field from) -> $(ota_marker_field target)); restoring the previous version first"
        json_progress "preparing" 5 "Finishing an interrupted update first..."
        if ! rollback_internal; then
            json_response "false" "A previous update was interrupted and the box could not be restored; try again"
            return 1
        fi
    fi

    # Check disk space (need at least 500MB free)
    local free_space
    free_space=$(df -m "$INSTALL_DIR" | awk 'NR==2 {print $4}')
    if [ "$free_space" -lt 500 ]; then
        json_response "false" "Insufficient disk space (need 500MB, have ${free_space}MB)"
        return 1
    fi

    # Create temp directory
    rm -rf "$TEMP_DIR"
    mkdir -p "$TEMP_DIR"

    # Download update
    json_progress "downloading" 10 "Downloading update package..."
    log "Downloading from: $download_url"

    if ! retry_download "$TEMP_DIR/update.tar.gz" "$download_url" 600 "update package"; then
        json_response "false" "Download failed after multiple attempts"
        rm -rf "$TEMP_DIR"
        return 1
    fi

    json_progress "downloading" 30 "Download complete, verifying..."

    # Verify download (check file size and type)
    local file_size
    file_size=$(stat -c%s "$TEMP_DIR/update.tar.gz" 2>/dev/null || stat -f%z "$TEMP_DIR/update.tar.gz")
    if [ "$file_size" -lt 10000 ]; then
        json_response "false" "Downloaded file too small (possibly corrupted or API error)"
        rm -rf "$TEMP_DIR"
        return 1
    fi

    # Verify it's a valid gzip file
    if ! gzip -t "$TEMP_DIR/update.tar.gz" 2>/dev/null; then
        json_response "false" "Downloaded file is not a valid gzip archive"
        rm -rf "$TEMP_DIR"
        return 1
    fi

    json_progress "extracting" 35 "Extracting update package..."

    # Extract to temp location
    mkdir -p "$TEMP_DIR/extracted"
    if ! tar -xzf "$TEMP_DIR/update.tar.gz" -C "$TEMP_DIR/extracted" 2>&2; then
        json_response "false" "Failed to extract update package"
        rm -rf "$TEMP_DIR"
        return 1
    fi

    # Find the actual content directory
    # Our release tarballs extract directly (files at root), but GitHub source
    # tarballs have a wrapper directory (repo-name-version/).
    # Check for VERSION file to determine correct root.
    local content_dir
    if [ -f "$TEMP_DIR/extracted/VERSION" ]; then
        # Our release tarball - content is at extraction root
        content_dir="$TEMP_DIR/extracted"
    else
        # GitHub source tarball - look for wrapper directory
        content_dir=$(find "$TEMP_DIR/extracted" -mindepth 1 -maxdepth 1 -type d | head -1)
        if [ -z "$content_dir" ]; then
            content_dir="$TEMP_DIR/extracted"
        fi
    fi

    log "Using content directory: $content_dir"

    # Sanity-check that this is actually a source tree BEFORE the backup /
    # rsync steps run. If the wrong asset was downloaded (e.g. the ARM64
    # binary tarball, which contains a single executable), the install
    # rsync's --delete would wipe every non-excluded file in INSTALL_DIR.
    if [ ! -d "$content_dir/magic_dingus_box_cpp/src" ] || [ ! -f "$content_dir/magic_dingus_box_cpp/CMakeLists.txt" ]; then
        json_response "false" "Update package is not a source tree (wrong asset downloaded?)"
        rm -rf "$TEMP_DIR"
        return 1
    fi

    json_progress "backing_up" 45 "Creating backup of current installation..."

    # Boot-time power-loss recovery must be in place before the marker that
    # arms it is written (see ensure_ota_recovery_unit).
    ensure_ota_recovery_unit

    # Root-owned files would make the backup fail (unreadable) or the
    # install rsync exit 23 (unwritable). See normalize_tree_ownership.
    normalize_tree_ownership

    # Backup current installation
    log "Creating backup at $BACKUP_DIR"
    rm -rf "$BACKUP_DIR"

    # Create backup (exclude large user data to save space, but keep build for rollback)
    mkdir -p "$BACKUP_DIR"
    # Use --no-group --no-owner to avoid permission errors on group/owner changes
    # NOTE: We include build/ so rollback has a working binary
    # NOTE: thumbnails/, services/.env, services/config/* are also
    # excluded from backup because they're operator content that
    # didn't change between install + rollback (preserved by the
    # install rsync below); no point round-tripping them through
    # backup. Saves significant disk space on a populated kiosk.
    #
    # `--exclude 'VERSION'` is a DELIBERATE divergence from the other three
    # lists (the contract header above says they normally move together —
    # this is the documented exception, pinned by
    # tests/local/update_rsync_excludes.bats). VERSION is copied in
    # explicitly AFTER rsync returns 0, which turns $BACKUP_DIR/VERSION
    # into a completion marker: `has_backup` in check_update and both
    # rollback entry points gate on that one file. Before this, a backup
    # that died mid-transfer (near-full SD card, power cut) still wrote
    # VERSION — it is transfer entry #9 of ~4,800 — so the UI offered a
    # Rollback whose `--delete` then wiped the real installation down to
    # whatever fragment the partial backup held, and reported success.
    #
    # `/VERSION` is anchored (leading /) to the transfer root: unanchored,
    # it also matched every nested file named VERSION (e.g. the FetchContent
    # deps under build/_deps), silently leaving them out of the backup.
    #
    # Exit 24 (a source file vanished mid-copy — the kiosk and web admin
    # keep running during the backup and replace their status files by
    # atomic rename) is harmless for a backup; anything else is a failure.
    local backup_exit=0
    rsync -a --delete --no-group --no-owner \
        --include 'magic_dingus_box_cpp/data/thumbnails/systems/***' \
        --exclude '/VERSION' \
        --exclude 'magic_dingus_box_cpp/data/media/*' \
        --exclude 'magic_dingus_box_cpp/data/roms/*' \
        --exclude 'magic_dingus_box_cpp/data/saves/*' \
        --exclude 'magic_dingus_box_cpp/data/states/*' \
        --exclude 'magic_dingus_box_cpp/data/thumbnails/*' \
        --exclude 'magic_dingus_box_cpp/data/media_browser.db*' \
        --exclude 'magic_dingus_box_cpp/data/pending_revocations.txt' \
        --exclude 'magic_dingus_box_cpp/data/qbit_paused_by_kiosk' \
        --exclude 'magic_dingus_box_cpp/data/upload_temp/' \
        --exclude 'magic_dingus_box_cpp/data/screenshots/' \
        --exclude 'services/.env' \
        --exclude 'services/config/*' \
        "$INSTALL_DIR/" "$BACKUP_DIR/" 2>&2 || backup_exit=$?
    if ! rsync_exit_ok "$backup_exit"; then
        json_response "false" "Failed to create backup (rsync exit code: $backup_exit)"
        # Reclaim the space and leave nothing that looks like a backup.
        # Without this the box is left with a full card AND a partial
        # BACKUP_DIR; the update is then not cleanly retryable.
        rm -rf "$BACKUP_DIR"
        rm -rf "$TEMP_DIR"
        return 1
    fi

    # Completion marker (see the VERSION exclude above). Only written once
    # the backup rsync has fully succeeded.
    local from_version
    from_version="$(get_current_version)"
    if [ -f "$INSTALL_DIR/VERSION" ]; then
        cp "$INSTALL_DIR/VERSION" "$BACKUP_DIR/VERSION"
    else
        # A pre-versioning tree: still mark the backup complete, or no
        # rollback (and no power-loss recovery) could ever use it.
        echo "$from_version" > "$BACKUP_DIR/VERSION"
    fi

    # From here until the verified start, the install tree is in flux. The
    # marker lets the next boot (or the next install) put the backup back if
    # this process dies — power cut, OOM, a killed job — before then.
    write_ota_marker "$target_version" "$from_version"

    # The kiosk is NOT stopped here any more. It is stopped once, by
    # stop_kiosk_for_swap, immediately before its binary changes (the
    # pre-compiled install, or promote_build_dir after a source build) —
    # or before the compile when build_memory_plan says the box cannot
    # afford both. Until then the OLD kiosk keeps running against the NEW
    # tree this rsync lays down, exactly as the web admin always has
    # during an update. That is safe because:
    #   - build/ (its binary AND its WorkingDirectory) is excluded from the
    #     rsync and only replaced at the swap;
    #   - rsync replaces each file by rename, so a file the kiosk holds open
    #     keeps its old contents, and a file it opens later is a whole old
    #     or whole new file, never a half-written one;
    #   - what it reads from the tree after startup is image/font assets
    #     and system tiles; the scripts it runs live in /usr/local/bin,
    #     refreshed only after the verified start; settings, playlists and
    #     its runtime files are excluded.
    # Worst case is cosmetic: an asset the new release renamed fails to
    # load until the restart. Only the web service must never be stopped
    # here (it is our parent's service).

    json_progress "installing" 60 "Installing new files..."

    # Install new files while preserving user content
    #
    # PRESERVED (user / per-Pi content - never overwritten):
    #   - data/media/*      - User-uploaded video files
    #   - data/roms/*       - User-uploaded ROM files
    #   - data/saves/*      - Game save files (SRAM per core)
    #   - data/states/*     - Save states (per core)
    #   - data/playlists/*  - User-created playlist YAML files
    #   - data/thumbnails/* - Game cover art populated externally.
    #                          EXCEPTION: data/thumbnails/systems/ IS
    #                          updated (see --include above the
    #                          excludes) — the system tiles are tracked
    #                          in git and new consoles need their tile
    #                          to reach existing boxes via OTA.
    #                          (deploy_cpp.sh syncs these from the
    #                          operator's local thumbnails dir, NOT
    #                          tracked in git, so the GitHub release
    #                          tarball doesn't contain them; without
    #                          this exclude the rsync --delete would
    #                          wipe every thumbnail when the operator
    #                          OTA-updates)
    #   - data/device_info.json - Device identity (UUID, hostname)
    #   - data/paired_remotes.json, data/flask_secret.key,
    #     data/pairing_session.json, data/pairing_audit.log
    #                       - Phone Remote pairing state. None of these
    #                          are in the release tarball, so without
    #                          excludes the rsync --delete below would
    #                          DELETE them → every paired phone silently
    #                          unpaired on every OTA.
    #   - data/kiosk_status.json, data/text_input_queue.jsonl,
    #     data/seek_request.json
    #                       - Transient kiosk<->web runtime files;
    #                          excluded so an OTA can't yank them out
    #                          from under the running web admin.
    #   - data/screenshots/ - Debug screenshots (debug/screenshot_capture,
    #                          `touch data/screenshot_request`). Per-box,
    #                          never in the tarball, and kept out of the
    #                          backup too: they can show personal content
    #                          and are throwaway by design.
    #   - config/*          - User settings (settings.json, WiFi)
    #   - services/.env     - Per-Pi Media Browser config (WireGuard
    #                          private key, ProtonVPN credentials,
    #                          auto-generated Radarr/Prowlarr API
    #                          keys, qBit admin password). Wiping
    #                          this on update would force operators
    #                          to re-do the entire Media Browser
    #                          setup flow from scratch (drop in WG
    #                          config, wait 90 sec for setup_services,
    #                          etc.) every time they updated.
    #   - services/config/* - Per-Pi Media Browser stack state
    #                          (Radarr library DB, Prowlarr indexer
    #                          sync history, qBit fastresume + cookies,
    #                          Gluetun VPN runtime state, FlareSolverr
    #                          state). All of this is auto-generated
    #                          by the Docker stack; not in git, but
    #                          critical for the operator's working
    #                          setup.
    #   - build/*           - Local build artifacts (rebuilt fresh
    #                          from updated sources below).
    #   - data/media_browser.db* - Media Browser watch state (resume
    #                          positions, watched flags, NEW badges) for
    #                          movies AND TV, plus its -wal/-shm
    #                          sidecars. Per-box operator data, never in
    #                          the release tarball, so without this
    #                          exclude the --delete below wiped every
    #                          household's watch history on EVERY OTA —
    #                          silently, because WatchStore just
    #                          re-creates an empty schema and nothing
    #                          errors. (Added v1.9.8; WatchStore landed
    #                          in v1.9.0, after the 2026-07-30 exclude
    #                          audit that caught the same class of bug
    #                          for the pairing files.)
    #   - VERSION           - NOT stamped by the rsync. See below: it is
    #                          written only after a verified service
    #                          start, so an interrupted OTA can never
    #                          leave a kiosk-less box self-reporting
    #                          "up to date".
    #
    # UPDATED (system files - replaced with new version):
    #   - Source code (src/*)
    #   - Scripts (scripts/*) — incl. setup_services.sh, update.sh
    #   - Bezel assets (assets/bezels/*)
    #   - System assets (assets/*, data/intro/*)
    #   - Documentation (docs/*)
    #   - Build configuration (CMakeLists.txt, etc.)
    #   - services/docker-compose.yml — operator gets new VPN
    #     firewall rules, port-sync timer changes, etc.
    #     NOTE: only true as of v1.9.7. The repo has no top-level
    #     services/ dir, so until release.yml started staging one at
    #     package time this line described something that never
    #     happened — the rsync --delete below DELETED the box's
    #     compose file on its first OTA instead. See the delivery
    #     guard after the rsync.
    #
    # Use --no-group --no-owner to avoid permission errors.
    # Exit code 23 is a FAILURE (see rsync_exit_ok): it means some files
    # were not written, i.e. a half-applied update. It used to be accepted.
    # `--exclude '/VERSION'` is deliberate and applies to THIS rsync only
    # (the two rollback rsyncs restore VERSION explicitly with `cp`, see
    # the comments there). The tarball contains exactly one file named
    # VERSION (./VERSION), and rsync --delete never deletes an excluded
    # file, so the box simply keeps its current value until :919 stamps
    # the new one after a verified service start — which is what the
    # NOTE below has always promised. Before this, VERSION was transferred
    # in the first handful of files, so an OTA killed anywhere between
    # here and the service start (power cut, or magic-dingus-web being
    # restarted out from under its own child) left a box with no kiosk
    # binary that answered "up to date" and hid the Install button.
    log "Installing new files..."
    local rsync_exit=0
    #
    # `/VERSION` and `/config/*` are anchored to the transfer root (leading
    # /). Unanchored, 'config/*' matched ANY path ending in config/<name>
    # anywhere in the tree, so a release adding e.g. a src/**/config/ dir
    # would never have been delivered (or its stale files deleted); the
    # only directory meant here is the top-level config/ holding
    # settings.json. services/config/* keeps its own explicit exclude.
    rsync -av --delete --no-group --no-owner \
        --include 'magic_dingus_box_cpp/data/thumbnails/systems/***' \
        --exclude '/VERSION' \
        --exclude 'magic_dingus_box_cpp/data/media/*' \
        --exclude 'magic_dingus_box_cpp/data/roms/*' \
        --exclude 'magic_dingus_box_cpp/data/saves/*' \
        --exclude 'magic_dingus_box_cpp/data/states/*' \
        --exclude 'magic_dingus_box_cpp/data/playlists/*' \
        --exclude 'magic_dingus_box_cpp/data/thumbnails/*' \
        --exclude 'magic_dingus_box_cpp/data/device_info.json' \
        --exclude 'magic_dingus_box_cpp/data/paired_remotes.json' \
        --exclude 'magic_dingus_box_cpp/data/flask_secret.key' \
        --exclude 'magic_dingus_box_cpp/data/pairing_session.json' \
        --exclude 'magic_dingus_box_cpp/data/pairing_audit.log' \
        --exclude 'magic_dingus_box_cpp/data/kiosk_status.json' \
        --exclude 'magic_dingus_box_cpp/data/text_input_queue.jsonl' \
        --exclude 'magic_dingus_box_cpp/data/seek_request.json' \
        --exclude 'magic_dingus_box_cpp/data/media_browser.db*' \
        --exclude 'magic_dingus_box_cpp/data/pending_revocations.txt' \
        --exclude 'magic_dingus_box_cpp/data/qbit_paused_by_kiosk' \
        --exclude 'magic_dingus_box_cpp/data/upload_temp/' \
        --exclude 'magic_dingus_box_cpp/data/screenshots/' \
        --exclude '/config/*' \
        --exclude 'magic_dingus_box_cpp/build/*' \
        --exclude 'services/.env' \
        --exclude 'services/config/*' \
        "$content_dir/" "$INSTALL_DIR/" 2>&2 || rsync_exit=$?

    # Add-only playlist sync: ship NEW default playlists (e.g. a new
    # console's playlist) without ever touching a playlist file that
    # already exists on the box — operator edits to existing playlists
    # stay untouched. Two gates keep this from polluting fielded boxes:
    #
    #   1. Same-system dedupe: if ANY existing playlist on the box already
    #      covers the same emulator_system, skip. Older boxes have
    #      pre-`games_*`-naming playlists (arcade.yaml vs games_arcade.yaml)
    #      — without this gate every rename in the repo would duplicate a
    #      menu row on every fielded box.
    #   2. Content existence: only add a playlist if at least one item is
    #      actually playable on this box (a youtube item, or a local path
    #      that exists). ROMs/videos are not in the release tarball, so a
    #      new console's playlist must wait until the box has its content
    #      — the next OTA after content arrives will add it.
    #
    # Side effect (deliberate): a default playlist the operator deleted
    # comes back on the next OTA if its content is still present.
    local box_pl_dir="$INSTALL_DIR/magic_dingus_box_cpp/data/playlists"
    if [ -d "$content_dir/magic_dingus_box_cpp/data/playlists" ]; then
        mkdir -p "$box_pl_dir"
        for new_pl in "$content_dir"/magic_dingus_box_cpp/data/playlists/*.yaml; do
            [ -e "$new_pl" ] || continue
            local pl_base
            pl_base="$(basename "$new_pl")"
            local pl_dest="$box_pl_dir/$pl_base"
            [ -e "$pl_dest" ] && continue

            # Gate 1: same-system dedupe (game playlists only)
            local new_sys existing_systems
            new_sys=$(grep -m1 -iE '^[[:space:]]*emulator_system:' "$new_pl" 2>/dev/null \
                        | awk -F: '{gsub(/[ "\047]/,"",$2); print tolower($2)}') || true
            if [ -n "$new_sys" ]; then
                existing_systems=$(grep -rhiE '^[[:space:]]*emulator_system:' "$box_pl_dir"/*.yaml 2>/dev/null \
                        | awk -F: '{gsub(/[ "\047]/,"",$2); print tolower($2)}' | sort -u) || true
                if echo "$existing_systems" | grep -qx "$new_sys"; then
                    log "Skipping $pl_base (box already has a playlist for system '$new_sys')"
                    continue
                fi
            fi

            # Gate 2: at least one item must be playable on this box
            local pl_playable=0
            if grep -qiE '^[[:space:]]*source_type:[[:space:]]*["'\'']?youtube' "$new_pl" 2>/dev/null; then
                pl_playable=1
            else
                while IFS= read -r item_path; do
                    [ -n "$item_path" ] || continue
                    if [ -e "$INSTALL_DIR/magic_dingus_box_cpp/$item_path" ] || [ -e "$INSTALL_DIR/$item_path" ]; then
                        pl_playable=1
                        break
                    fi
                done < <(grep -E '^[[:space:]]*path:' "$new_pl" 2>/dev/null \
                        | sed -E 's/^[[:space:]]*path:[[:space:]]*//; s/^["'\'']//; s/["'\'']$//')
            fi
            if [ "$pl_playable" -eq 0 ]; then
                log "Skipping $pl_base (no referenced content present on this box yet)"
                continue
            fi

            cp "$new_pl" "$pl_dest"
            log "Added new default playlist: $pl_base"
        done
    fi

    # 0 / 24 only (rsync_exit_ok). 23 used to pass here, so a root-owned
    # file the rsync could not replace produced a half-applied update that
    # reported success.
    if ! rsync_exit_ok "$rsync_exit"; then
        log_error "Failed to install files (rsync exit code: $rsync_exit), attempting rollback..."
        fail_install "Failed to install files (rsync exit code: $rsync_exit)"
        return 1
    fi

    # Compose-file delivery guard (see ensure_compose_file above).
    ensure_compose_file

    # The downloaded tarball and its extracted copy are spent. Drop them
    # now rather than at the end: TEMP_DIR is under /tmp, which is a RAM
    # tmpfs on Trixie, and a source build is about to be planned against
    # MemAvailable — tmpfs pages are not "available".
    rm -rf "$TEMP_DIR/update.tar.gz" "$TEMP_DIR/extracted"

    # NOTE: the out-of-tree helper refresh (refresh_out_of_tree_files) runs
    # only AFTER the verified kiosk start, so a rolled-back update can never
    # leave /usr/local/bin helpers from the release it rolled back.

    # NOTE: VERSION file is written AFTER successful service start (see below)
    # This ensures version consistency if build fails

    # Check for pre-compiled binary (faster than compiling)
    local device_arch=$(get_device_arch)
    local binary_url=""
    local use_binary=false

    if [ "$device_arch" = "arm64" ]; then
        json_progress "checking_binary" 35 "Checking for pre-compiled binary..."
        binary_url=$(get_binary_url "$target_version")

        if [ -n "$binary_url" ]; then
            json_progress "downloading_binary" 40 "Downloading pre-compiled binary..."
            log "Found pre-compiled ARM64 binary: $binary_url"

            if retry_download "$TEMP_DIR/binary.tar.gz" "$binary_url" 300 "pre-compiled binary"; then
                if gzip -t "$TEMP_DIR/binary.tar.gz" 2>/dev/null; then
                    json_progress "installing_binary" 50 "Installing pre-compiled binary..."

                    # Guarded: under `set -e` a failed extract used to exit
                    # the whole script right here — kiosk stopped, no
                    # rollback, nothing reported.
                    mkdir -p "$TEMP_DIR/binary_extracted"
                    if ! tar -xzf "$TEMP_DIR/binary.tar.gz" -C "$TEMP_DIR/binary_extracted" 2>&2; then
                        log_warn "Pre-compiled binary archive could not be extracted, will compile from source"
                    elif [ ! -f "$TEMP_DIR/binary_extracted/magic_dingus_box_cpp" ]; then
                        log_warn "Pre-compiled binary archive has no kiosk binary, will compile from source"

                    # Verify binary architecture AND loadability. The CI
                    # binary is built on Debian Trixie; on a box running an
                    # older Debian (older glibc, libgpiod 1.x) it passes the
                    # arch check but cannot load — it would install, fail
                    # the service-start check, and roll back the ENTIRE
                    # update, leaving the box permanently unable to update
                    # via the binary path. ldd surfaces both missing
                    # libraries ("not found") and glibc version gaps
                    # ("version GLIBC_x.yz not found"); either means this
                    # box must compile from source instead.
                    elif ! chmod +x "$TEMP_DIR/binary_extracted/magic_dingus_box_cpp" \
                        || ! verify_kiosk_binary "$TEMP_DIR/binary_extracted/magic_dingus_box_cpp"; then
                        log_warn "Binary architecture mismatch, will compile from source"
                    elif ldd "$TEMP_DIR/binary_extracted/magic_dingus_box_cpp" 2>&1 | grep -q "not found"; then
                        log_warn "Pre-compiled binary needs newer system libraries than this OS provides; will compile from source"
                    elif ! { stop_kiosk_for_swap 50; install_kiosk_binary "$TEMP_DIR/binary_extracted/magic_dingus_box_cpp"; }; then
                        # The binary itself is fine — the box could not take
                        # it (disk full, I/O error). Compiling would hit the
                        # same wall, so restore the previous version.
                        log_error "Could not install the pre-compiled binary, rolling back..."
                        json_progress "error" 50 "Could not install the new program, rolling back..."
                        fail_install "Could not install the new kiosk binary"
                        return 1
                    else
                        use_binary=true
                        log "Using pre-compiled ARM64 binary"
                    fi
                else
                    log_warn "Binary download corrupt, will compile from source"
                fi
            else
                log_warn "Binary download failed, will compile from source"
            fi
        else
            log "No pre-compiled binary found for this version"
        fi
    fi

    # Only build from source if no binary available. build_from_source
    # decides from measured memory whether the current kiosk keeps the TV
    # on while this compiles (see build_memory_plan), and stops it only
    # for the swap.
    if [ "$use_binary" = false ]; then
        log "Building application from source..."

        if ! build_from_source; then
            log_error "Build failed, attempting rollback..."
            json_progress "error" 70 "Build failed, rolling back..."
            fail_install "Build failed"
            return 1
        fi
    fi

    # Phone Remote bootstrap (idempotent). The Phone Remote feature added
    # new system deps (python3-pip, python3-evdev, flask-sock) and a udev
    # rule for /dev/uinput. An existing Pi OTA-updating to a release
    # introducing these would otherwise get the new binary but no deps,
    # silently breaking the WS path. Two independent markers, two narrow
    # fixes:
    #   - flask-sock missing  -> install_deps.sh (apt + pip, via sudo)
    #   - uinput rule missing -> setup_phone_remote_uinput.sh (via sudo -n)
    # NOT setup_services.sh: it used to be run here, unprivileged, and
    # failed at its Step 0 on every box, so the rule never arrived and this
    # whole bootstrap re-ran on every OTA. setup_services.sh also restarts
    # magic-dingus-web mid-update and brings Docker up on games-only boxes.
    json_progress "phone_remote_bootstrap" 85 "Checking Phone Remote dependencies..."
    if ! python3 -c "import flask_sock" 2>/dev/null; then
        log "Phone Remote: flask-sock not installed; running install_deps.sh"
        if [[ -x "${INSTALL_DIR}/magic_dingus_box_cpp/scripts/install_deps.sh" ]]; then
            bash "${INSTALL_DIR}/magic_dingus_box_cpp/scripts/install_deps.sh" \
                || log_warn "install_deps.sh failed (Phone Remote will be degraded)"
        fi
    fi
    if [[ ! -f /etc/udev/rules.d/90-magicdingus-uinput.rules ]]; then
        log "Phone Remote: uinput udev rule missing; installing it"
        ensure_phone_remote_uinput
    else
        log "Phone Remote: uinput rule already installed"
    fi
    # python3-gunicorn (ensure_web_server_dep) is installed further down,
    # after the kiosk's verified start — it only concerns the web server.

    # RetroArch core bootstrap (idempotent). New releases can reference new
    # emulator cores (v1.7.x added N64 + Dreamcast); the cores are binary
    # .so files that are NOT in the release tarball (gitignored). Scan the
    # box's live playlists for every referenced core and run
    # install_cores.sh (apt + aarch64 core repo) only if one is missing.
    # Scanning the BOX's playlists (not the release's) is deliberate: the
    # add-only playlist sync above only adds a game playlist when its
    # content is present, so a box only fetches cores it can actually use.
    json_progress "cores_bootstrap" 87 "Checking emulator cores..."
    local cores_user="${SUDO_USER:-$(id -un)}"
    local cores_home
    # `|| true`: under set -e + pipefail a failed lookup (or no getent at
    # all, e.g. a macOS dev run) must fall through to $HOME, not abort the
    # install after the files are already in place.
    cores_home="$(getent passwd "$cores_user" 2>/dev/null | cut -d: -f6)" || true
    [ -n "$cores_home" ] || cores_home="$HOME"
    local cores_dir="${cores_home}/.config/retroarch/cores"
    local missing_core=0
    while IFS= read -r core_name; do
        [ -n "$core_name" ] || continue
        if [ ! -f "${cores_dir}/${core_name}.so" ]; then
            log "Emulator core missing: ${core_name}.so"
            missing_core=1
        fi
    done < <(grep -rhE '^[[:space:]]*emulator_core:' \
                "$INSTALL_DIR/magic_dingus_box_cpp/data/playlists/"*.yaml 2>/dev/null \
                | awk -F: '{gsub(/[ "\047]/,"",$2); print $2}' | sort -u)
    if [ "$missing_core" -eq 1 ]; then
        if [ -x "${INSTALL_DIR}/magic_dingus_box_cpp/scripts/install_cores.sh" ]; then
            log "Installing missing RetroArch cores..."
            json_progress "cores_bootstrap" 88 "Installing emulator cores..."
            bash "${INSTALL_DIR}/magic_dingus_box_cpp/scripts/install_cores.sh" \
                || log_warn "install_cores.sh failed (new-system games may not launch)"
        else
            log_warn "install_cores.sh not found; skipping core bootstrap"
        fi
    else
        log "All referenced emulator cores present"
    fi

    # Activate the any-Wi-Fi network posture delivered by this update.
    # The rsync above only lands FILES; without this call an OTA-updated
    # field unit would carry the installer forever and never run it —
    # the golden image gets the posture baked into /etc, but fielded
    # boxes only ever update. Idempotent, never drops the link, and a
    # failure must not fail the update (the dispatcher will also be
    # installed by any future provisioning run).
    #
    # `sudo -n` is load-bearing: this script runs as the web-service user
    # (magic-dingus-web is User=magic — verified live), and the installer's
    # first act is an EUID root check, so the original unprivileged `bash`
    # call failed with "must run as root" on EVERY OTA — the one delivery
    # path this hook exists for. The magic user has passwordless sudo (the
    # run_systemctl wrapper above depends on the same fact); -n makes a
    # sudo that would prompt fail instantly instead of hanging a
    # dev-machine run. Gated on SKIP_SYSTEMCTL like every other
    # system-touching call: CI runners have passwordless sudo too, and the
    # BATS suite must never rewrite a runner's NetworkManager config.
    #
    # The test is `-f`, not `-x`, and that is load-bearing: the installer
    # shipped mode 0644 in the v1.9.6/v1.9.7 tarballs, so the old `-x` gate
    # silently skipped it on EVERY OTA — the entire any-Wi-Fi hardening was
    # inert on every fielded box and the elif chain had no else, so nothing
    # was logged. The file mode is fixed in git as of v1.9.8, but line 894
    # invokes via `sudo -n bash` anyway, so the bit was never needed here.
    if [ "$SKIP_SYSTEMCTL" = "true" ]; then
        log "SKIP: network hardening (test mode)"
    elif [ -f "${INSTALL_DIR}/magic_dingus_box_cpp/scripts/setup_network_hardening.sh" ]; then
        log "Applying network hardening (IPv6 off + public-DNS-first)..."
        sudo -n bash "${INSTALL_DIR}/magic_dingus_box_cpp/scripts/setup_network_hardening.sh" \
            || log_warn "network hardening install failed (will retry on next provisioning run)"
    else
        log_warn "setup_network_hardening.sh not found in this release; skipping network hardening"
    fi

    # Converge the playback memory posture delivered by this update
    # (kiosk MemoryLow protection + zram tune + cgroup controller on the
    # kernel cmdline — the box-side half of the 2026-08-11 playback
    # stutter fix; the kiosk-side half ships in the binary). Same
    # delivery rationale, gating, and failure posture as the network
    # hardening hook above: fielded boxes only ever update, so this hook
    # is their one delivery path. The script's cmdline append logs
    # REBOOT_REQUIRED rather than rebooting — an OTA must never
    # power-cycle the box, and the posture simply arms on the next
    # natural restart (inert-but-harmless until then).
    #
    # It also installs magic-dingus-audio.service (PulseAudio in its own
    # unit) + the kiosk drop-in ordering the kiosk after it, via
    # setup_audio_service.sh. The kiosk is stopped at this point, so its
    # start below pulls the audio unit in (Wants=); a box where this
    # fails keeps sound through init_audio.sh's legacy path.
    if [ "$SKIP_SYSTEMCTL" = "true" ]; then
        log "SKIP: memory tuning (test mode)"
    elif [ -f "${INSTALL_DIR}/magic_dingus_box_cpp/scripts/setup_memory_tuning.sh" ]; then
        log "Converging playback memory posture (MemoryLow + zram + cgroup cmdline)..."
        sudo -n bash "${INSTALL_DIR}/magic_dingus_box_cpp/scripts/setup_memory_tuning.sh" \
            || log_warn "memory tuning failed (will retry on next OTA or provisioning run)"
    else
        log_warn "setup_memory_tuning.sh not found in this release; skipping memory tuning"
    fi

    # Converge the Radarr/Sonarr Custom Formats delivered by this update.
    #
    # The rsync above ships scripts/data/{radarr,sonarr}_custom_formats.json
    # to every box, but nothing reconciled them into the live services:
    # setup_services.sh owns that work and only runs at provisioning time or
    # from the Content Manager's Media Browser Configure/Reconfigure flow.
    # So a release that added a Custom Format (e.g. the 2026-08-13
    # English-audio rule) left every fielded box downloading against the OLD
    # scoring rules with the NEW fixtures sitting on disk beside them — an
    # update that does not change behaviour. This hook is the delivery path.
    #
    # Same gating and failure posture as the two hooks above: skipped in test
    # mode, only run if the script exists in this release, `sudo -n` because
    # the OTA runs as the unprivileged web-service user and the script reads
    # root-owned service config, and a failure is a WARNING that never aborts
    # the OTA. The script itself exits 0 for every "nothing to converge"
    # condition (no services/.env, no API key, service unreachable), so a box
    # with no Media Browser costs one file check.
    if [ "$SKIP_SYSTEMCTL" = "true" ]; then
        log "SKIP: Custom Format convergence (test mode)"
    elif [ -f "${INSTALL_DIR}/magic_dingus_box_cpp/scripts/converge_custom_formats.sh" ]; then
        log "Converging Radarr/Sonarr Custom Formats..."
        sudo -n bash "${INSTALL_DIR}/magic_dingus_box_cpp/scripts/converge_custom_formats.sh" \
            || log_warn "Custom Format convergence failed (will retry on next OTA or provisioning run)"
    else
        log_warn "converge_custom_formats.sh not found in this release; skipping Custom Format convergence"
    fi

    # Normally a no-op (the swap above stopped it). Load-bearing whenever a
    # path reaches here with the old kiosk still up (e.g. MAGIC_SKIP_BUILD):
    # verify_kiosk_started STARTS the unit, and starting a running unit
    # would "verify" the old process.
    stop_kiosk_for_swap 88

    json_progress "restarting_services" 90 "Restarting services..."

    # Reload systemd and start C++ app
    log "Restarting services..."
    run_systemctl daemon-reload

    # Verify the new kiosk actually runs (verify_kiosk_started): it must
    # come up AND stay up for KIOSK_STABLE_SECS, or exit with the
    # "no display connected" code — a good binary on a box whose TV is off.
    if ! verify_kiosk_started; then
        log_error "Service failed to start, rolling back..."
        json_progress "error" 90 "Service failed to start, rolling back..."
        fail_install "The updated kiosk did not start"
        return 1
    fi

    # Refresh the shipped copies that live outside $INSTALL_DIR (see
    # refresh_out_of_tree_files). Only now, after the verified start: done
    # earlier, a rollback left /usr/local/bin + dnsmasq at the NEW release
    # while the tree went back to the old one.
    refresh_out_of_tree_files

    # Only commit VERSION after successful service start
    echo "$target_version" > "$INSTALL_DIR/VERSION"
    log "VERSION updated to $target_version"

    # The update is committed; nothing left for the boot recovery to undo.
    clear_ota_marker

    # Content Manager server package. Here, not with the other bootstraps
    # before the kiosk start: the picture is already back, the update is
    # already committed, and the web restart below is what picks it up.
    # Never fails the update (see ensure_web_server_dep).
    json_progress "web_server_dep" 95 "Checking Content Manager server..."
    ensure_web_server_dep || true

    # Cleanup temp files
    rm -rf "$TEMP_DIR"

    json_progress "complete" 100 "Update complete!"
    log "Update to version $target_version complete!"

    # Final success response - output BEFORE restarting web service
    cat << EOF
{
    "ok": true,
    "stage": "complete",
    "progress": 100,
    "message": "Update complete!",
    "new_version": "$target_version"
}
EOF

    # Restart web service AFTER outputting final JSON
    # This will cause our parent process to be killed, but that's OK
    # since we've already output the completion message
    run_systemctl restart magic-dingus-web.service 2>/dev/null || true
}

# Internal rollback function (used during failed updates)
rollback_internal() {
    # Gate on the completion marker, not merely on the directory: a backup
    # that died mid-transfer has a tree but no VERSION, and restoring from
    # it with --delete would take the real installation down with it. Same
    # predicate `has_backup` uses, so the UI and the script agree.
    if [ ! -f "$BACKUP_DIR/VERSION" ]; then
        log_error "No complete backup available for rollback"
        return 1
    fi

    log "Rolling back to previous version..."

    # Only stop C++ service - don't stop web service during rollback.
    # Not at boot: the recovery oneshot is ordered BEFORE the kiosk, and a
    # stop would cancel the kiosk's queued start job.
    if [ "$BOOT_RECOVERY" != "true" ]; then
        run_systemctl stop magic-dingus-box-cpp.service 2>/dev/null || true
    fi

    # Root-owned files would make the restore exit 23 (see
    # normalize_tree_ownership).
    normalize_tree_ownership
    local restore_exit=0

    # Restore backup. Same exclude list as the install rsync — the
    # rollback should leave operator content alone, NOT roll it back
    # to whatever was in the pre-update backup. Specifically:
    #   - Anything under data/* that the install path preserved is
    #     ALREADY current; restoring from backup would do nothing or
    #     wipe newly-added content (e.g., a video the operator
    #     uploaded between the failed install and this rollback).
    #   - services/.env + services/config/* must be preserved through
    #     rollback. Otherwise: install fails partway through →
    #     rollback wipes VPN credentials → operator's Media Browser
    #     dies even though the kiosk binary rolled back successfully.
    #
    # `VERSION` is deliberately NOT excluded here (unlike the install and
    # backup rsyncs): a rollback MUST restore the old version number, and
    # the explicit `cp` below does it regardless. Do not "harmonise" the
    # lists by adding it.
    rsync -a --delete --no-group --no-owner \
        --include 'magic_dingus_box_cpp/data/thumbnails/systems/***' \
        --exclude 'magic_dingus_box_cpp/data/media/*' \
        --exclude 'magic_dingus_box_cpp/data/roms/*' \
        --exclude 'magic_dingus_box_cpp/data/saves/*' \
        --exclude 'magic_dingus_box_cpp/data/states/*' \
        --exclude 'magic_dingus_box_cpp/data/playlists/*' \
        --exclude 'magic_dingus_box_cpp/data/thumbnails/*' \
        --exclude 'magic_dingus_box_cpp/data/device_info.json' \
        --exclude 'magic_dingus_box_cpp/data/paired_remotes.json' \
        --exclude 'magic_dingus_box_cpp/data/flask_secret.key' \
        --exclude 'magic_dingus_box_cpp/data/pairing_session.json' \
        --exclude 'magic_dingus_box_cpp/data/pairing_audit.log' \
        --exclude 'magic_dingus_box_cpp/data/kiosk_status.json' \
        --exclude 'magic_dingus_box_cpp/data/text_input_queue.jsonl' \
        --exclude 'magic_dingus_box_cpp/data/seek_request.json' \
        --exclude 'magic_dingus_box_cpp/data/media_browser.db*' \
        --exclude 'magic_dingus_box_cpp/data/pending_revocations.txt' \
        --exclude 'magic_dingus_box_cpp/data/qbit_paused_by_kiosk' \
        --exclude 'magic_dingus_box_cpp/data/upload_temp/' \
        --exclude 'magic_dingus_box_cpp/data/screenshots/' \
        --exclude '/config/*' \
        --exclude 'services/.env' \
        --exclude 'services/config/*' \
        "$BACKUP_DIR/" "$INSTALL_DIR/" || restore_exit=$?

    # Explicitly restore VERSION file from backup
    if [ -f "$BACKUP_DIR/VERSION" ]; then
        cp "$BACKUP_DIR/VERSION" "$INSTALL_DIR/VERSION"
        log "VERSION restored from backup"
    fi

    # The backup predates the v1.9.7 compose repair on every fielded box,
    # so the rsync above can re-delete the compose file this update just
    # delivered. Re-run the guard against the (already restored) in-tree
    # copy.
    ensure_compose_file

    # A restored release without the audio unit gets its autospawn back
    # (see retire_audio_service_if_absent).
    retire_audio_service_if_absent

    # Put the /usr/local/bin helpers + usb0 dnsmasq conf back in step with
    # the restored tree (they are copies OF the tree; see
    # refresh_out_of_tree_files). Before this, a rollback left them at the
    # release it had just rolled back.
    refresh_out_of_tree_files

    # Restart C++ service (web service will be restarted at end of main
    # function). At boot the kiosk starts by itself right after us.
    if [ "$BOOT_RECOVERY" != "true" ]; then
        run_systemctl daemon-reload
        run_systemctl start magic-dingus-box-cpp.service 2>/dev/null || true
    fi

    local restored_version
    restored_version=$(get_current_version)

    if ! rsync_exit_ok "$restore_exit"; then
        # Keep the in-progress marker: the boot recovery (or the next
        # install) retries the restore instead of trusting this tree.
        log_error "Restore from backup was incomplete (rsync exit code: $restore_exit); will retry at the next restart or update"
        return 1
    fi

    clear_ota_marker
    log "Rolled back to version $restored_version"
    return 0
}

# Boot-time recovery from an install that never finished (`update.sh
# recover`, run by magic-dingus-ota-recovery.service before the kiosk
# starts). The unit runs the BACKUP's copy of this script: that is the
# update.sh that wrote the marker, while the copy in INSTALL_DIR may be
# from the half-installed release.
recover_interrupted_update() {
    BOOT_RECOVERY="true"

    if [ ! -f "$OTA_MARKER" ]; then
        log "No interrupted update to recover"
        return 0
    fi

    local target from current
    target="$(ota_marker_field target)"
    from="$(ota_marker_field from)"
    current="$(get_current_version)"

    # VERSION is stamped only after a verified start, so VERSION == target
    # (and target != from) means the update DID complete and only the marker
    # removal was lost. Nothing to undo.
    if [ -n "$target" ] && [ "$target" != "$from" ] && [ "$current" = "$target" ]; then
        log "Update to $target had completed; clearing the stale in-progress marker"
        clear_ota_marker
        return 0
    fi

    if [ ! -f "$BACKUP_DIR/VERSION" ]; then
        # Nothing to restore from; a marker without a backup can never be
        # acted on and would only re-run this at every boot.
        log_error "Interrupted update found (${from:-?} -> ${target:-?}) but no complete backup exists; cannot restore"
        clear_ota_marker
        return 0
    fi

    log_warn "Update ${from:-?} -> ${target:-?} was interrupted (power loss?); restoring ${from:-the previous version}"
    rollback_internal
}

# User-initiated rollback
rollback() {
    # Completion-marker gate — see rollback_internal.
    if [ ! -f "$BACKUP_DIR/VERSION" ]; then
        json_response "false" "No complete backup available for rollback"
        return 1
    fi

    local backup_version
    if [ -f "$BACKUP_DIR/VERSION" ]; then
        backup_version=$(cat "$BACKUP_DIR/VERSION" | tr -d '[:space:]')
    else
        backup_version="unknown"
    fi

    json_progress "stopping_services" 10 "Stopping C++ service..."

    # Only stop C++ service - don't stop web service during rollback
    log "Stopping C++ service..."
    run_systemctl stop magic-dingus-box-cpp.service 2>/dev/null || true
    sleep 1

    json_progress "restoring" 30 "Restoring previous version..."

    # Restore backup (preserve user data — same exclude list as
    # install + internal rollback so user/per-Pi content is left
    # alone in either direction).
    #
    # As in rollback_internal: `VERSION` is deliberately NOT excluded —
    # a rollback must restore the old version number.
    log "Restoring from backup..."
    normalize_tree_ownership
    local rsync_exit=0
    rsync -av --delete --no-group --no-owner \
        --include 'magic_dingus_box_cpp/data/thumbnails/systems/***' \
        --exclude 'magic_dingus_box_cpp/data/media/*' \
        --exclude 'magic_dingus_box_cpp/data/roms/*' \
        --exclude 'magic_dingus_box_cpp/data/saves/*' \
        --exclude 'magic_dingus_box_cpp/data/states/*' \
        --exclude 'magic_dingus_box_cpp/data/playlists/*' \
        --exclude 'magic_dingus_box_cpp/data/thumbnails/*' \
        --exclude 'magic_dingus_box_cpp/data/device_info.json' \
        --exclude 'magic_dingus_box_cpp/data/paired_remotes.json' \
        --exclude 'magic_dingus_box_cpp/data/flask_secret.key' \
        --exclude 'magic_dingus_box_cpp/data/pairing_session.json' \
        --exclude 'magic_dingus_box_cpp/data/pairing_audit.log' \
        --exclude 'magic_dingus_box_cpp/data/kiosk_status.json' \
        --exclude 'magic_dingus_box_cpp/data/text_input_queue.jsonl' \
        --exclude 'magic_dingus_box_cpp/data/seek_request.json' \
        --exclude 'magic_dingus_box_cpp/data/media_browser.db*' \
        --exclude 'magic_dingus_box_cpp/data/pending_revocations.txt' \
        --exclude 'magic_dingus_box_cpp/data/qbit_paused_by_kiosk' \
        --exclude 'magic_dingus_box_cpp/data/upload_temp/' \
        --exclude 'magic_dingus_box_cpp/data/screenshots/' \
        --exclude '/config/*' \
        --exclude 'services/.env' \
        --exclude 'services/config/*' \
        "$BACKUP_DIR/" "$INSTALL_DIR/" 2>&2 || rsync_exit=$?

    if ! rsync_exit_ok "$rsync_exit"; then
        json_response "false" "Failed to restore backup (rsync exit code: $rsync_exit)"
        return 1
    fi

    # Explicitly restore VERSION file from backup
    if [ -f "$BACKUP_DIR/VERSION" ]; then
        cp "$BACKUP_DIR/VERSION" "$INSTALL_DIR/VERSION"
        log "VERSION restored from backup"
    fi

    # See rollback_internal: the backup can predate the compose repair.
    ensure_compose_file

    # See rollback_internal: undo the audio unit's autospawn=no.
    retire_audio_service_if_absent

    # See rollback_internal: helpers outside the tree follow the tree.
    refresh_out_of_tree_files

    # A restored tree supersedes any interrupted install.
    clear_ota_marker

    json_progress "restarting_services" 80 "Restarting services..."

    # Restart C++ service
    log "Restarting C++ service..."
    run_systemctl daemon-reload
    run_systemctl start magic-dingus-box-cpp.service 2>/dev/null || true

    local restored_version
    restored_version=$(get_current_version)

    json_progress "complete" 100 "Rollback complete!"
    log "Rolled back to version $restored_version"

    cat << EOF
{
    "ok": true,
    "stage": "complete",
    "progress": 100,
    "message": "Rollback complete!",
    "version": "$restored_version"
}
EOF

    # Restart web service AFTER outputting final JSON
    run_systemctl restart magic-dingus-web.service 2>/dev/null || true
}

# Print usage
usage() {
    echo "Magic Dingus Box Update Script"
    echo ""
    echo "Usage: $0 <command> [args]"
    echo ""
    echo "Commands:"
    echo "  check                    Check for available updates"
    echo "  install <version> <url>  Install a specific version"
    echo "  rollback                 Rollback to previous version"
    echo "  recover                  Undo an install interrupted by power loss (boot-time)"
    echo "  version                  Show current version"
    echo "  channel [stable|beta]    Show (no argument) or set the update channel"
    echo ""
    echo "Examples:"
    echo "  $0 check"
    echo "  $0 channel beta          # this box gets pre-release builds (owner boxes only)"
    echo "  $0 install 1.0.1 https://github.com/.../release.tar.gz"
    echo "  $0 rollback"
    echo ""
}

# Single-flight for the mutating commands. Two installs (or an install and
# a rollback) share TEMP_DIR — the second's `rm -rf "$TEMP_DIR"` pulls the
# first's download out from under it — and race the same rsync --delete over
# the install tree. The web admin refuses a second job with 409; this lock
# covers every other way in (a manual run over ssh, a retry script). Held on
# fd 9 for the life of the script; the kernel drops it on any exit. The
# lock file sits NEXT TO TEMP_DIR, not inside it, so the rm -rf above never
# deletes it. Skipped where flock is absent (macOS dev / BATS on a Mac).
acquire_update_lock() {
    command -v flock >/dev/null 2>&1 || return 0
    local lock="${TEMP_DIR%/}.lock"
    if ! { exec 9>>"$lock"; } 2>/dev/null; then
        log_warn "Cannot open $lock — continuing without the single-flight lock"
        return 0
    fi
    if ! flock -n 9; then
        json_response "false" "Another update or rollback is already running"
        exit 1
    fi
}

# Main command dispatcher. Skipped when the file is SOURCED (the BATS suite
# sources it to unit-test individual functions).
if [ "${BASH_SOURCE[0]}" != "$0" ]; then
    return 0 2>/dev/null || true
fi

case "${1:-}" in
    check)
        check_update
        ;;
    install)
        if [ -z "${2:-}" ] || [ -z "${3:-}" ]; then
            json_response "false" "Usage: $0 install <version> <download_url>"
            exit 1
        fi
        acquire_update_lock
        # Media Browser services paused for a source build come back on
        # EVERY exit — including a killed job (SIGTERM/SIGHUP run EXIT traps
        # in bash) and a set -e abort. Idempotent with the explicit resumes.
        trap 'resume_build_paused_services' EXIT
        install_update "$2" "$3"
        ;;
    rollback)
        acquire_update_lock
        rollback
        ;;
    recover)
        # Boot-time: nothing else should hold the lock. If something does,
        # a live update/rollback owns the tree — leave it alone.
        acquire_update_lock
        recover_interrupted_update
        ;;
    version)
        echo "$(get_current_version)"
        ;;
    channel)
        # No lock: a one-word file written atomically, read once per check.
        channel_command "${2:-}"
        ;;
    --help|-h|help)
        usage
        ;;
    *)
        usage
        exit 1
        ;;
esac
