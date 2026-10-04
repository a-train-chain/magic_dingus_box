#!/bin/bash
#
# Magic Dingus Box — PulseAudio lifecycle for the kiosk.
#
# PulseAudio runs as its OWN system service, magic-dingus-audio.service
# (User=magic), not inside the kiosk's unit. Until 2026-10 init_audio.sh
# (the kiosk's ExecStartPre) daemonized PulseAudio INTO the kiosk's
# cgroup, so every kiosk stop, restart or crash SIGKILLed the sound server
# ("Killing process ... (pulseaudio) with signal SIGKILL"), which can
# corrupt PulseAudio's tdb state files; every kiosk start logged "Found
# left-over process (pulseaudio) in control group"; and the kiosk's
# MemoryLow / OOMScoreAdjust silently covered PulseAudio too. Verified on
# a live Pi 5, 2026-10-03.
#
# Subcommands (one per place this runs):
#   prepare       ROOT, the audio unit's ExecStartPre=+ — linger, mask the
#                 user-session PulseAudio/PipeWire units, remove the old
#                 HDMI-hiding udev rule, make sure /run/user/<uid> is the
#                 real logind runtime dir, wait for the HDMI audio card.
#   run           magic, the audio unit's ExecStart — write the pulse
#                 config, then exec PulseAudio in the foreground so it is
#                 the unit's main process.
#   set-sink      magic, the audio unit's ExecStartPost AND (via
#                 init_audio.sh) the kiosk's ExecStartPre — wait for
#                 PulseAudio to answer, then set the default sink from
#                 settings.json audio.output. Exit 2 if PulseAudio never
#                 answered, 0 otherwise.
#   legacy-start  magic — the pre-2026-10 behaviour, for a box whose audio
#                 unit is not installed (an OTA from an update.sh too old
#                 to run setup_memory_tuning.sh, or a hand-built box):
#                 kill any PulseAudio, start one with --start (it lands in
#                 the CALLER's cgroup — the kiosk's), set the sink. Only
#                 init_audio.sh calls this, and only when the unit file is
#                 absent, so a box always gets sound.
#
# The magic user's UID is resolved at RUNTIME (`id -u`), never hardcoded:
# a box whose `magic` user is not UID 1000 used to boot silent because
# XDG_RUNTIME_DIR pointed nowhere. systemd's %U specifier cannot be used
# for this — in a system unit it is the MANAGER's uid (0), not User=.
#
# Test seams:
#   MAGIC_AUDIO_USER     (default magic)
#   MAGIC_SETTINGS_FILE  (default /opt/magic_dingus_box/config/settings.json)
#   MAGIC_PULSEAUDIO_BIN (default /usr/bin/pulseaudio)
#   MAGIC_HDMI_WAIT_SECS (default 10)
#   MAGIC_PA_WAIT_SECS   (default 10; set-sink's wait for PulseAudio)

set -uo pipefail

SELF="${BASH_SOURCE[0]}"
SCRIPT_DIR="$(cd "$(dirname "$SELF")" && pwd)"
AUDIO_USER="${MAGIC_AUDIO_USER:-magic}"
SETTINGS_FILE="${MAGIC_SETTINGS_FILE:-/opt/magic_dingus_box/config/settings.json}"
PULSEAUDIO_BIN="${MAGIC_PULSEAUDIO_BIN:-/usr/bin/pulseaudio}"
HDMI_WAIT_SECS="${MAGIC_HDMI_WAIT_SECS:-10}"
PA_WAIT_SECS="${MAGIC_PA_WAIT_SECS:-10}"
UDEV_RULE=/etc/udev/rules.d/91-pulse-ignore-unused-hdmi.rules
# Marks a client.conf as written by this script, so legacy mode only ever
# removes its own file.
CLIENT_CONF_MARKER="# magic-dingus-audio: written by audio_service.sh"

log() { echo "[audio] $*"; }

# ---------------------------------------------------------------------------
# prepare (root)
# ---------------------------------------------------------------------------
cmd_prepare() {
    if [[ "$EUID" -ne 0 ]]; then
        log "ERROR: prepare must run as root"
        return 1
    fi
    local uid runtime
    uid="$(id -u "$AUDIO_USER")" || { log "ERROR: no user $AUDIO_USER"; return 1; }
    runtime="/run/user/${uid}"

    # Mask the systemd USER-SESSION PulseAudio units — FIRST, before linger
    # or the user manager start below can launch them. Linger means
    # user@$uid runs at every boot, and the distro-default user units
    # (pulseaudio.socket + pulseaudio.service, --global enabled) socket-
    # activate a SECOND, config-less PA instance that races ours. Observed
    # live 2026-07-16: the user-session instance won the race, wedged
    # (pactl: "Connection refused"), and every gst_element_set_state
    # (PLAYING) in the kiosk failed — no video at all for the whole boot.
    # Masking (symlink to /dev/null in /etc/systemd/user) is persistent and
    # clone-safe; the check makes re-runs a silent no-op.
    if [[ "$(systemctl --global is-enabled pulseaudio.socket 2>/dev/null)" != "masked" ]]; then
        log "Masking user-session PulseAudio units (magic-dingus-audio owns PA)..."
        systemctl --global mask pulseaudio.service pulseaudio.socket || true
        stop_user_units "$uid" pulseaudio.socket pulseaudio.service
    fi

    # Mask PipeWire the same way. Stock Trixie images ship PipeWire as the
    # default audio server: pipewire-pulse holds the PulseAudio socket (so
    # our PA daemon can't bind it) and WirePlumber holds the ALSA devices
    # (so module-alsa-card can't open them) — first hit on the Pi 5 bench
    # install, 2026-07-22. No-op once masked, harmless without PipeWire.
    if systemctl --global is-enabled pipewire-pulse.socket >/dev/null 2>&1 && \
       [[ "$(systemctl --global is-enabled pipewire-pulse.socket 2>/dev/null)" != "masked" ]]; then
        log "Masking user-session PipeWire units (magic-dingus-audio owns PA)..."
        systemctl --global mask pipewire.service pipewire.socket \
            pipewire-pulse.service pipewire-pulse.socket wireplumber.service || true
        stop_user_units "$uid" pipewire.socket pipewire-pulse.socket \
            pipewire.service pipewire-pulse.service wireplumber.service
    fi

    # Linger. Without it systemd-logind tears down /run/user/$uid whenever
    # the user has no login session — on a headless kiosk, "always" —
    # wiping PulseAudio's socket directory ~20 s after the last process
    # exits; libpulse clients then fail with "Failed to create secure
    # directory /run/user/<uid>/pulse". Persistent (/var/lib/systemd/
    # linger/magic), so idempotent across reboots and re-deploys.
    if [[ "$(loginctl show-user "$AUDIO_USER" --property=Linger --value 2>/dev/null)" != "yes" ]]; then
        log "Enabling systemd linger for ${AUDIO_USER}..."
        loginctl enable-linger "$AUDIO_USER" || log "WARNING: enable-linger failed"
    fi

    # Remove the old "ignore HDMI1" udev rule. It hardcoded HDMI1 as "the
    # unused port"; with the TV on HDMI1 (observed live on a Pi 5,
    # 2026-10-03) it hid the only working sink and the box had no sound.
    # The empty port's "Failed to find a working profile" line is harmless
    # log noise; a hidden real port is not. Before PA enumerates cards.
    if [[ -f "$UDEV_RULE" ]]; then
        log "Removing udev rule that hid the HDMI1 audio port..."
        rm -f "$UDEV_RULE"
        udevadm control --reload-rules || true
        udevadm trigger --subsystem-match=sound --action=change || true
        udevadm settle --timeout=5 || true
    fi

    # /run/user/$uid must be logind's tmpfs, not a directory we made: the
    # tmpfs is mounted by user-runtime-dir@$uid when the user manager
    # starts, and mounting it AFTER PulseAudio created its socket would
    # hide that socket from every client. Starting user@$uid (what linger
    # does at boot anyway; a no-op when it already runs) pins the tmpfs.
    # Bounded so a wedged user manager can never hang the audio unit.
    if ! mountpoint -q "$runtime" 2>/dev/null; then
        timeout 20 systemctl start "user@${uid}.service" 2>/dev/null \
            || log "WARNING: could not start user@${uid}.service; creating ${runtime} directly"
    fi
    if [[ ! -d "$runtime" ]]; then
        mkdir -p "$runtime"
        chown "${AUDIO_USER}:${AUDIO_USER}" "$runtime"
        chmod 700 "$runtime"
    fi

    wait_for_hdmi_card
    return 0
}

# Stop already-running user-session units (best effort; the user manager
# may not be up yet).
stop_user_units() {
    local uid="$1"; shift
    timeout 10 runuser -u "$AUDIO_USER" -- env XDG_RUNTIME_DIR="/run/user/${uid}" \
        systemctl --user stop "$@" >/dev/null 2>&1 || true
}

# Wait for the HDMI audio card so PulseAudio enumerates it at startup
# instead of coming up with only auto_null.
wait_for_hdmi_card() {
    local waited=0
    log "Waiting for HDMI audio card..."
    while [[ "$waited" -lt "$HDMI_WAIT_SECS" ]]; do
        if aplay -l 2>/dev/null | grep -q "vc4hdmi"; then
            log "HDMI audio card detected after ${waited}s"
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    log "Warning: HDMI audio card not detected after ${HDMI_WAIT_SECS}s, proceeding anyway"
    return 0
}

# ---------------------------------------------------------------------------
# PulseAudio per-user config (written as the audio user, before PA starts)
# ---------------------------------------------------------------------------
# $1 = "service" or "legacy"
write_pulse_config() {
    local mode="$1"
    local dir="${HOME}/.config/pulse"
    mkdir -p "$dir"

    # restore_device=false keeps streams following the default sink we set,
    # instead of PA remembering a per-stream device from a previous boot.
    cat > "${dir}/default.pa" <<'PAEOF'
.include /etc/pulse/default.pa
load-module module-stream-restore restore_device=false
PAEOF

    # Never idle-exit: PA used to shut down 20 s after the last client left
    # (e.g. between the intro video and the next movie), and the next
    # GStreamer pipeline then failed to reach PLAYING.
    cat > "${dir}/daemon.conf" <<'PAEOF'
exit-idle-time = -1
PAEOF

    if [[ "$mode" == "service" ]]; then
        # systemd owns starting PulseAudio. A client (the kiosk's
        # pulsesink, a pactl call) that found PA down during a restart
        # would otherwise AUTOSPAWN a private instance inside its own
        # cgroup — recreating exactly the kiosk-cgroup daemon this unit
        # exists to remove. Restart=always brings the real one back in ~2 s.
        printf '%s\nautospawn = no\n' "$CLIENT_CONF_MARKER" > "${dir}/client.conf"
    elif [[ -f "${dir}/client.conf" ]] && grep -qF "$CLIENT_CONF_MARKER" "${dir}/client.conf"; then
        # Legacy mode has no supervisor; autospawn is its only recovery.
        rm -f "${dir}/client.conf"
    fi
}

pulse_runtime_dir() {
    echo "/run/user/$(id -u)"
}

# ---------------------------------------------------------------------------
# run (audio user) — becomes the PulseAudio process
# ---------------------------------------------------------------------------
cmd_run() {
    if [[ "$EUID" -eq 0 ]]; then
        log "ERROR: run must not be root (the unit sets User=${AUDIO_USER})"
        return 1
    fi
    XDG_RUNTIME_DIR="$(pulse_runtime_dir)"
    export XDG_RUNTIME_DIR

    write_pulse_config service

    # A PulseAudio this unit did not start can already be serving: a box
    # mid-transition (the old kiosk still running with its own PA inside
    # its cgroup) or a rollback to a release whose init_audio.sh starts
    # one. Never kill it — that is the kill-and-restart dance this unit
    # replaces. Wait until it exits (the kiosk stopping takes it along),
    # then take over. Audio keeps working the whole time.
    local logged=false pid
    while pid="$(pgrep -u "$(id -u)" -x pulseaudio 2>/dev/null | head -1)" && [[ -n "$pid" ]]; do
        if [[ "$logged" == false ]]; then
            log "PulseAudio pid ${pid} was started outside this unit; waiting for it to exit"
            logged=true
        fi
        sleep 2
    done

    # No PulseAudio is running, so these are stale (left by a SIGKILLed
    # daemon) and would fail the bind with "Address already in use".
    rm -f "${XDG_RUNTIME_DIR}/pulse/native" "${XDG_RUNTIME_DIR}/pulse/pid" 2>/dev/null

    # Foreground (--daemonize=no) so PulseAudio IS the unit's main
    # process: systemd supervises and restarts it, and it lives in this
    # unit's cgroup, not the kiosk's. stderr goes to the journal under
    # this unit (StandardError=journal) — chosen over --log-target=journal,
    # which only exists when PulseAudio was built with journal support.
    log "Starting PulseAudio (XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR})"
    exec "$PULSEAUDIO_BIN" --daemonize=no --exit-idle-time=-1 --log-target=stderr
}

# ---------------------------------------------------------------------------
# set-sink (audio user)
# ---------------------------------------------------------------------------
read_audio_output() {
    local out="auto"
    if [[ -f "$SETTINGS_FILE" ]]; then
        out="$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d.get("audio",{}).get("output","auto"))' \
            "$SETTINGS_FILE" 2>/dev/null)" || out="auto"
        [[ -n "$out" ]] || out="auto"
    fi
    echo "$out"
}

cmd_set_sink() {
    XDG_RUNTIME_DIR="$(pulse_runtime_dir)"
    export XDG_RUNTIME_DIR

    local i ready=false
    for ((i = 0; i < PA_WAIT_SECS * 2; i++)); do
        if pactl info >/dev/null 2>&1; then
            ready=true
            break
        fi
        sleep 0.5
    done
    if [[ "$ready" != true ]]; then
        log "Warning: PulseAudio not answering after ${PA_WAIT_SECS}s"
        return 2
    fi

    # Resolve now that PulseAudio has enumerated the actual cards on this
    # board (Pi 4 vs Pi 5 vs USB DAC) — sink names embed SoC bus addresses
    # that differ between boards. A saved "headphone" on a board with no
    # analog jack degrades to HDMI rather than silence.
    local want sink
    want="$(read_audio_output)"
    log "Audio output setting: ${want}"
    sink="$(pactl list short sinks 2>/dev/null | "$SCRIPT_DIR/resolve_audio_sink.sh" "$want" || true)"
    if [[ -n "$sink" ]]; then
        pactl set-default-sink "$sink" || log "Warning: could not set default sink ${sink}"
        log "PulseAudio default sink configured: ${sink}"
    else
        log "Warning: no usable audio sink found; leaving PulseAudio default unchanged"
    fi
    log "PulseAudio ready, default sink: $(pactl get-default-sink 2>/dev/null || echo unknown)"
    return 0
}

# ---------------------------------------------------------------------------
# legacy-start (audio user) — only when the audio unit is not installed
# ---------------------------------------------------------------------------
cmd_legacy_start() {
    log "magic-dingus-audio.service is not installed; starting PulseAudio the legacy way"
    # Root-level preparation (linger, masks, udev rule, runtime dir, HDMI
    # wait). The magic user has passwordless sudo; -n never prompts.
    if [[ "$EUID" -eq 0 ]]; then
        cmd_prepare || true
    else
        sudo -n bash "$SELF" prepare || log "WARNING: audio preparation failed"
    fi

    sudo -n killall pulseaudio 2>/dev/null || true
    sleep 1

    XDG_RUNTIME_DIR="$(pulse_runtime_dir)"
    export XDG_RUNTIME_DIR
    rm -f "${XDG_RUNTIME_DIR}/pulse/native" "${XDG_RUNTIME_DIR}/pulse/pid" 2>/dev/null

    write_pulse_config legacy

    # --start daemonizes into the caller's cgroup (the kiosk's).
    "$PULSEAUDIO_BIN" --start --exit-idle-time=-1 --log-target=syslog \
        || log "WARNING: pulseaudio --start failed"

    cmd_set_sink || true
    return 0
}

case "${1:-}" in
    prepare)      cmd_prepare ;;
    run)          cmd_run ;;
    set-sink)     cmd_set_sink ;;
    legacy-start) cmd_legacy_start ;;
    *)
        echo "usage: $0 {prepare|run|set-sink|legacy-start}" >&2
        exit 64
        ;;
esac
