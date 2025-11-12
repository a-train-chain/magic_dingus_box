#!/bin/bash
# Force fix keyboard settings in main RetroArch config
# This ensures keyboard works even if temp config has issues

CONFIG_FILE="${HOME}/.config/retroarch/retroarch.cfg"

echo "=== Force Fixing Keyboard Settings ==="

# Backup
cp "$CONFIG_FILE" "${CONFIG_FILE}.bak.$(date +%Y%m%d_%H%M%S)"

# Remove old keyboard settings
sed -i '/^input_driver/d' "$CONFIG_FILE"
sed -i '/^input_enabled/d' "$CONFIG_FILE"
sed -i '/^input_game_focus_enable/d' "$CONFIG_FILE"
sed -i '/^input_auto_game_focus/d' "$CONFIG_FILE"
sed -i '/^input_enable_hotkey/d' "$CONFIG_FILE"
sed -i '/^input_hotkey_block_delay/d' "$CONFIG_FILE"
sed -i '/^input_menu_toggle/d' "$CONFIG_FILE"
sed -i '/^input_block_timeout/d' "$CONFIG_FILE"
sed -i '/^config_save_on_exit/d' "$CONFIG_FILE"

# Add correct settings at the end (so they override)
cat >> "$CONFIG_FILE" <<EOF

# CRITICAL: Keyboard input settings (added by fix_keyboard_now.sh)
input_driver = "x"
input_enabled = "true"
input_game_focus_enable = "true"
input_auto_game_focus = "true"
input_block_timeout = "0"
input_enable_hotkey = "true"
input_hotkey_block_delay = "0"
input_menu_toggle = "f1"
input_menu_toggle_gamepad_combo = "2+3"
config_save_on_exit = "false"
EOF

echo "✓ Keyboard settings added to main config"
echo ""
echo "Current keyboard settings:"
grep -E "^input_driver|^input_enable_hotkey|^input_menu_toggle|^config_save" "$CONFIG_FILE" | tail -4

