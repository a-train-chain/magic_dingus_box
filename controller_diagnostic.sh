#!/bin/bash
# N64 Controller Diagnostic Script

echo "=== N64 Controller Diagnostic ==="
echo ""

# Check if running on Pi
if [ ! -f "/opt/magic_dingus_box/magic_dingus_box/main.py" ]; then
    echo "❌ ERROR: This script must be run on the Raspberry Pi with Magic Dingus Box installed"
    echo "Expected to find: /opt/magic_dingus_box/magic_dingus_box/main.py"
    exit 1
fi

echo "✓ Running on Raspberry Pi with Magic Dingus Box installed"
echo ""

# Check USB devices
echo "1. USB Controller Detection:"
lsusb | grep -iE 'game|controller|joystick|n64|broadcom' || echo "   ❌ No controllers found via USB"
echo ""

# Check input devices
echo "2. Linux Input Devices:"
if ls /dev/input/js* >/dev/null 2>&1; then
    echo "   ✓ Joystick device(s) found:"
    ls -la /dev/input/js*
else
    echo "   ❌ No joystick devices (/dev/input/js*)"
fi

echo ""
echo "3. Event devices:"
ls -la /dev/input/event* | head -8
echo ""

# Check device capabilities
echo "4. Controller Device Details:"
cat /proc/bus/input/devices | grep -A15 -iE 'game|controller|joystick|n64|broadcom' | head -30 || echo "   ❌ No controller devices found in /proc/bus/input/devices"
echo ""

# Check user permissions
echo "5. User Permissions:"
groups | grep -E 'input|plugdev' || echo "   ⚠️  User not in input/plugdev groups (may cause issues)"
echo ""

# Check Magic Dingus Box logs for controller errors
echo "6. Recent Controller Logs:"
tail -50 /opt/magic_dingus_box/dev_data/logs/magic-ui.log 2>/dev/null | grep -iE 'controller|joystick|evdev|input' | tail -10 || echo "   No recent controller logs found"
echo ""

echo "=== Diagnostic Complete ==="
echo ""
echo "Next steps:"
echo "1. If no controller detected: Check USB connection and try different USB port"
echo "2. If controller detected but no /dev/input/js*: Try replugging controller"
echo "3. Run the Python identification script: python3 identify_controller.py"
echo ""
echo "Share the output above with the developer for help!"
