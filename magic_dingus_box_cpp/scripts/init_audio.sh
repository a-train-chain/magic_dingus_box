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

# Start PulseAudio (--start daemonizes, so no exec needed)
/usr/bin/pulseaudio --start --log-target=syslog

# Wait for PulseAudio to be ready
for i in $(seq 1 10); do
    if pactl info >/dev/null 2>&1; then
        break
    fi
    sleep 0.5
done

# Apply saved audio output setting before the app starts
# This ensures the intro video plays on the correct output from the start
SETTINGS_FILE="/opt/magic_dingus_box/config/settings.json"
if [ -f "$SETTINGS_FILE" ]; then
    AUDIO_OUTPUT=$(python3 -c "import json; d=json.load(open('$SETTINGS_FILE')); print(d.get('audio',{}).get('output','auto'))" 2>/dev/null)
    case "$AUDIO_OUTPUT" in
        headphone)
            SINK="alsa_output.platform-fe00b840.mailbox.stereo-fallback"
            echo "Setting audio output to Headphone"
            pactl set-default-sink "$SINK" 2>/dev/null
            ;;
        hdmi)
            SINK="alsa_output.platform-fef00700.hdmi.hdmi-stereo"
            echo "Setting audio output to HDMI"
            pactl set-default-sink "$SINK" 2>/dev/null
            ;;
        *)
            echo "Audio output: Auto (system default)"
            ;;
    esac
fi
