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

# Start PulseAudio
exec /usr/bin/pulseaudio --start --log-target=syslog
