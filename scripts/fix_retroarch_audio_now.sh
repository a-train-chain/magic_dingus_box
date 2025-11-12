#!/bin/bash
# Quick fix for RetroArch audio - sets volume to 100% and fixes menu combo
# Run this while game is running (will take effect on next game launch)

set -e

CONFIG_FILE="${HOME}/.config/retroarch/retroarch.cfg"

echo "=== Quick RetroArch Audio Fix ==="
echo ""

if [ ! -f "$CONFIG_FILE" ]; then
    echo "ERROR: RetroArch config not found: $CONFIG_FILE"
    exit 1
fi

# Backup config
BACKUP="${CONFIG_FILE}.bak.$(date +%Y%m%d_%H%M%S)"
cp "$CONFIG_FILE" "$BACKUP"
echo "Backed up config to: $BACKUP"
echo ""

# Fix audio volume (CRITICAL - was set to 0!)
sed -i 's/^audio_volume = ".*"/audio_volume = "1.0"/' "$CONFIG_FILE"
sed -i 's/^audio_mixer_volume = ".*"/audio_mixer_volume = "1.0"/' "$CONFIG_FILE"

# Ensure audio is enabled and not muted
sed -i 's/^audio_enable = ".*"/audio_enable = "true"/' "$CONFIG_FILE"
sed -i 's/^audio_mute_enable = ".*"/audio_mute_enable = "false"/' "$CONFIG_FILE"

# Fix menu toggle combo
sed -i 's/^input_menu_toggle_gamepad_combo = ".*"/input_menu_toggle_gamepad_combo = "2+3"/' "$CONFIG_FILE"

# Ensure ALSA driver (PulseAudio failed)
sed -i 's/^audio_driver = ".*"/audio_driver = "alsa"/' "$CONFIG_FILE"

# Set HDMI audio device
sed -i 's|^audio_device = ".*"|audio_device = "alsa/hdmi:CARD=vc4hdmi0,DEV=0"|' "$CONFIG_FILE"

echo "Fixed audio settings:"
echo "  - audio_volume: 1.0 (was 0.0)"
echo "  - audio_mute_enable: false"
echo "  - audio_driver: alsa"
echo "  - audio_device: alsa/hdmi:CARD=vc4hdmi0,DEV=0"
echo "  - input_menu_toggle_gamepad_combo: 2+3 (Select+Start)"
echo ""
echo "Current audio settings:"
grep "^audio_" "$CONFIG_FILE" | grep -E "(driver|device|enable|mute|volume)" | head -6
echo ""
echo "Menu toggle setting:"
grep "input_menu_toggle_gamepad_combo" "$CONFIG_FILE"
echo ""
echo "✓ Config updated! Restart the game for changes to take effect."
echo ""
echo "Note: If you're using bezels/overlays, the temp config issue has been"
echo "fixed in the launch script, so future launches will work correctly."

