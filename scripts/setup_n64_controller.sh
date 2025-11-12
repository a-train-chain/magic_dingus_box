#!/bin/bash
# Setup RetroArch for N64 controller support
# Configures controller autodetection and N64-specific settings

set -e

CONFIG_FILE="${HOME}/.config/retroarch/retroarch.cfg"

echo "=== Setting up N64 Controller Support ==="
echo ""

if [ ! -f "$CONFIG_FILE" ]; then
    echo "ERROR: RetroArch config not found: $CONFIG_FILE"
    exit 1
fi

# Backup
BACKUP="${CONFIG_FILE}.bak.$(date +%Y%m%d_%H%M%S)"
cp "$CONFIG_FILE" "$BACKUP"
echo "Backed up config to: $BACKUP"
echo ""

# Remove keyboard-related settings
sed -i '/^input_menu_toggle = "f1"/d' "$CONFIG_FILE"
sed -i '/^input_driver = "x"/d' "$CONFIG_FILE" 2>/dev/null || true

# Ensure controller settings are correct
echo "Configuring controller settings..."

# Controller autodetection
sed -i '/^input_autodetect_enable/d' "$CONFIG_FILE"
echo 'input_autodetect_enable = "true"' >> "$CONFIG_FILE"

# Joypad driver (udev for Linux)
sed -i '/^input_joypad_driver/d' "$CONFIG_FILE"
echo 'input_joypad_driver = "udev"' >> "$CONFIG_FILE"

# Auto game focus (allows controller in menus)
sed -i '/^input_auto_game_focus/d' "$CONFIG_FILE"
echo 'input_auto_game_focus = "true"' >> "$CONFIG_FILE"

# Game focus enable
sed -i '/^input_game_focus_enable/d' "$CONFIG_FILE"
echo 'input_game_focus_enable = "true"' >> "$CONFIG_FILE"

# Enable hotkeys for controller
sed -i '/^input_enable_hotkey/d' "$CONFIG_FILE"
echo 'input_enable_hotkey = "true"' >> "$CONFIG_FILE"

sed -i '/^input_hotkey_block_delay/d' "$CONFIG_FILE"
echo 'input_hotkey_block_delay = "0"' >> "$CONFIG_FILE"

# Menu toggle combo (Select+Start)
sed -i '/^input_menu_toggle_gamepad_combo/d' "$CONFIG_FILE"
echo 'input_menu_toggle_gamepad_combo = "2+3"' >> "$CONFIG_FILE"

# Controller autoconfig
sed -i '/^input_autoconfig_enable/d' "$CONFIG_FILE"
echo 'input_autoconfig_enable = "true"' >> "$CONFIG_FILE"

# Player 1 controller index
sed -i '/^input_player1_joypad_index/d' "$CONFIG_FILE"
echo 'input_player1_joypad_index = "0"' >> "$CONFIG_FILE"

# Enable all players
for player in 1 2 3 4; do
    sed -i "/^input_player${player}_enable/d" "$CONFIG_FILE"
    echo "input_player${player}_enable = \"true\"" >> "$CONFIG_FILE"
done

# Don't block input
sed -i '/^input_block_timeout/d' "$CONFIG_FILE"
echo 'input_block_timeout = "0"' >> "$CONFIG_FILE"

# Prevent config save (keeps our settings)
sed -i '/^config_save_on_exit/d' "$CONFIG_FILE"
echo 'config_save_on_exit = "false"' >> "$CONFIG_FILE"

echo "✓ Controller settings configured"
echo ""
echo "Current controller settings:"
grep -E "^input_autodetect|^input_joypad_driver|^input_menu_toggle_gamepad|^input_enable_hotkey|^input_player1" "$CONFIG_FILE" | head -6
echo ""
echo "Controller Configuration:"
echo "  - Autodetect: Enabled"
echo "  - Driver: udev"
echo "  - Menu combo: Select+Start (buttons 2+3)"
echo "  - Hotkeys: Enabled"
echo ""
echo "For N64 games:"
echo "  - Controller will auto-detect"
echo "  - Press Select+Start to open menu"
echo "  - D-Pad to navigate, A button to select"
echo ""
echo "✓ Setup complete!"

