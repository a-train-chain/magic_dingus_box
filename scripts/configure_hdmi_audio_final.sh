#!/bin/bash
# Final HDMI audio configuration for RetroArch on Raspberry Pi
# Uses ALSA with correct device format for Pi HDMI

set -e

CONFIG_FILE="${HOME}/.config/retroarch/retroarch.cfg"

echo "=== Configuring HDMI Audio for RetroArch ==="
echo ""

# Backup
BACKUP="${CONFIG_FILE}.bak.$(date +%Y%m%d_%H%M%S)"
cp "$CONFIG_FILE" "$BACKUP"
echo "Backed up config to: $BACKUP"
echo ""

# For Raspberry Pi, ALSA is most reliable
# Use plughw device format which handles format conversion automatically
AUDIO_DRIVER="alsa"

# Determine which HDMI port to use (try card 1 first, then card 2)
HDMI_CARD=1
if aplay -l | grep -q "card 1.*vc4hdmi0"; then
    HDMI_DEVICE="plughw:1,0"
    echo "Using HDMI card 1 (vc4hdmi0): $HDMI_DEVICE"
elif aplay -l | grep -q "card 2.*vc4hdmi1"; then
    HDMI_CARD=2
    HDMI_DEVICE="plughw:2,0"
    echo "Using HDMI card 2 (vc4hdmi1): $HDMI_DEVICE"
else
    HDMI_DEVICE="plughw:1,0"
    echo "Using default HDMI device: $HDMI_DEVICE"
fi

# Ensure HDMI audio is unmuted and at full volume
echo ""
echo "Setting HDMI audio volume..."
amixer -c $HDMI_CARD set PCM 100% unmute 2>/dev/null || true
amixer -c $HDMI_CARD set PCM unmute 2>/dev/null || true

echo ""
echo "Updating RetroArch config..."

# Remove old audio settings
sed -i '/^audio_driver/d' "$CONFIG_FILE"
sed -i '/^audio_device/d' "$CONFIG_FILE"
sed -i '/^audio_enable/d' "$CONFIG_FILE"
sed -i '/^audio_mute_enable/d' "$CONFIG_FILE"
sed -i '/^audio_volume/d' "$CONFIG_FILE"
sed -i '/^audio_out_rate/d' "$CONFIG_FILE"
sed -i '/^audio_latency/d' "$CONFIG_FILE"
sed -i '/^audio_resampler/d' "$CONFIG_FILE"
sed -i '/^audio_sync/d' "$CONFIG_FILE"

# Add correct audio settings
cat >> "$CONFIG_FILE" <<EOF

# HDMI Audio Configuration (configured by configure_hdmi_audio_final.sh)
# Using ALSA with plughw device for automatic format conversion
audio_driver = "alsa"
audio_device = "$HDMI_DEVICE"
audio_enable = "true"
audio_mute_enable = "false"
audio_volume = "1.0"
audio_out_rate = "48000"
audio_latency = "64"
audio_resampler = "sinc"
audio_sync = "true"
EOF

echo "✓ Config updated!"
echo ""
echo "Audio Configuration:"
echo "  Driver: ALSA (most reliable for Raspberry Pi)"
echo "  Device: $HDMI_DEVICE"
echo "  Output Rate: 48000 Hz"
echo "  Volume: 100%"
echo "  Mute: OFF"
echo ""
echo "Current settings:"
grep -E "^audio_driver|^audio_device|^audio_enable|^audio_volume|^audio_out_rate" "$CONFIG_FILE" | head -5
echo ""
echo "✓ Configuration complete!"
echo ""
echo "Next steps:"
echo "  1. Reboot your Pi: sudo reboot"
echo "  2. Launch a RetroArch game"
echo "  3. Audio should work through HDMI"
echo ""
echo "If audio still doesn't work after reboot, try:"
echo "  - In RetroArch menu: Settings > Audio > Audio Driver > sdl2"
echo "  - Or check: Settings > Audio > Audio Device (should show $HDMI_DEVICE)"

