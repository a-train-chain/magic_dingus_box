#!/bin/bash
# Direct launch of test game for input testing
# No prompts, just launches the game

TEST_ROM="/opt/magic_dingus_box/dev_data/roms/nes/Super Mario Bros. 3.nes"
TEST_CORE="nestopia_libretro"
LAUNCH_SCRIPT="/opt/magic_dingus_box/scripts/launch_retroarch.sh"

echo "=== Launching RetroArch Test Game ==="
echo ""
echo "Test ROM: Super Mario Bros. 3"
echo "Core: nestopia_libretro"
echo ""
echo "Input Testing Instructions:"
echo "  Keyboard:"
echo "    - Press F1 to open RetroArch menu"
echo "    - Arrow keys to navigate menu"
echo "    - Enter to select"
echo ""
echo "  Controller (if connected):"
echo "    - Press Select+Start simultaneously to open menu"
echo "    - D-Pad to navigate"
echo "    - A button to select"
echo ""
echo "  To exit:"
echo "    - In menu: Close Content or Quit RetroArch"
echo ""
echo "Launching in 2 seconds..."
sleep 2

"$LAUNCH_SCRIPT" "$TEST_ROM" "$TEST_CORE" "" magic-ui.service

echo ""
echo "Game exited. Test complete!"

