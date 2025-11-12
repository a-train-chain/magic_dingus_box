#!/bin/bash
# Diagnostic script to test RetroArch audio configuration
# Run this on the Pi to check audio setup

set -e

echo "=== RetroArch Audio Diagnostic ==="
echo ""

# Check audio devices
echo "1. Checking ALSA audio devices:"
if command -v aplay >/dev/null 2>&1; then
    echo "Available playback devices:"
    aplay -l 2>&1 || echo "  ERROR: aplay -l failed"
else
    echo "  WARNING: aplay not found"
fi
echo ""

# Check PulseAudio
echo "2. Checking PulseAudio:"
if command -v pulseaudio >/dev/null 2>&1; then
    CURRENT_USER="${USER:-$(whoami)}"
    if pgrep -u "$CURRENT_USER" pulseaudio >/dev/null 2>&1; then
        echo "  PulseAudio is running for user $CURRENT_USER"
        if command -v pactl >/dev/null 2>&1; then
            echo "  Available sinks:"
            pactl list short sinks 2>&1 || echo "    ERROR: Failed to list sinks"
            echo "  Default sink:"
            pactl info 2>/dev/null | grep "Default Sink:" || echo "    ERROR: Failed to get default sink"
        fi
    else
        echo "  PulseAudio is NOT running for user $CURRENT_USER"
    fi
else
    echo "  PulseAudio not installed"
fi
echo ""

# Check /proc/asound
echo "3. Checking /proc/asound:"
if [ -f /proc/asound/cards ]; then
    echo "  Sound cards:"
    cat /proc/asound/cards
    echo ""
    echo "  HDMI devices:"
    grep -i "hdmi\|vc4hdmi" /proc/asound/cards || echo "    No HDMI devices found"
else
    echo "  /proc/asound/cards not found"
fi
echo ""

# Check RetroArch config
echo "4. Checking RetroArch configuration:"
CONFIG_DIR="${HOME}/.config/retroarch"
CONFIG_FILE="${CONFIG_DIR}/retroarch.cfg"
if [ -f "$CONFIG_FILE" ]; then
    echo "  Config file: $CONFIG_FILE"
    echo "  Audio driver:"
    grep "^audio_driver" "$CONFIG_FILE" || echo "    Not set"
    echo "  Audio device:"
    grep "^audio_device" "$CONFIG_FILE" || echo "    Not set"
    echo "  Audio enabled:"
    grep "^audio_enable" "$CONFIG_FILE" || echo "    Not set"
    echo "  Audio muted:"
    grep "^audio_mute_enable" "$CONFIG_FILE" || echo "    Not set"
    echo "  Audio volume:"
    grep "^audio_volume" "$CONFIG_FILE" || echo "    Not set"
else
    echo "  RetroArch config file not found: $CONFIG_FILE"
fi
echo ""

# Test audio playback
echo "5. Testing audio playback:"
if command -v speaker-test >/dev/null 2>&1; then
    echo "  Running speaker-test (will play for 2 seconds)..."
    timeout 2 speaker-test -t sine -f 1000 -c 2 2>&1 || echo "    ERROR: speaker-test failed"
else
    echo "  speaker-test not available (install alsa-utils)"
fi
echo ""

echo "=== Diagnostic Complete ==="
echo ""
echo "To test RetroArch audio, launch a game and check:"
echo "  - Log file: /tmp/magic_retroarch_launch.log"
echo "  - RetroArch menu: Settings > Audio"

