#!/bin/bash
# Wait for HDMI audio card to be available before starting PulseAudio
# This prevents race conditions where PulseAudio starts before HDMI is initialized

MAX_WAIT=10  # Maximum seconds to wait
WAITED=0

# Kill any conflicting PulseAudio instances (e.g. from other users or auto-login)
echo "Cleaning up existing PulseAudio processes..."
sudo killall pulseaudio 2>/dev/null || true
# Wait a moment for processes to exit
sleep 1

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
PAEOF
echo "PulseAudio default sink configured: $DEFAULT_SINK"

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
