#!/bin/bash
#
# Magic Dingus Box — install the playback memory posture.
#
# One idempotent entry point, called from deploy_cpp.sh (fresh
# provisioning), update.sh (so fielded boxes converge via OTA), and
# first_boot.sh (so clones cut from older donor images converge on
# first boot). Safe to re-run any time. Installs:
#   1. /etc/systemd/system/magic-dingus-box-cpp.service.d/memory-protect.conf
#        -> MemoryLow=512M: below this usage the kiosk's pages are
#           exempt from reclaim, so service memory pressure swaps the
#           latency-tolerant arr stack, never the video pipeline.
#        -> OOMScoreAdjust=-500: if memory still runs out, the OOM killer
#           takes a (self-restarting) container, not the kiosk.
#   1b. /etc/systemd/system/magic-dingus-box-cpp.service.d/stop-timeout.conf
#        -> TimeoutStopSec=20 so a mid-game stop lets RetroArch auto-save
#           (the OTA delivery path for a unit-file change; see step 1b)
#   1c. Media Browser unit timing drop-ins (storage-attach TimeoutStartSec,
#        smoke-test TimeoutStartSec, missing-search timer OnBootSec) — same
#        OTA delivery path as 1b
#   1d. magic-dingus-audio.service + its kiosk drop-in, via
#        setup_audio_service.sh (PulseAudio out of the kiosk's cgroup) —
#        same OTA delivery path as 1b
#   1e. restart gluetun-cascade-restart.service when it runs an older copy
#        of its script than /usr/local/bin holds, via
#        restart_stale_cascade_watcher.sh — same OTA delivery path as 1b
#   2. /etc/systemd/system/system.slice.d/mdb-memory.conf
#        -> cgroup v2 distributes protection top-down; without at least
#           as much memory.low on system.slice, (1) is silently inert.
#   3. /etc/sysctl.d/99-mdb-zram.conf
#        -> vm.page-cluster=0: zram has no seek cost to amortize, so the
#           default 8-page swap-in clusters only multiply decompression
#           work per fault.
#   4. /boot/firmware/cmdline.txt += "cgroup_enable=memory cgroup_memory=1"
#        -> the Pi firmware injects cgroup_disable=memory by default;
#           without this append the memory controller never exists and
#           (1)+(2) are inert. Takes effect on the NEXT reboot — the
#           script prints REBOOT_REQUIRED when it made this change and
#           leaves the reboot decision to the caller (an OTA must not
#           power-cycle a box mid-update; the posture simply arms on the
#           box's next natural restart).
#
# Field history: 2026-08-11, magicpi5 (Pi 5 2GB). 1080p playback froze
# then fast-forwarded to catch up: 768MB in zram swap and 300k major
# faults in the kiosk while the full download stack stayed resident
# during movies. This posture is one of the two halves of the fix (the
# other is the kiosk's own memory-gated service pause,
# platform_profile's service_quiet_mode) — with both in place the same
# box played the same episode with zero stall-watchdog hits and 978
# major faults total. Board-agnostic on purpose: the Pi 4B fleet has
# the same firmware default and the same class of symptom.
#
# Test seams (mirroring update.sh conventions):
#   MAGIC_TUNING_ROOT    - prefix for /etc and /boot paths (BATS fake root;
#                          also skips the EUID root check)
#   MAGIC_SKIP_SYSTEMCTL - "true" skips daemon-reload and sysctl apply

set -euo pipefail

TUNING_ROOT="${MAGIC_TUNING_ROOT:-}"
SKIP_SYSTEMCTL="${MAGIC_SKIP_SYSTEMCTL:-false}"

log() { echo "[memory-tuning] $1"; }

if [[ -z "$TUNING_ROOT" && "$EUID" -ne 0 ]]; then
    echo "[memory-tuning] ERROR: must run as root (sudo)" >&2
    exit 1
fi

ETC="${TUNING_ROOT}/etc"
CMDLINE="${TUNING_ROOT}/boot/firmware/cmdline.txt"
CGROUP_FLAGS="cgroup_enable=memory cgroup_memory=1"

# --- 1. kiosk service MemoryLow drop-in -------------------------------------
install -d -m 0755 "${ETC}/systemd/system/magic-dingus-box-cpp.service.d"
cat > "${ETC}/systemd/system/magic-dingus-box-cpp.service.d/memory-protect.conf" << 'EOF'
# Magic Dingus Box playback memory posture (setup_memory_tuning.sh).
# Protect the kiosk's working set from reclaim so service memory
# pressure swaps the arr stack, never the video pipeline. Inert unless
# the kernel cmdline carries cgroup_enable=memory (same installer).
[Service]
MemoryLow=512M
# When memory does run out, the kernel OOM killer must pick a container
# (restart: always brings it back) — never the kiosk, whose death drops
# the picture and the game. -500 biases the choice without making the
# kiosk unkillable; RetroArch, forked by the kiosk, inherits it.
OOMScoreAdjust=-500
EOF
log "kiosk MemoryLow + OOMScoreAdjust drop-in installed"

# --- 1b. kiosk stop timeout drop-in -----------------------------------------
# Not memory posture, but this script is the root-run hook every delivery
# path (deploy, OTA, first boot, source-box sync) already executes, and a
# drop-in overrides whatever TimeoutStopSec the installed unit carries.
# The in-tree unit already says 20, but OTA never installs unit files into
# /etc/systemd/system — field boxes keep the unit they were imaged with
# (5 s). See OTA_UPDATE_GUARANTEES.md "Reaching field boxes".
cat > "${ETC}/systemd/system/magic-dingus-box-cpp.service.d/stop-timeout.conf" << 'EOF'
# Magic Dingus Box kiosk stop timeout (setup_memory_tuning.sh).
# A stop mid-game forwards SIGTERM to RetroArch, which writes its
# auto-save state before the kiosk re-acquires DRM and exits. The old
# 5 s value SIGKILLed that save. Overrides the installed unit, which
# OTA never replaces.
[Service]
TimeoutStopSec=20
EOF
log "kiosk TimeoutStopSec=20 drop-in installed"

# --- 1c. Media Browser service unit timing drop-ins -------------------------
# Same delivery reasoning as 1b: these unit files were fixed in-tree, but
# OTA never re-installs units, so fielded boxes only get the fix as a
# drop-in from this root-run hook. Drop-ins for a unit that is not
# installed (unprovisioned box) are inert.
#   storage-attach: its script can legitimately run lock wait (120) + rm
#     (120) + up (300); the old 180 s killed it mid-`up`, leaving the
#     storage-bound containers removed and not re-created.
#   smoke-test: no start timeout at all — a wedged *arr hung it forever.
#   missing-search timer: OnBootSec must clear missing_search.py's 10 min
#     boot deferral, or the first run is always a no-op and the next comes
#     4 h later. The empty assignment resets the unit's own OnBootSec list.
install -d -m 0755 "${ETC}/systemd/system/magic-dingus-storage-attach.service.d"
cat > "${ETC}/systemd/system/magic-dingus-storage-attach.service.d/mdb-timeout.conf" << 'EOF'
# Magic Dingus Box (setup_memory_tuning.sh): cover storage_attach.sh's
# lock wait + compose rm + compose up worst case.
[Service]
TimeoutStartSec=600
EOF
install -d -m 0755 "${ETC}/systemd/system/magic-dingus-smoke-test.service.d"
cat > "${ETC}/systemd/system/magic-dingus-smoke-test.service.d/mdb-timeout.conf" << 'EOF'
# Magic Dingus Box (setup_memory_tuning.sh): a hung smoke test must fail,
# not run forever.
[Service]
TimeoutStartSec=300
EOF
install -d -m 0755 "${ETC}/systemd/system/magic-dingus-missing-search.timer.d"
cat > "${ETC}/systemd/system/magic-dingus-missing-search.timer.d/mdb-boot-delay.conf" << 'EOF'
# Magic Dingus Box (setup_memory_tuning.sh): first run after the script's
# own 10-minute boot deferral, not inside it.
[Timer]
OnBootSec=
OnBootSec=11min
EOF
log "service timing drop-ins installed (storage-attach, smoke-test, missing-search)"

# --- 1d. PulseAudio as its own unit -----------------------------------------
# Not memory posture either (though it does take PulseAudio out from under
# the kiosk's MemoryLow/OOMScoreAdjust above): this script is the one
# root-run hook every delivery path executes — including the OLD update.sh
# on the OTA that first ships the audio unit — so setup_audio_service.sh
# rides it. Placed before step 4, which can exit early. Best-effort: a
# box without the unit still gets sound (init_audio.sh's legacy path).
AUDIO_INSTALLER="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/setup_audio_service.sh"
if [[ -f "$AUDIO_INSTALLER" ]]; then
    bash "$AUDIO_INSTALLER" 2>&1 | sed 's/^/  /' \
        || log "WARNING: audio service install failed (init_audio.sh keeps starting PulseAudio itself)"
else
    log "setup_audio_service.sh not found; skipping audio service install"
fi

# --- 1e. restart a cascade watcher running a stale script -------------------
# Same "only root hook the OLD update.sh runs" reasoning as 1d: v1.9.14's
# update.sh copies a new gluetun_cascade_restart.sh into /usr/local/bin but
# never restarts the long-running watcher, and the next update sees an
# identical file and skips its restart too. The helper restarts the unit
# only when the installed script is newer than the running process; it is a
# no-op on games-only boxes and never fails. Before step 4 (early exit).
CASCADE_HELPER="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/restart_stale_cascade_watcher.sh"
if [[ "$SKIP_SYSTEMCTL" == "true" ]]; then
    log "SKIP: cascade watcher staleness check (test mode)"
elif [[ -f "$CASCADE_HELPER" ]]; then
    bash "$CASCADE_HELPER" 2>&1 | sed 's/^/  /' || true
else
    log "restart_stale_cascade_watcher.sh not found; skipping cascade watcher check"
fi

# --- 2. system.slice companion ----------------------------------------------
install -d -m 0755 "${ETC}/systemd/system/system.slice.d"
cat > "${ETC}/systemd/system/system.slice.d/mdb-memory.conf" << 'EOF'
# Companion to magic-dingus-box-cpp.service.d/memory-protect.conf.
# cgroup v2 protection distributes top-down: without slice-level
# memory.low the service-level setting is silently inert. Only the
# kiosk claims protection within system.slice, so the full amount
# flows to it.
[Slice]
MemoryLow=512M
EOF
log "system.slice protection companion installed"

# --- 3. zram swap-in tune ---------------------------------------------------
install -d -m 0755 "${ETC}/sysctl.d"
cat > "${ETC}/sysctl.d/99-mdb-zram.conf" << 'EOF'
# zram swap readahead off — 8-page fault clusters amortize disk seeks
# that zram does not have; they just multiply decompression work per
# fault (setup_memory_tuning.sh).
vm.page-cluster = 0
EOF
log "zram page-cluster sysctl installed"

# --- 4. kernel cmdline: enable the memory controller ------------------------
REBOOT_NEEDED=false
if [[ ! -f "$CMDLINE" ]]; then
    log "no ${CMDLINE} on this machine — skipping cmdline step (dev box?)"
elif grep -q "cgroup_enable=memory" "$CMDLINE"; then
    log "cmdline already enables the memory cgroup controller"
else
    # cmdline.txt MUST stay a single line or the Pi does not boot.
    # Refuse to touch a file that is already malformed, back it up once,
    # append in place, and verify the line count afterwards.
    if [[ "$(wc -l < "$CMDLINE")" -gt 1 ]]; then
        log "ERROR: ${CMDLINE} is unexpectedly multi-line; refusing to edit"
        exit 1
    fi
    if ! ls "${CMDLINE}.bak-"* >/dev/null 2>&1; then
        cp "$CMDLINE" "${CMDLINE}.bak-memtuning"
    fi
    sed -i.memtuning-tmp "1s/\$/ ${CGROUP_FLAGS}/" "$CMDLINE"
    rm -f "${CMDLINE}.memtuning-tmp"
    if [[ "$(wc -l < "$CMDLINE")" -gt 1 ]] \
        || ! grep -q "cgroup_enable=memory" "$CMDLINE"; then
        log "ERROR: cmdline edit failed verification; restoring backup"
        cp "${CMDLINE}.bak-memtuning" "$CMDLINE"
        exit 1
    fi
    log "cmdline now enables the memory cgroup controller"
    REBOOT_NEEDED=true
fi

# --- 5. apply what can apply now --------------------------------------------
if [[ "$SKIP_SYSTEMCTL" == "true" ]]; then
    log "SKIP: daemon-reload + sysctl apply (test mode)"
else
    systemctl daemon-reload 2>/dev/null \
        || log "WARNING: daemon-reload failed (drop-ins apply on next reload)"
    sysctl -p "${ETC}/sysctl.d/99-mdb-zram.conf" >/dev/null 2>&1 \
        || log "WARNING: sysctl apply failed (applies on next boot)"
fi

if [[ "$REBOOT_NEEDED" == "true" ]]; then
    # Machine-readable marker for callers (update.sh logs it; first_boot
    # relies on the box's next natural power cycle). The posture is
    # inert-but-harmless until then — identical to pre-fix behavior.
    log "REBOOT_REQUIRED: memory cgroup controller arms on next reboot"
fi

log "memory posture converged"
