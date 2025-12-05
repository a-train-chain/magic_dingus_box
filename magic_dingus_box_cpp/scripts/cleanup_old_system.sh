#!/usr/bin/env bash
# Cleanup script: Disable all old Python/X11 services and ensure only C++ engine runs

set -e

echo "=== Cleaning up old Magic Dingus Box system ==="
echo ""

# Step 1: Stop all old services
echo "Step 1: Stopping old services..."
sudo systemctl stop magic-ui.service 2>/dev/null || true
sudo systemctl stop magic-ui-x11.service 2>/dev/null || true
sudo systemctl stop magic-mpv.service 2>/dev/null || true
sudo systemctl stop magic-mpv-x11.service 2>/dev/null || true
sudo systemctl --user stop magic-ui.service 2>/dev/null || true
sudo systemctl --user stop magic-ui-x11.service 2>/dev/null || true
sudo systemctl stop picom.service 2>/dev/null || true
sudo systemctl stop lightdm.service 2>/dev/null || true

echo "  ✓ Old services stopped"
echo ""

# Step 2: Disable old services from boot
echo "Step 2: Disabling old services from auto-start..."
sudo systemctl disable magic-ui.service 2>/dev/null || true
sudo systemctl disable magic-ui-x11.service 2>/dev/null || true
sudo systemctl disable magic-mpv.service 2>/dev/null || true
sudo systemctl disable magic-mpv-x11.service 2>/dev/null || true
sudo systemctl --user disable magic-ui.service 2>/dev/null || true
sudo systemctl --user disable magic-ui-x11.service 2>/dev/null || true
sudo systemctl disable picom.service 2>/dev/null || true
sudo systemctl disable lightdm.service 2>/dev/null || true

echo "  ✓ Old services disabled"
echo ""

# Step 3: Mask services to prevent accidental start
echo "Step 3: Masking old services..."
sudo systemctl mask magic-ui.service 2>/dev/null || true
sudo systemctl mask magic-ui-x11.service 2>/dev/null || true
sudo systemctl mask magic-mpv.service 2>/dev/null || true
sudo systemctl mask magic-mpv-x11.service 2>/dev/null || true
sudo systemctl mask picom.service 2>/dev/null || true

echo "  ✓ Old services masked"
echo ""

# Step 4: Kill any remaining Python processes
echo "Step 4: Killing any remaining Python processes..."
sudo pkill -f "magic_dingus_box.main" 2>/dev/null || true
sudo pkill -f "python.*magic" 2>/dev/null || true

echo "  ✓ Python processes killed"
echo ""

# Step 5: Check what's still running
echo "Step 5: Checking for remaining old system processes..."
if pgrep -f "magic_dingus_box" > /dev/null; then
    echo "  WARNING: Some magic_dingus_box processes still running:"
    pgrep -af "magic_dingus_box"
else
    echo "  ✓ No old processes found"
fi

echo ""
echo "=== Cleanup Complete ==="
echo ""
echo "Old system services have been stopped and disabled."
echo ""
echo "To enable the new C++ kiosk engine:"
echo "  cd /opt/magic_dingus_box/magic_dingus_box_cpp/systemd"
echo "  sudo ./migrate_to_cpp.sh"
echo ""
echo "Note: The C++ engine runs without X11, so lightdm is disabled."

