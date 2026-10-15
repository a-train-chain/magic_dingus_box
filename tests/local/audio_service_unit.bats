#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# PulseAudio as its own systemd unit (magic-dingus-audio.service) instead of
# a daemon init_audio.sh forked INTO the kiosk's cgroup, where every kiosk
# stop/restart/crash SIGKILLed it (verified on a live Pi 5, 2026-10-03).
#
# Covers: the unit's content, the kiosk unit's ordering, the installer
# (fake root), init_audio.sh's two branches (unit present -> never starts
# PulseAudio; unit absent -> legacy start so a box always has sound),
# audio_service.sh run/set-sink with stubbed binaries, that every install
# path reaches the installer, and shellcheck cleanliness.

SCRIPTS="$CPP_DIR/scripts"
AUDIO_UNIT="$CPP_DIR/systemd/magic-dingus-audio.service"
KIOSK_UNIT="$CPP_DIR/systemd/magic-dingus-box-cpp.service"
GOLDEN="$TESTS_REPO_ROOT/scripts/golden_image"

PI5_SINKS='0	alsa_output.platform-107c701400.hdmi.hdmi-stereo	module-alsa-card.c	s16le 2ch 48000Hz	IDLE'

setup() {
    T="$(mktemp -d)"
    STUBS="$T/bin"
    CALLS="$T/calls.log"
    mkdir -p "$STUBS" "$T/home"
    : > "$CALLS"

    # pactl: answers unless $T/pa_down exists; logs mutating calls.
    cat > "$STUBS/pactl" <<EOF
#!/bin/bash
echo "pactl \$*" >> "$CALLS"
[ -e "$T/pa_down" ] && exit 1
case "\$1 \$2" in
    "list short") printf '%s\n' "$PI5_SINKS" ;;
    "get-default-sink ") echo alsa_output.platform-107c701400.hdmi.hdmi-stereo ;;
esac
exit 0
EOF
    # pulseaudio / sudo / killall / pgrep: record, never act.
    for b in pulseaudio sudo killall; do
        printf '#!/bin/bash\necho "%s $*" >> "%s"\nexit 0\n' "$b" "$CALLS" > "$STUBS/$b"
    done
    printf '#!/bin/bash\nexit 1\n' > "$STUBS/pgrep"
    chmod +x "$STUBS"/*

    printf '{"audio": {"output": "hdmi"}}\n' > "$T/settings.json"
    export MAGIC_SETTINGS_FILE="$T/settings.json"
    export MAGIC_PULSEAUDIO_BIN="$STUBS/pulseaudio"
    export MAGIC_PA_WAIT_SECS=1
    export HOME="$T/home"
    export PATH="$STUBS:$PATH"
}

teardown() {
    rm -rf "$T"
}

# `! cmd` does not fail a bats test unless it is the last line (bats runs
# tests under set -e, which ignores negated commands) — so assert absence
# through a function that returns 1.
refute() {
    if "$@"; then
        echo "unexpectedly succeeded: $*"
        return 1
    fi
}

# --- the unit ---------------------------------------------------------------

@test "audio unit runs PulseAudio as magic, always restarting, before the kiosk" {
    grep -qx 'User=magic' "$AUDIO_UNIT"
    grep -qx 'Group=magic' "$AUDIO_UNIT"
    grep -qx 'Restart=always' "$AUDIO_UNIT"
    grep -qx 'StartLimitIntervalSec=0' "$AUDIO_UNIT"
    grep -q '^Before=.*magic-dingus-box-cpp.service' "$AUDIO_UNIT"
    grep -qx 'WantedBy=multi-user.target' "$AUDIO_UNIT"
}

@test "audio unit: root prepare, foreground run, best-effort sink step" {
    grep -q '^ExecStartPre=+/bin/bash .*/scripts/audio_service.sh prepare$' "$AUDIO_UNIT"
    grep -q '^ExecStart=/bin/bash .*/scripts/audio_service.sh run$' "$AUDIO_UNIT"
    grep -q '^ExecStartPost=-/bin/bash .*/scripts/audio_service.sh set-sink$' "$AUDIO_UNIT"
    grep -q '^ConditionPathExists=.*/scripts/audio_service.sh$' "$AUDIO_UNIT"
}

@test "audio unit never hardcodes the uid (no /run/user/1000, no %U)" {
    refute grep -Eq '^[^#].*(/run/user/|%U)' "$AUDIO_UNIT"
}

@test "kiosk unit wants + orders after the audio unit and no longer kills PulseAudio" {
    grep -q '^Wants=.*magic-dingus-audio.service' "$KIOSK_UNIT"
    grep -q '^After=.*magic-dingus-audio.service' "$KIOSK_UNIT"
    refute grep -q '^Requires=.*magic-dingus-audio' "$KIOSK_UNIT"
    refute grep -q '^ExecStopPost=.*pulseaudio' "$KIOSK_UNIT"
}

# --- installer --------------------------------------------------------------

@test "setup_audio_service.sh installs the unit and the kiosk drop-in (fake root)" {
    export MAGIC_TUNING_ROOT="$T/root" MAGIC_SKIP_SYSTEMCTL=true MAGIC_AUDIO_UID=1001
    run bash "$SCRIPTS/setup_audio_service.sh"
    [ "$status" -eq 0 ]
    cmp -s "$AUDIO_UNIT" "$T/root/etc/systemd/system/magic-dingus-audio.service"
    d="$T/root/etc/systemd/system/magic-dingus-box-cpp.service.d/audio-service.conf"
    grep -qx 'Wants=magic-dingus-audio.service' "$d"
    grep -qx 'After=magic-dingus-audio.service' "$d"
    # Empty assignment clears the old units' `pulseaudio --kill`.
    grep -qx 'ExecStopPost=' "$d"
    grep -qx 'Environment=XDG_RUNTIME_DIR=/run/user/1001' "$d"
}

@test "setup_audio_service.sh is idempotent" {
    export MAGIC_TUNING_ROOT="$T/root" MAGIC_SKIP_SYSTEMCTL=true MAGIC_AUDIO_UID=1000
    bash "$SCRIPTS/setup_audio_service.sh"
    run bash "$SCRIPTS/setup_audio_service.sh"
    [ "$status" -eq 0 ]
    [[ "$output" == *"already current"* ]]
    [[ "$output" == *"kiosk drop-in already current"* ]]
}

@test "setup_memory_tuning.sh (the OTA root hook) installs the audio unit" {
    export MAGIC_TUNING_ROOT="$T/root" MAGIC_SKIP_SYSTEMCTL=true MAGIC_AUDIO_UID=1000
    mkdir -p "$T/root/boot/firmware"
    printf '%s' "console=tty1 root=PARTUUID=dead-02 rootwait" > "$T/root/boot/firmware/cmdline.txt"
    run bash "$SCRIPTS/setup_memory_tuning.sh"
    [ "$status" -eq 0 ]
    [ -f "$T/root/etc/systemd/system/magic-dingus-audio.service" ]
    [ -f "$T/root/etc/systemd/system/magic-dingus-box-cpp.service.d/audio-service.conf" ]
}

# --- every install path reaches the installer -------------------------------

@test "every install path runs setup_memory_tuning.sh, which runs setup_audio_service.sh" {
    grep -q 'setup_audio_service.sh' "$SCRIPTS/setup_memory_tuning.sh"
    grep -q 'scripts/setup_memory_tuning.sh' "$SCRIPTS/update.sh"
    grep -q 'scripts/setup_memory_tuning.sh' "$SCRIPTS/deploy_cpp.sh"
    grep -q 'scripts/setup_memory_tuning.sh' "$GOLDEN/first_boot.sh"
    grep -q 'scripts/setup_memory_tuning.sh' "$GOLDEN/sync_source_box.sh"
}

@test "sync_source_box.sh pushes every audio file BEFORE running the installer" {
    f="$GOLDEN/sync_source_box.sh"
    run_line="$(grep -n 'sudo /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/setup_memory_tuning.sh' "$f" | head -1 | cut -d: -f1)"
    for src in scripts/audio_service.sh scripts/init_audio.sh scripts/setup_audio_service.sh \
               scripts/resolve_audio_sink.sh systemd/magic-dingus-audio.service; do
        line="$(grep -n "^push magic_dingus_box_cpp/${src} " "$f" | head -1 | cut -d: -f1)"
        [ -n "$line" ] || { echo "not pushed: $src"; false; }
        [ "$line" -lt "$run_line" ] || { echo "pushed after the installer runs: $src"; false; }
    done
    grep -q 'is-enabled --quiet magic-dingus-audio.service' "$f"
}

@test "clone prep quiesces the audio unit; restore starts it" {
    grep -q 'systemctl stop magic-dingus-audio.service' "$GOLDEN/prepare_for_cloning.sh"
    grep -q 'systemctl start magic-dingus-audio.service' "$GOLDEN/restore_after_cloning.sh"
}

# --- init_audio.sh (kiosk ExecStartPre) -------------------------------------

@test "init_audio.sh with the unit installed never starts or kills PulseAudio" {
    touch "$T/unit"
    MAGIC_AUDIO_UNIT_FILE="$T/unit" run bash "$SCRIPTS/init_audio.sh"
    [ "$status" -eq 0 ]
    refute grep -q '^pulseaudio' "$CALLS"
    refute grep -q 'killall' "$CALLS"
    grep -qx 'pactl set-default-sink alsa_output.platform-107c701400.hdmi.hdmi-stereo' "$CALLS"
}

@test "init_audio.sh with the unit installed but PA down asks systemd, still exits 0" {
    touch "$T/unit" "$T/pa_down"
    MAGIC_AUDIO_UNIT_FILE="$T/unit" run bash "$SCRIPTS/init_audio.sh"
    [ "$status" -eq 0 ]
    grep -qx 'sudo -n systemctl start --no-block magic-dingus-audio.service' "$CALLS"
    refute grep -q '^pulseaudio' "$CALLS"
}

@test "init_audio.sh: the retry after asking systemd uses a SHORT PulseAudio wait" {
    # A dead PulseAudio must not double the dark screen: the first set-sink
    # waits MAGIC_PA_WAIT_SECS, the retry only MAGIC_INIT_AUDIO_RETRY_WAIT_SECS
    # (default 3 s), whatever MAGIC_PA_WAIT_SECS says.
    touch "$T/unit" "$T/pa_down"
    MAGIC_AUDIO_UNIT_FILE="$T/unit" run bash "$SCRIPTS/init_audio.sh"
    [ "$status" -eq 0 ]
    [[ "$output" == *"not answering after 1s"* ]]
    [[ "$output" == *"not answering after 3s"* ]]
    grep -q 'MAGIC_PA_WAIT_SECS="${MAGIC_INIT_AUDIO_RETRY_WAIT_SECS:-3}"' "$SCRIPTS/init_audio.sh"
    # Default must stay well under the first wait's 10 s.
    default="$(sed -n 's/.*MAGIC_INIT_AUDIO_RETRY_WAIT_SECS:-\([0-9]*\)}.*/\1/p' "$SCRIPTS/init_audio.sh" | head -1)"
    [ "$default" -le 5 ]
}

@test "init_audio.sh WITHOUT the unit falls back to starting PulseAudio itself" {
    [[ $EUID -ne 0 ]] || skip "as root, legacy-start runs prepare directly instead of via sudo"
    MAGIC_AUDIO_UNIT_FILE="$T/missing" run bash "$SCRIPTS/init_audio.sh"
    [ "$status" -eq 0 ]
    grep -q '^pulseaudio --start' "$CALLS"
    grep -q 'sudo -n bash .*audio_service.sh prepare' "$CALLS"
    grep -qx 'pactl set-default-sink alsa_output.platform-107c701400.hdmi.hdmi-stereo' "$CALLS"
}

@test "a masked audio unit (symlink to /dev/null) counts as not installed" {
    ln -s /dev/null "$T/masked"
    MAGIC_AUDIO_UNIT_FILE="$T/masked" run bash "$SCRIPTS/init_audio.sh"
    [ "$status" -eq 0 ]
    grep -q '^pulseaudio --start' "$CALLS"
}

# --- audio_service.sh -------------------------------------------------------

@test "run: writes pulse config with autospawn off, execs PulseAudio in the foreground" {
    [[ $EUID -ne 0 ]] || skip "run refuses root by design (the unit sets User=magic)"
    run bash "$SCRIPTS/audio_service.sh" run
    [ "$status" -eq 0 ]
    grep -q -- '^pulseaudio --daemonize=no --exit-idle-time=-1' "$CALLS"
    grep -qx 'exit-idle-time = -1' "$HOME/.config/pulse/daemon.conf"
    grep -q 'restore_device=false' "$HOME/.config/pulse/default.pa"
    grep -qx 'autospawn = no' "$HOME/.config/pulse/client.conf"
}

@test "run: waits for a PulseAudio it did not start instead of killing it" {
    [[ $EUID -ne 0 ]] || skip "run refuses root by design (the unit sets User=magic)"
    # pgrep reports a foreign PA once, then none.
    cat > "$STUBS/pgrep" <<EOF
#!/bin/bash
if [ ! -e "$T/pgrep_seen" ]; then touch "$T/pgrep_seen"; echo 4242; exit 0; fi
exit 1
EOF
    chmod +x "$STUBS/pgrep"
    run bash "$SCRIPTS/audio_service.sh" run
    [ "$status" -eq 0 ]
    [[ "$output" == *"pid 4242 was started outside this unit"* ]]
    refute grep -q 'killall' "$CALLS"
    grep -q -- '^pulseaudio --daemonize=no' "$CALLS"
}

@test "legacy-start removes only our own client.conf (autospawn is its recovery)" {
    mkdir -p "$HOME/.config/pulse"
    printf '# magic-dingus-audio: written by audio_service.sh\nautospawn = no\n' > "$HOME/.config/pulse/client.conf"
    run bash "$SCRIPTS/audio_service.sh" legacy-start
    [ "$status" -eq 0 ]
    [ ! -f "$HOME/.config/pulse/client.conf" ]

    printf 'autospawn = yes\n' > "$HOME/.config/pulse/client.conf"
    run bash "$SCRIPTS/audio_service.sh" legacy-start
    [ -f "$HOME/.config/pulse/client.conf" ]
}

@test "set-sink exits 2 when PulseAudio never answers" {
    touch "$T/pa_down"
    run bash "$SCRIPTS/audio_service.sh" set-sink
    [ "$status" -eq 2 ]
    refute grep -q 'set-default-sink' "$CALLS"
}

@test "prepare refuses to run unprivileged" {
    [ "$EUID" -ne 0 ] || skip "running as root"
    run bash "$SCRIPTS/audio_service.sh" prepare
    [ "$status" -ne 0 ]
}

# --- lint -------------------------------------------------------------------

@test "shellcheck clean: audio scripts and every changed install script" {
    command -v shellcheck >/dev/null 2>&1 || skip "shellcheck not installed"
    run shellcheck "$SCRIPTS/audio_service.sh" "$SCRIPTS/setup_audio_service.sh" "$SCRIPTS/init_audio.sh"
    echo "$output"
    [ "$status" -eq 0 ]
    run shellcheck -S error "$SCRIPTS/setup_memory_tuning.sh" "$SCRIPTS/verify_box.sh" \
        "$SCRIPTS/deploy_cpp.sh" "$SCRIPTS/update.sh" \
        "$GOLDEN/sync_source_box.sh" "$GOLDEN/prepare_for_cloning.sh" \
        "$GOLDEN/restore_after_cloning.sh" "$GOLDEN/prepare_golden_image.sh" \
        "$GOLDEN/first_boot.sh"
    echo "$output"
    [ "$status" -eq 0 ]
}
