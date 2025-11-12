#!/bin/bash
# Completely rebuild RetroArch config file with correct audio settings
# This fixes corrupted config files

set -e

CONFIG_FILE="${HOME}/.config/retroarch/retroarch.cfg"
CONFIG_DIR="${HOME}/.config/retroarch"

echo "=== Rebuilding RetroArch Config File ==="
echo ""

mkdir -p "$CONFIG_DIR"

# Backup old config
if [ -f "$CONFIG_FILE" ]; then
    BACKUP="${CONFIG_FILE}.backup.$(date +%Y%m%d_%H%M%S)"
    cp "$CONFIG_FILE" "$BACKUP"
    echo "Backed up old config to: $BACKUP"
    
    # Check if it's binary/corrupted
    if file "$CONFIG_FILE" | grep -q "binary\|data"; then
        echo "WARNING: Config file is binary/corrupted. Will rebuild completely."
        rm -f "$CONFIG_FILE"
    fi
fi

# Determine HDMI device
HDMI_DEVICE="plughw:1,0"
if aplay -l 2>/dev/null | grep -q "card 1.*vc4hdmi0"; then
    HDMI_DEVICE="plughw:1,0"
elif aplay -l 2>/dev/null | grep -q "card 2.*vc4hdmi1"; then
    HDMI_DEVICE="plughw:2,0"
fi

echo "Using HDMI device: $HDMI_DEVICE"
echo ""

# Create a clean config file with essential settings
cat > "$CONFIG_FILE" <<EOF
# RetroArch Configuration File
# Rebuilt by rebuild_retroarch_config.sh on $(date)
# DO NOT EDIT MANUALLY - settings are auto-configured

# Video settings
video_driver = "gl"
video_fullscreen = "false"
video_windowed_fullscreen = "true"

# Audio settings - HDMI configuration
audio_driver = "alsa"
audio_device = "$HDMI_DEVICE"
audio_enable = "true"
audio_mute_enable = "false"
audio_volume = "1.0"
audio_out_rate = "48000"
audio_latency = "64"
audio_resampler = "sinc"
audio_sync = "true"

# Controller settings - ensure all players are enabled
input_joypad_driver = "udev"
input_autodetect_enable = "true"
input_auto_game_focus = "true"
input_game_focus_enable = "true"
input_enable_hotkey = "true"
input_hotkey_block_delay = "0"
input_menu_toggle_gamepad_combo = "2+3"
input_player1_joypad_index = "0"
input_player1_enable = "true"
input_player2_enable = "true"
input_player3_enable = "true"
input_player4_enable = "true"
input_autoconfig_enable = "true"
input_block_timeout = "0"
input_enabled = "true"

# Prevent RetroArch from overwriting settings
config_save_on_exit = "false"

# Menu settings
menu_driver = "ozone"
menu_show_core_updater = "true"
menu_show_online_updater = "true"

# Core updater
core_updater_buildbot_cores_url = "https://buildbot.libretro.com/nightly/linux/aarch64/latest"
core_updater_buildbot_assets_url = "https://buildbot.libretro.com/assets/"
core_updater_auto_extract_archive = "true"
EOF

# Set proper permissions
chmod 644 "$CONFIG_FILE"

echo "✓ Config file rebuilt!"
echo ""
echo "Verifying settings..."
if [ -f "$CONFIG_FILE" ] && ! file "$CONFIG_FILE" | grep -q "binary\|data"; then
    echo "✓ Config file is valid text file"
    echo ""
    echo "Audio settings:"
    grep "^audio_" "$CONFIG_FILE" | grep -E "(driver|device|enable|volume|out_rate|mute)" | head -6
    echo ""
    echo "Controller settings:"
    grep "^input_" "$CONFIG_FILE" | grep -E "(joypad_driver|autodetect|menu_toggle_gamepad|enable_hotkey)" | head -4
else
    echo "✗ ERROR: Config file still appears corrupted!"
    exit 1
fi

echo ""
echo "=== Configuration Rebuilt Successfully ==="
echo ""
echo "Settings:"
echo "  Audio Driver: ALSA"
echo "  Audio Device: $HDMI_DEVICE"
echo "  Audio Volume: 100%"
echo "  Audio Output Rate: 48000 Hz"
echo "  Menu Combo: Select+Start (2+3)"
echo ""
echo "✓ Ready for reboot!"

