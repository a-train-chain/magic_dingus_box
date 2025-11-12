#!/bin/bash
# Test intro video setup and provide troubleshooting info

echo "=== Intro Video Test ==="
echo ""

# Check settings
echo "1. Checking settings..."
if [ -f /data/settings.json ]; then
    echo "   Settings file exists"
    INTRO_SETTING=$(grep '"intro_video"' /data/settings.json | sed 's/.*: *//' | sed 's/[",]*$//')
    echo "   intro_video setting: '$INTRO_SETTING'"
else
    echo "   Settings file missing: /data/settings.json"
fi

echo ""
echo "2. Checking intro video files..."
ls -la /data/media/intro* 2>/dev/null || echo "   No intro files in /data/media/"
ls -la /opt/magic_dingus_box/dev_data/media/intro* 2>/dev/null || echo "   No intro files in /opt/magic_dingus_box/dev_data/media/"

echo ""
echo "3. Checking RetroArch lock file..."
if [ -f /tmp/magic_retroarch_active.lock ]; then
    echo "   Lock file exists - checking if RetroArch is running..."
    LOCK_PID=$(cat /tmp/magic_retroarch_active.lock)
    if ps -p "$LOCK_PID" >/dev/null 2>&1 && ps -p "$LOCK_PID" -o comm= | grep -qi retroarch; then
        echo "   RetroArch is running (PID: $LOCK_PID) - intro will be skipped"
    else
        echo "   Lock file is stale (PID: $LOCK_PID not running) - should be cleaned up"
    fi
else
    echo "   No lock file - intro should play"
fi

echo ""
echo "4. Testing intro video logic..."
INTRO_30FPS="/data/media/intro.30fps.mp4"
if [ -f "$INTRO_30FPS" ]; then
    echo "   ✓ Default intro file exists: $INTRO_30FPS"
    echo "   Intro should play on next UI restart"
else
    echo "   ✗ Default intro file missing: $INTRO_30FPS"
    echo "   This is why intro doesn't play!"
fi

echo ""
echo "5. Recommendations:"
if [ ! -f "$INTRO_30FPS" ]; then
    echo "   - Copy or create intro.30fps.mp4 in /data/media/"
    echo "   - Or symlink: ln -s /path/to/intro.mp4 /data/media/intro.30fps.mp4"
fi
if [ -f /tmp/magic_retroarch_active.lock ]; then
    LOCK_PID=$(cat /tmp/magic_retroarch_active.lock)
    if ! ps -p "$LOCK_PID" >/dev/null 2>&1; then
        echo "   - Remove stale lock file: rm /tmp/magic_retroarch_active.lock"
    fi
fi
echo "   - Restart UI: systemctl --user restart magic-ui.service"

echo ""
echo "=== End Test ==="
