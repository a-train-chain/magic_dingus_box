#!/bin/bash
# Verify audio setup is correct before reboot

echo "=== Verifying Audio Setup ==="
echo ""

echo "1. RetroArch config file:"
if [ -f ~/.config/retroarch/retroarch.cfg ]; then
    if file ~/.config/retroarch/retroarch.cfg | grep -q "ASCII text"; then
        echo "   ✓ Config file is valid (text format)"
        echo ""
        echo "   Audio settings:"
        grep "^audio_" ~/.config/retroarch/retroarch.cfg | grep -E "(driver|device|enable|volume|out_rate|mute)" | head -6
        echo ""
        echo "   Config save protection:"
        grep "^config_save_on_exit" ~/.config/retroarch/retroarch.cfg || echo "     (not set - RetroArch may overwrite settings)"
    else
        echo "   ✗ Config file is corrupted (binary format)"
        echo "   Run: /opt/magic_dingus_box/scripts/rebuild_retroarch_config.sh"
    fi
else
    echo "   ✗ Config file not found"
fi

echo ""
echo "2. HDMI audio device:"
aplay -l | grep -i hdmi | head -2 || echo "   (no HDMI devices found)"

echo ""
echo "3. HDMI audio volume:"
amixer -c 1 get PCM 2>/dev/null | grep -E '(Front Left|Front Right|\[on\]|\[off\])' | head -2 || echo "   (could not check volume)"

echo ""
echo "4. Launch script:"
if [ -f /opt/magic_dingus_box/scripts/launch_retroarch.sh ]; then
    echo "   ✓ Launch script exists"
    if grep -q "plughw:1,0" /opt/magic_dingus_box/scripts/launch_retroarch.sh; then
        echo "   ✓ Launch script has HDMI audio device configured"
    else
        echo "   ✗ Launch script missing HDMI audio device"
    fi
else
    echo "   ✗ Launch script not found"
fi

echo ""
echo "=== Verification Complete ==="
echo ""
echo "If everything shows ✓, you're ready to reboot!"
echo "After reboot, launch a game and audio should work."

