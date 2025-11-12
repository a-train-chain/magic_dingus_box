#!/bin/bash
# Test RetroArch keyboard and controller input
# Launches a test game and verifies input configuration

set -e

echo "=== RetroArch Input Test ==="
echo ""

# Check if keyboard is connected
echo "1. Checking keyboard detection:"
if lsusb | grep -qi keyboard; then
    echo "   ✓ USB keyboard detected"
    lsusb | grep -i keyboard
else
    echo "   ✗ No USB keyboard found"
fi
echo ""

# Check if controller is connected
echo "2. Checking controller detection:"
if ls /dev/input/js* 2>/dev/null; then
    echo "   ✓ Controller device(s) found:"
    ls -l /dev/input/js*
else
    echo "   ✗ No controller devices found"
fi
echo ""

# Check RetroArch config
CONFIG_FILE="${HOME}/.config/retroarch/retroarch.cfg"
echo "3. Checking RetroArch input configuration:"
if [ -f "$CONFIG_FILE" ]; then
    echo "   Keyboard settings:"
    grep -E "^input_driver|^input_enabled|^input_game_focus|^input_menu_toggle|^input_enable_hotkey" "$CONFIG_FILE" | head -5 || echo "     (not found)"
    echo ""
    echo "   Controller settings:"
    grep -E "^input_joypad_driver|^input_autodetect|^input_menu_toggle_gamepad" "$CONFIG_FILE" | head -3 || echo "     (not found)"
else
    echo "   ✗ RetroArch config not found: $CONFIG_FILE"
fi
echo ""

# Find a test ROM
TEST_ROM=""
TEST_CORE=""
if [ -f "/opt/magic_dingus_box/dev_data/roms/nes/Super Mario Bros. 3.nes" ]; then
    TEST_ROM="/opt/magic_dingus_box/dev_data/roms/nes/Super Mario Bros. 3.nes"
    TEST_CORE="nestopia_libretro"
    echo "4. Test ROM found:"
    echo "   ROM: $TEST_ROM"
    echo "   Core: $TEST_CORE"
elif [ -f "/opt/magic_dingus_box/dev_data/roms/nes" ] && [ -n "$(ls /opt/magic_dingus_box/dev_data/roms/nes/*.nes 2>/dev/null | head -1)" ]; then
    TEST_ROM=$(ls /opt/magic_dingus_box/dev_data/roms/nes/*.nes 2>/dev/null | head -1)
    TEST_CORE="nestopia_libretro"
    echo "4. Test ROM found:"
    echo "   ROM: $TEST_ROM"
    echo "   Core: $TEST_CORE"
else
    echo "4. ✗ No test ROM found"
    echo ""
    echo "=== Test Complete (no ROM to launch) ==="
    exit 0
fi
echo ""

# Check launch script
LAUNCH_SCRIPT="/opt/magic_dingus_box/scripts/launch_retroarch.sh"
if [ ! -f "$LAUNCH_SCRIPT" ]; then
    echo "✗ Launch script not found: $LAUNCH_SCRIPT"
    exit 1
fi

echo "5. Ready to launch test game"
echo "   This will:"
echo "   - Launch RetroArch with the test ROM"
echo "   - You can test keyboard (F1 for menu)"
echo "   - You can test controller (Select+Start for menu)"
echo ""
read -p "   Launch test game now? (y/n): " -n 1 -r
echo ""
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Test cancelled."
    exit 0
fi

echo ""
echo "=== Launching RetroArch Test Game ==="
echo "Instructions:"
echo "  - Keyboard: Press F1 to open menu"
echo "  - Controller: Press Select+Start to open menu"
echo "  - In menu: Use arrow keys/D-pad to navigate, Enter/A button to select"
echo "  - Exit: Close Content or Quit RetroArch from menu"
echo ""
echo "Launching in 3 seconds..."
sleep 3

# Launch the game
"$LAUNCH_SCRIPT" "$TEST_ROM" "$TEST_CORE" "" magic-ui.service

echo ""
echo "=== Test Complete ==="
echo ""
echo "Did the keyboard work? (F1 opened menu?)"
echo "Did the controller work? (Select+Start opened menu?)"

