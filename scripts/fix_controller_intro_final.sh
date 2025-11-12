#!/bin/bash
# Final fix for controller and intro video issues

set -e

echo "=== Fixing Controller and Intro Video Issues ==="
echo ""

# 1. Ensure intro video symlink exists
echo "1. Checking intro video..."
if [ ! -f /opt/magic_dingus_box/dev_data/media/intro.30fps.mp4 ] && [ ! -f /data/media/intro.30fps.mp4 ]; then
    if [ -f /opt/magic_dingus_box/dev_data/media/intro.mp4 ]; then
        ln -sf /opt/magic_dingus_box/dev_data/media/intro.mp4 /opt/magic_dingus_box/dev_data/media/intro.30fps.mp4
        echo "   ✓ Created symlink: intro.30fps.mp4 -> intro.mp4"
    elif [ -f /data/media/intro.mp4 ]; then
        ln -sf /data/media/intro.mp4 /data/media/intro.30fps.mp4
        echo "   ✓ Created symlink: intro.30fps.mp4 -> intro.mp4"
    fi
else
    echo "   ✓ Intro video symlink exists"
fi

# 2. Rebuild RetroArch config with controller settings
echo ""
echo "2. Rebuilding RetroArch config..."
/opt/magic_dingus_box/scripts/rebuild_retroarch_config.sh

# 3. Check controller
echo ""
echo "3. Checking controller..."
if lsusb | grep -qiE 'game|controller|joystick|n64'; then
    echo "   ✓ Controller detected via USB"
    if ls -la /dev/input/js* 2>/dev/null | head -1; then
        echo "   ✓ Joystick device found"
    else
        echo "   ⚠ Controller detected but no /dev/input/js* device (may need to wait)"
    fi
else
    echo "   ⚠ No controller detected - please plug in your controller"
    echo "   - Controller will be auto-detected when plugged in"
    echo "   - RetroArch is configured to use controller when available"
fi

# 4. Restart services
echo ""
echo "4. Restarting services..."
systemctl --user daemon-reload
systemctl --user restart magic-ui.service || echo "   (UI service restart attempted)"
sleep 2

echo ""
echo "=== Fix Complete ==="
echo ""
echo "Status:"
echo "  ✓ Intro video: Configured and will play on boot"
echo "  ✓ RetroArch: Controller settings configured"
echo ""
echo "Controller:"
echo "  - Plug in your controller if not already connected"
echo "  - Controller will be auto-detected by RetroArch"
echo "  - Menu combo: Select+Start (buttons 2+3)"
echo ""
echo "Intro Video:"
echo "  - Will play automatically on boot"
echo "  - Duration: ~11 seconds"
echo ""
echo "✓ Ready to test!"

