#!/bin/bash
# Fix RetroArch keyboard input
# Ensures keyboard works in RetroArch

set -e

CONFIG_FILE="${HOME}/.config/retroarch/retroarch.cfg"

echo "=== Fixing RetroArch Keyboard Input ==="
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

# Ensure keyboard input driver is set
sed -i 's/^input_driver = ".*"/input_driver = "x"/' "$CONFIG_FILE"
if ! grep -q "^input_driver" "$CONFIG_FILE"; then
    echo 'input_driver = "x"' >> "$CONFIG_FILE"
fi

# Ensure input is enabled
sed -i 's/^input_enabled = ".*"/input_enabled = "true"/' "$CONFIG_FILE"
if ! grep -q "^input_enabled" "$CONFIG_FILE"; then
    echo 'input_enabled = "true"' >> "$CONFIG_FILE"
fi

# Ensure game focus is enabled (allows keyboard in game)
sed -i 's/^input_game_focus_enable = ".*"/input_game_focus_enable = "true"/' "$CONFIG_FILE"
if ! grep -q "^input_game_focus_enable" "$CONFIG_FILE"; then
    echo 'input_game_focus_enable = "true"' >> "$CONFIG_FILE"
fi

# Ensure auto game focus is enabled
sed -i 's/^input_auto_game_focus = ".*"/input_auto_game_focus = "true"/' "$CONFIG_FILE"
if ! grep -q "^input_auto_game_focus" "$CONFIG_FILE"; then
    echo 'input_auto_game_focus = "true"' >> "$CONFIG_FILE"
fi

# Ensure F1 menu toggle is set
sed -i 's/^input_menu_toggle = ".*"/input_menu_toggle = "f1"/' "$CONFIG_FILE"
if ! grep -q "^input_menu_toggle" "$CONFIG_FILE"; then
    echo 'input_menu_toggle = "f1"' >> "$CONFIG_FILE"
fi

# Ensure hotkeys are enabled
sed -i 's/^input_enable_hotkey = ".*"/input_enable_hotkey = "true"/' "$CONFIG_FILE"
if ! grep -q "^input_enable_hotkey" "$CONFIG_FILE"; then
    echo 'input_enable_hotkey = "true"' >> "$CONFIG_FILE"
fi

# Don't block input
sed -i 's/^input_block_timeout = ".*"/input_block_timeout = "0"/' "$CONFIG_FILE"
if ! grep -q "^input_block_timeout" "$CONFIG_FILE"; then
    echo 'input_block_timeout = "0"' >> "$CONFIG_FILE"
fi

echo "Fixed keyboard input settings:"
echo "  - input_driver: x (X11 keyboard driver)"
echo "  - input_enabled: true"
echo "  - input_game_focus_enable: true"
echo "  - input_auto_game_focus: true"
echo "  - input_menu_toggle: f1"
echo "  - input_enable_hotkey: true"
echo ""
echo "Current keyboard input settings:"
grep "^input_driver\|^input_enabled\|^input_game_focus\|^input_menu_toggle\|^input_enable_hotkey" "$CONFIG_FILE" | head -6
echo ""
echo "✓ Config updated! Restart the game for changes to take effect."
echo ""
echo "Note: Make sure your keyboard is plugged in before launching RetroArch."

