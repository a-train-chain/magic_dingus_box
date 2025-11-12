#!/bin/bash
# Set RetroArch to use HDMI audio
# Updates config with correct HDMI audio device

set -e

CONFIG_FILE="${HOME}/.config/retroarch/retroarch.cfg"

echo "=== Setting RetroArch HDMI Audio ==="
echo ""

# Backup
BACKUP="${CONFIG_FILE}.bak.$(date +%Y%m%d_%H%M%S)"
cp "$CONFIG_FILE" "$BACKUP"
echo "Backed up config to: $BACKUP"
echo ""

# Try PulseAudio first
AUDIO_DRIVER="alsa"
CURRENT_USER="${USER:-$(whoami)}"
if command -v pulseaudio >/dev/null 2>&1 && pgrep -u "$CURRENT_USER" pulseaudio >/dev/null 2>&1; then
    AUDIO_DRIVER="pulse"
    echo "Using PulseAudio driver"
else
    echo "Using ALSA driver"
    echo ""
    echo "Available HDMI devices:"
    aplay -l | grep -i hdmi || echo "  (none found)"
fi

# Detect HDMI device
HDMI_DEVICE=""
if [ "$AUDIO_DRIVER" = "pulse" ]; then
    if command -v pactl >/dev/null 2>&1; then
        HDMI_SINK=$(pactl list short sinks 2>/dev/null | grep -i hdmi | head -1 | awk '{print $2}')
        if [ -n "$HDMI_SINK" ]; then
            HDMI_DEVICE="$HDMI_SINK"
            echo "Found PulseAudio HDMI sink: $HDMI_DEVICE"
        fi
    fi
else
    # Try card 1 first (vc4hdmi0)
    if aplay -l | grep -q "card 1.*vc4hdmi0"; then
        HDMI_DEVICE="alsa/hdmi:CARD=vc4hdmi0,DEV=0"
        echo "Using HDMI device: $HDMI_DEVICE (card 1)"
    # Try card 2 (vc4hdmi1)
    elif aplay -l | grep -q "card 2.*vc4hdmi1"; then
        HDMI_DEVICE="alsa/hdmi:CARD=vc4hdmi1,DEV=0"
        echo "Using HDMI device: $HDMI_DEVICE (card 2)"
    else
        HDMI_DEVICE="alsa/hdmi:CARD=vc4hdmi0,DEV=0"
        echo "Using default HDMI device: $HDMI_DEVICE"
    fi
fi

echo ""
echo "Updating RetroArch config..."

# Update audio driver
sed -i '/^audio_driver/d' "$CONFIG_FILE"
echo "audio_driver = \"$AUDIO_DRIVER\"" >> "$CONFIG_FILE"

# Update audio device
if [ -n "$HDMI_DEVICE" ]; then
    sed -i '/^audio_device/d' "$CONFIG_FILE"
    echo "audio_device = \"$HDMI_DEVICE\"" >> "$CONFIG_FILE"
fi

# Ensure audio is enabled
sed -i '/^audio_enable/d' "$CONFIG_FILE"
echo "audio_enable = \"true\"" >> "$CONFIG_FILE"

sed -i '/^audio_mute_enable/d' "$CONFIG_FILE"
echo "audio_mute_enable = \"false\"" >> "$CONFIG_FILE"

sed -i '/^audio_volume/d' "$CONFIG_FILE"
echo "audio_volume = \"1.0\"" >> "$CONFIG_FILE"

sed -i '/^audio_out_rate/d' "$CONFIG_FILE"
echo "audio_out_rate = \"48000\"" >> "$CONFIG_FILE"

echo ""
echo "✓ Config updated!"
echo ""
echo "Current audio settings:"
grep -E "^audio_driver|^audio_device|^audio_enable|^audio_volume|^audio_out_rate" "$CONFIG_FILE" | head -5
echo ""
echo "Restart your game for changes to take effect."
echo ""
echo "If audio still doesn't work, try in RetroArch menu:"
echo "  Settings > Audio > Audio Driver"
echo "  - Try 'pulse' if available"
echo "  - Or try 'sdl2' as fallback"

