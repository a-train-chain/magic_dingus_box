#!/bin/bash
# Comprehensive fix for RetroArch HDMI audio - ensures settings persist
# This script rebuilds the audio config section properly

set -e

CONFIG_FILE="${HOME}/.config/retroarch/retroarch.cfg"
CONFIG_DIR="${HOME}/.config/retroarch"

echo "=== Fixing RetroArch Audio Configuration (Persistent) ==="
echo ""

# Create config directory if needed
mkdir -p "$CONFIG_DIR"

# Backup existing config
if [ -f "$CONFIG_FILE" ]; then
    BACKUP="${CONFIG_FILE}.bak.$(date +%Y%m%d_%H%M%S)"
    cp "$CONFIG_FILE" "$BACKUP"
    echo "Backed up config to: $BACKUP"
    
    # Check if config is corrupted (binary file)
    if file "$CONFIG_FILE" | grep -q "binary"; then
        echo "WARNING: Config file appears corrupted (binary). Creating new one..."
        mv "$CONFIG_FILE" "${CONFIG_FILE}.corrupted.$(date +%Y%m%d_%H%M%S)"
        touch "$CONFIG_FILE"
    fi
fi

# Ensure HDMI audio is unmuted and at full volume
echo ""
echo "Setting HDMI audio volume..."
amixer -c 1 set PCM 100% unmute 2>/dev/null || true
amixer -c 1 set PCM unmute 2>/dev/null || true

# Determine HDMI device
HDMI_DEVICE="plughw:1,0"
if aplay -l 2>/dev/null | grep -q "card 1.*vc4hdmi0"; then
    HDMI_DEVICE="plughw:1,0"
    echo "Using HDMI card 1: $HDMI_DEVICE"
elif aplay -l 2>/dev/null | grep -q "card 2.*vc4hdmi1"; then
    HDMI_DEVICE="plughw:2,0"
    echo "Using HDMI card 2: $HDMI_DEVICE"
fi

echo ""
echo "Updating RetroArch config..."

# Remove ALL existing audio settings (use sed with proper escaping)
sed -i '/^audio_driver/d' "$CONFIG_FILE" 2>/dev/null || true
sed -i '/^audio_device/d' "$CONFIG_FILE" 2>/dev/null || true
sed -i '/^audio_enable/d' "$CONFIG_FILE" 2>/dev/null || true
sed -i '/^audio_mute_enable/d' "$CONFIG_FILE" 2>/dev/null || true
sed -i '/^audio_volume/d' "$CONFIG_FILE" 2>/dev/null || true
sed -i '/^audio_out_rate/d' "$CONFIG_FILE" 2>/dev/null || true
sed -i '/^audio_latency/d' "$CONFIG_FILE" 2>/dev/null || true
sed -i '/^audio_resampler/d' "$CONFIG_FILE" 2>/dev/null || true
sed -i '/^audio_sync/d' "$CONFIG_FILE" 2>/dev/null || true
sed -i '/^config_save_on_exit/d' "$CONFIG_FILE" 2>/dev/null || true

# Add correct audio settings at the end of file
cat >> "$CONFIG_FILE" <<EOF

# HDMI Audio Configuration - DO NOT EDIT (auto-configured)
# Configured by fix_retroarch_audio_persistent.sh on $(date)
audio_driver = "alsa"
audio_device = "$HDMI_DEVICE"
audio_enable = "true"
audio_mute_enable = "false"
audio_volume = "1.0"
audio_out_rate = "48000"
audio_latency = "64"
audio_resampler = "sinc"
audio_sync = "true"
# Prevent RetroArch from overwriting these settings
config_save_on_exit = "false"
EOF

echo "✓ Config updated!"
echo ""
echo "Verifying configuration..."
if grep -q "^audio_driver = \"alsa\"" "$CONFIG_FILE" && grep -q "^audio_device = \"$HDMI_DEVICE\"" "$CONFIG_FILE"; then
    echo "✓ Audio settings verified in config file"
else
    echo "✗ WARNING: Settings not found in config file!"
    echo "Showing last 20 lines of config:"
    tail -20 "$CONFIG_FILE"
fi

echo ""
echo "Current audio settings:"
grep "^audio_" "$CONFIG_FILE" | grep -E "(driver|device|enable|volume|out_rate|mute)" | head -6 || echo "  (settings not found - check config file)"

echo ""
echo "=== Configuration Complete ==="
echo ""
echo "Settings:"
echo "  Driver: ALSA"
echo "  Device: $HDMI_DEVICE"
echo "  Volume: 100%"
echo "  Output Rate: 48000 Hz"
echo "  Mute: OFF"
echo ""
echo "✓ Ready for reboot!"
echo ""
echo "After reboot, launch a game and audio should work."
echo "If not, check: tail -50 /tmp/magic_retroarch_launch.log | grep -i audio"

