#!/bin/bash
#
# Magic Dingus Box — install PulseAudio as its own systemd unit.
#
# Installs magic-dingus-audio.service (from ../systemd/ next to this
# script) into /etc/systemd/system, enables it, and writes the kiosk
# drop-in magic-dingus-box-cpp.service.d/audio-service.conf that:
#   - Wants= + After= the audio unit, so the kiosk starts with PulseAudio
#     already answering and the default sink set;
#   - clears ExecStopPost= — kiosk units installed before 2026-10 carry
#     `ExecStopPost=/usr/bin/pulseaudio --kill`, which would otherwise
#     kill the audio unit's PulseAudio on every kiosk stop. OTA never
#     replaces the kiosk unit file itself (OTA_UPDATE_GUARANTEES.md
#     "Reaching field boxes"), so the drop-in is the fix's only route to
#     fielded boxes;
#   - sets XDG_RUNTIME_DIR from the magic user's REAL uid, replacing the
#     unit's hardcoded /run/user/1000 so the kiosk's GStreamer pulsesink
#     and apply_output()'s pactl reach the audio unit's socket on a box
#     whose magic user is not uid 1000.
#
# Never STARTS or STOPS anything. On an OTA the kiosk is stopped while
# this runs and its next start pulls the audio unit in (Wants=); on a
# deploy the kiosk restart does the same, its stop taking the old
# kiosk-cgroup PulseAudio with it; at boot both units are wanted by
# multi-user.target. Starting the unit next to a running pre-2026-10 kiosk
# would only make it wait (audio_service.sh run never kills a PulseAudio
# it did not start).
#
# Idempotent; safe to re-run. Called from setup_memory_tuning.sh — the
# root-run hook that every install path already executes, including the
# OLD update.sh on the first OTA that ships this (a field box updates
# with the update.sh it already has). Direct callers are fine too.
#
# Test seams (same conventions as setup_ota_recovery.sh):
#   MAGIC_TUNING_ROOT    - prefix for /etc (BATS fake root; skips the root check)
#   MAGIC_SKIP_SYSTEMCTL - "true" skips daemon-reload / enable
#   MAGIC_AUDIO_UID      - uid to write into the drop-in (default: id -u magic)

set -euo pipefail

TUNING_ROOT="${MAGIC_TUNING_ROOT:-}"
SKIP_SYSTEMCTL="${MAGIC_SKIP_SYSTEMCTL:-false}"
UNIT="magic-dingus-audio.service"
KIOSK_UNIT="magic-dingus-box-cpp.service"

log() { echo "[audio-service] $1"; }

if [[ -z "$TUNING_ROOT" && "$EUID" -ne 0 ]]; then
    echo "[audio-service] ERROR: must run as root (sudo)" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${SCRIPT_DIR}/../systemd/${UNIT}"
DEST="${TUNING_ROOT}/etc/systemd/system/${UNIT}"
DROPIN_DIR="${TUNING_ROOT}/etc/systemd/system/${KIOSK_UNIT}.d"
DROPIN="${DROPIN_DIR}/audio-service.conf"

if [[ ! -f "$SRC" ]]; then
    echo "[audio-service] ERROR: unit source not found: $SRC" >&2
    exit 1
fi

changed=0
if [[ ! -f "$DEST" ]] || ! cmp -s "$SRC" "$DEST"; then
    mkdir -p "$(dirname "$DEST")"
    install -m 0644 "$SRC" "$DEST"
    changed=1
    log "installed $DEST"
else
    log "$UNIT already current"
fi

AUDIO_UID="${MAGIC_AUDIO_UID:-$(id -u magic 2>/dev/null || true)}"
env_line=""
if [[ "$AUDIO_UID" =~ ^[0-9]+$ ]]; then
    env_line="Environment=XDG_RUNTIME_DIR=/run/user/${AUDIO_UID}"
else
    log "WARNING: could not resolve the magic user's uid; kiosk keeps its unit's XDG_RUNTIME_DIR"
fi

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
{
    cat <<'EOF'
# Magic Dingus Box (setup_audio_service.sh): PulseAudio runs in its own
# unit, magic-dingus-audio.service, not inside the kiosk's cgroup.
[Unit]
Wants=magic-dingus-audio.service
After=magic-dingus-audio.service

[Service]
# Pre-2026-10 kiosk units ran `pulseaudio --kill` here, which would now
# kill the audio unit's daemon on every kiosk stop. Empty = clear the list.
ExecStopPost=
EOF
    if [[ -n "$env_line" ]]; then
        echo "# The magic user's real uid (resolved at install time)."
        echo "$env_line"
    fi
} > "$tmp"

install -d -m 0755 "$DROPIN_DIR"
if [[ ! -f "$DROPIN" ]] || ! cmp -s "$tmp" "$DROPIN"; then
    install -m 0644 "$tmp" "$DROPIN"
    changed=1
    log "installed kiosk drop-in $DROPIN"
else
    log "kiosk drop-in already current"
fi

if [[ "$SKIP_SYSTEMCTL" == "true" ]]; then
    log "SKIP: daemon-reload / enable (test mode)"
    exit 0
fi

if [[ "$changed" -eq 1 ]]; then
    systemctl daemon-reload
fi
if ! systemctl is-enabled --quiet "$UNIT" 2>/dev/null; then
    systemctl enable "$UNIT" >/dev/null 2>&1
    log "enabled $UNIT"
fi
