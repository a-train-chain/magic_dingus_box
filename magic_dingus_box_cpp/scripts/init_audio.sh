#!/bin/bash
# Wait for HDMI audio card to be available before starting PulseAudio
# This prevents race conditions where PulseAudio starts before HDMI is initialized

MAX_WAIT=10
WAITED=0

# Resolve the magic user's UID once. Pi OS's first-user creation flow puts
# the operator at UID 1000 by default, but a Pi cloned from a distro that
# created a `pi` user first (or any non-default install order) can land
# `magic` at UID 1001+. Hardcoding 1000 in the runtime-dir paths used to
# break audio silently in those cases — XDG_RUNTIME_DIR points nowhere,
# PulseAudio fails to start, kiosk boots to black screen with no audio.
MAGIC_UID="$(id -u magic)"
RUNTIME_DIR="/run/user/${MAGIC_UID}"

# Enable systemd linger for the magic user. Without linger, systemd-logind
# tears down /run/user/$UID whenever there's no active login session for
# the user — which on a headless kiosk is "always". The teardown happens
# every ~20s after the last process exits, wiping pulseaudio's per-user
# state directory and causing libpulse clients (GStreamer's pulsesink
# inside the kiosk) to fail with "Failed to create secure directory
# /run/user/1000/pulse" the next time they try to connect. Symptom from
# the user's perspective: video plays once after boot, then silently
# refuses to play any subsequent file.
#
# Linger is a one-time persistent setting (stored in
# /var/lib/systemd/linger/magic) so the enable call below is idempotent
# across reboots and re-deploys.
if [ "$(loginctl show-user magic --property=Linger --value 2>/dev/null)" != "yes" ]; then
    echo "Enabling systemd linger for magic user (persistent /run/user/1000)..."
    sudo loginctl enable-linger magic
fi

# Suppress PulseAudio warnings for the unused HDMI controller. The Pi 4 exposes
# two HDMI controllers via DT (vc4-hdmi-0, vc4-hdmi-1) but the kiosk only uses
# HDMI0. The unused vc4-hdmi-1 has no audio profile, so on every kiosk start
# PulseAudio's module-alsa-card fails to load it and logs:
#   module-alsa-card.c: Failed to find a working profile.
#   module.c: Failed to load module "module-alsa-card" (... platform-fef05700.hdmi ...)
# We tell udev to mark this card with PULSE_IGNORE=1 so module-udev-detect
# skips it. Idempotent install — only writes/triggers the rule when missing.
UDEV_RULE=/etc/udev/rules.d/91-pulse-ignore-unused-hdmi.rules
if [ ! -f "$UDEV_RULE" ]; then
    echo "Installing udev rule to silence unused-HDMI PulseAudio warnings..."
    sudo tee "$UDEV_RULE" > /dev/null <<'EORULE'
# Mark the unused vc4-hdmi-1 ALSA card as PulseAudio-ignored on Pi 4 kiosk.
# Without this rule, PulseAudio logs "Failed to find a working profile" on
# every start because no sink is plugged into HDMI1.
SUBSYSTEM=="sound", KERNEL=="card?", ATTR{id}=="vc4hdmi1", ENV{PULSE_IGNORE}="1"
EORULE
    sudo udevadm control --reload-rules
    sudo udevadm trigger --subsystem-match=sound
fi

# Kill any existing PulseAudio and clean up stale socket
echo "Cleaning up existing PulseAudio processes..."
sudo killall pulseaudio 2>/dev/null || true
sleep 1

# Ensure XDG_RUNTIME_DIR exists (may not exist on cold boot before user login)
if [ ! -d "${RUNTIME_DIR}" ]; then
    echo "Creating ${RUNTIME_DIR}..."
    sudo mkdir -p "${RUNTIME_DIR}"
    sudo chown magic:magic "${RUNTIME_DIR}"
    sudo chmod 700 "${RUNTIME_DIR}"
fi

# Remove stale PulseAudio socket to prevent "Address already in use"
rm -f "${RUNTIME_DIR}/pulse/native" "${RUNTIME_DIR}/pulse/pid" 2>/dev/null

echo "Waiting for HDMI audio card..."

while [ $WAITED -lt $MAX_WAIT ]; do
    # Check if vc4hdmi0 (HDMI audio) is available
    if aplay -l 2>/dev/null | grep -q "vc4hdmi"; then
        echo "HDMI audio card detected after ${WAITED}s"
        break
    fi
    sleep 1
    WAITED=$((WAITED + 1))
done

if [ $WAITED -ge $MAX_WAIT ]; then
    echo "Warning: HDMI audio card not detected after ${MAX_WAIT}s, proceeding anyway"
fi

# Determine desired audio output from saved settings BEFORE starting PulseAudio
# This configures default.pa so PulseAudio starts with the correct default sink
SETTINGS_FILE="/opt/magic_dingus_box/config/settings.json"
PULSE_CONFIG="$HOME/.config/pulse/default.pa"
HDMI_SINK="alsa_output.platform-fef00700.hdmi.hdmi-stereo"
HEADPHONE_SINK="alsa_output.platform-fe00b840.mailbox.stereo-fallback"
DEFAULT_SINK="$HDMI_SINK"  # Default to HDMI

if [ -f "$SETTINGS_FILE" ]; then
    AUDIO_OUTPUT=$(python3 -c "import json; d=json.load(open('$SETTINGS_FILE')); print(d.get('audio',{}).get('output','auto'))" 2>/dev/null)
    case "$AUDIO_OUTPUT" in
        headphone)
            DEFAULT_SINK="$HEADPHONE_SINK"
            echo "Audio output setting: Headphone"
            ;;
        hdmi)
            DEFAULT_SINK="$HDMI_SINK"
            echo "Audio output setting: HDMI"
            ;;
        *)
            echo "Audio output setting: Auto (HDMI default)"
            ;;
    esac
fi

# Write PulseAudio config with correct default sink BEFORE starting PulseAudio
# This ensures all new streams (including GStreamer) connect to the right sink
mkdir -p "$(dirname "$PULSE_CONFIG")"
cat > "$PULSE_CONFIG" <<PAEOF
.include /etc/pulse/default.pa
set-default-sink $DEFAULT_SINK
load-module module-stream-restore restore_device=false
PAEOF
echo "PulseAudio default sink configured: $DEFAULT_SINK"

# Disable PulseAudio's 20-second idle-exit. Without this, PA shuts down
# whenever no audio sinks are connected (e.g., between the intro video
# ending and the user starting movie playback in the Media Browser).
# When the next audio client tries to connect, autospawn fails ("Failed
# to create secure directory /run/user/1000/pulse"), causing GStreamer's
# pipeline state-change to PLAYING to fail. -1 = run forever, ready for
# the next client.
PULSE_DAEMON_CONFIG="$HOME/.config/pulse/daemon.conf"
cat > "$PULSE_DAEMON_CONFIG" <<PAEOF
exit-idle-time = -1
PAEOF
echo "PulseAudio idle-exit disabled (daemon.conf)"

# Start PulseAudio (--start daemonizes, so no exec needed)
/usr/bin/pulseaudio --start --log-target=syslog

# Wait for PulseAudio to be ready
for i in $(seq 1 10); do
    if pactl info >/dev/null 2>&1; then
        break
    fi
    sleep 0.5
done

echo "PulseAudio ready, default sink: $(pactl info 2>/dev/null | grep 'Default Sink' | cut -d: -f2 | tr -d ' ')"
