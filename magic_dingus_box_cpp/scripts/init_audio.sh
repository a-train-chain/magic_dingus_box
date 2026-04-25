#!/bin/bash
# Wait for HDMI audio card to be available before starting PulseAudio
# This prevents race conditions where PulseAudio starts before HDMI is initialized

MAX_WAIT=10
WAITED=0

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

# Kill any existing PulseAudio and clean up stale socket
echo "Cleaning up existing PulseAudio processes..."
sudo killall pulseaudio 2>/dev/null || true
sleep 1

# Ensure XDG_RUNTIME_DIR exists (may not exist on cold boot before user login)
if [ ! -d "/run/user/1000" ]; then
    echo "Creating /run/user/1000..."
    sudo mkdir -p /run/user/1000
    sudo chown magic:magic /run/user/1000
    sudo chmod 700 /run/user/1000
fi

# Remove stale PulseAudio socket to prevent "Address already in use"
rm -f /run/user/1000/pulse/native /run/user/1000/pulse/pid 2>/dev/null

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
