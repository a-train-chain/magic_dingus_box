#!/bin/bash
# Diagnose controller and intro video issues

echo "=== Controller Diagnostics ==="
echo ""

echo "1. USB Devices:"
lsusb | grep -iE 'game|controller|joystick|n64' || echo "   No controllers found via USB"
echo ""

echo "2. Input Devices:"
ls -la /dev/input/js* 2>/dev/null || echo "   No joystick devices (/dev/input/js*)"
ls -la /dev/input/event* 2>/dev/null | head -5
echo ""

echo "3. Input Device Info:"
cat /proc/bus/input/devices | grep -A10 -iE 'game|controller|joystick|n64' | head -30 || echo "   No controller devices found"
echo ""

echo "4. User Groups:"
groups | grep -E 'input|plugdev' || echo "   User not in input/plugdev groups"
echo ""

echo "=== Intro Video Diagnostics ==="
echo ""

echo "1. Intro Video Files:"
ls -la /opt/magic_dingus_box/dev_data/media/intro* 2>/dev/null || ls -la /data/media/intro* 2>/dev/null
echo ""

echo "2. RetroArch Lock File:"
ls -la /tmp/magic_retroarch_active.lock 2>/dev/null || echo "   No lock file (good - intro should play)"
echo ""

echo "3. UI Service Status:"
systemctl --user status magic-ui.service --no-pager | head -15
echo ""

echo "4. MPV Service Status:"
systemctl status magic-mpv.service --no-pager | head -15 || systemctl --user status magic-mpv-x11.service --no-pager | head -15
echo ""

echo "5. Recent UI Logs (intro-related):"
tail -100 /opt/magic_dingus_box/dev_data/logs/magic-ui.log 2>/dev/null | grep -iE '(intro|played_intro|retroarch_lock|Playing|Loaded)' | tail -10 || echo "   No recent intro logs"
echo ""

echo "6. Python Check:"
python3 <<EOF
import sys
sys.path.insert(0, '/opt/magic_dingus_box')
from magic_dingus_box.config import AppConfig
import os
from pathlib import Path

config = AppConfig()
intro_30fps = config.media_dir / "intro.30fps.mp4"
retroarch_lock_file = "/tmp/magic_retroarch_active.lock"

print(f"Media dir: {config.media_dir}")
print(f"Intro path exists: {intro_30fps.exists()}")
if intro_30fps.exists():
    print(f"Intro path resolved: {intro_30fps.resolve()}")
print(f"RetroArch lock exists: {os.path.exists(retroarch_lock_file)}")
EOF

echo ""
echo "=== Recommendations ==="
echo ""
echo "Controller:"
echo "  - Plug in your controller and run this script again"
echo "  - Check if controller appears in USB devices"
echo ""
echo "Intro Video:"
echo "  - Check UI logs for intro video messages"
echo "  - If intro doesn't play, check that /data/media/intro.30fps.mp4 exists"
echo "  - Ensure intro_video setting is null (not empty string) in settings.json"
echo "  - Restart UI service: systemctl --user restart magic-ui.service"

