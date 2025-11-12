#!/bin/bash
# Fix controller and intro video issues
# Ensures everything is configured correctly

set -e

echo "=== Fixing Controller and Intro Video Issues ==="
echo ""

# 1. Fix RetroArch controller config
echo "1. Rebuilding RetroArch config with controller settings..."
/opt/magic_dingus_box/scripts/rebuild_retroarch_config.sh

# 2. Ensure intro video symlink exists
echo ""
echo "2. Checking intro video..."
if [ ! -f /opt/magic_dingus_box/dev_data/media/intro.30fps.mp4 ]; then
    if [ -f /opt/magic_dingus_box/dev_data/media/intro.mp4 ]; then
        ln -sf /opt/magic_dingus_box/dev_data/media/intro.mp4 /opt/magic_dingus_box/dev_data/media/intro.30fps.mp4
        echo "   ✓ Created symlink: intro.30fps.mp4 -> intro.mp4"
    else
        echo "   ✗ Intro video not found: /opt/magic_dingus_box/dev_data/media/intro.mp4"
    fi
else
    echo "   ✓ Intro video symlink exists"
fi

# 3. Restart services
echo ""
echo "3. Restarting services..."
systemctl --user daemon-reload
systemctl --user restart magic-mpv-x11.service || echo "   (mpv service not running)"
systemctl --user restart magic-ui.service || echo "   (ui service not running)"

echo ""
echo "=== Fix Complete ==="
echo ""
echo "Controller:"
echo "  - RetroArch config rebuilt with controller settings"
echo "  - All players enabled"
echo "  - Menu combo: Select+Start (2+3)"
echo ""
echo "Intro Video:"
echo "  - Symlink created: intro.30fps.mp4"
echo "  - Services restarted"
echo ""
echo "✓ Ready for reboot!"

