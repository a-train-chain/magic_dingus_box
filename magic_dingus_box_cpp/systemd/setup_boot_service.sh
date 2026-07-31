#!/bin/bash
# Setup script: Install Magic Dingus Box C++ as a systemd service for auto-boot

set -e

echo "=== Magic Dingus Box: Setting up Auto-Boot Service ==="
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo "Please run as root (use sudo)"
    exit 1
fi

# Detect installation path
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
APP_PATH="$BUILD_DIR/magic_dingus_box_cpp"

# Check if app exists
if [ ! -f "$APP_PATH" ]; then
    echo "ERROR: C++ app not found at $APP_PATH"
    echo "Please build the app first:"
    echo "  cd $PROJECT_ROOT"
    echo "  mkdir -p build && cd build"
    echo "  cmake .. && make -j4"
    exit 1
fi
echo "✓ Found app at: $APP_PATH"

# Get absolute paths
PROJECT_ROOT_ABS="$(cd "$PROJECT_ROOT" && pwd)"
BUILD_DIR_ABS="$(cd "$BUILD_DIR" && pwd)"

# Step 1: Stop old services
echo ""
echo "Step 1: Stopping old services..."
systemctl stop magic-ui.service 2>/dev/null || true
systemctl stop magic-ui-x11.service 2>/dev/null || true
systemctl stop magic-dingus-box-cpp.service 2>/dev/null || true
systemctl --user stop magic-ui.service 2>/dev/null || true
systemctl --user stop magic-ui-x11.service 2>/dev/null || true
echo "  ✓ Old services stopped"

# Step 2: Disable old services
echo ""
echo "Step 2: Disabling old services from auto-start..."
systemctl disable magic-ui.service 2>/dev/null || true
systemctl disable magic-ui-x11.service 2>/dev/null || true
systemctl --user disable magic-ui.service 2>/dev/null || true
systemctl --user disable magic-ui-x11.service 2>/dev/null || true
echo "  ✓ Old services disabled"

# Step 3: Stop lightdm if running
echo ""
echo "Step 3: Stopping display manager (lightdm)..."
systemctl stop lightdm.service 2>/dev/null || true
echo "  ✓ Display manager stopped"

# Step 3.5: Configure GPIO overlay for power switch
echo ""
echo "Step 3.5: Configuring GPIO3 power switch overlay..."
BOOT_CONFIG="/boot/config.txt"
# For newer Raspberry Pi OS, config may be in /boot/firmware/config.txt
if [ ! -f "$BOOT_CONFIG" ] && [ -f "/boot/firmware/config.txt" ]; then
    BOOT_CONFIG="/boot/firmware/config.txt"
fi

# Power switch wiring:
#   Toggle COM -> GPIO3
#   Toggle ON throw -> GND
#   Toggle OFF throw -> unconnected
# Behavior:
#   Switch ON: GPIO3 = LOW (connected to GND) -> Pi runs normally
#   Switch OFF: GPIO3 = HIGH (pull-up) -> triggers shutdown
#   Switch OFF->ON: GPIO3 goes LOW -> wakes from halt (hardware feature)
GPIO_OVERLAY="dtoverlay=gpio-shutdown,gpio_pin=3,active_low=0,gpio_pull=up"

if [ -f "$BOOT_CONFIG" ]; then
    if grep -q "dtoverlay=gpio-shutdown" "$BOOT_CONFIG"; then
        echo "  Updating existing GPIO shutdown overlay..."
        # Remove old overlay line and add new one
        sed -i '/dtoverlay=gpio-shutdown/d' "$BOOT_CONFIG"
        sed -i '/# GPIO3.*Magic Dingus Box/d' "$BOOT_CONFIG"
    fi
    echo "  Adding GPIO power switch overlay to $BOOT_CONFIG..."
    echo "" >> "$BOOT_CONFIG"
    echo "# GPIO3 power switch (Magic Dingus Box)" >> "$BOOT_CONFIG"
    echo "# ON position = GPIO3 LOW (run), OFF position = GPIO3 HIGH (shutdown)" >> "$BOOT_CONFIG"
    echo "$GPIO_OVERLAY" >> "$BOOT_CONFIG"
    echo "  ✓ GPIO overlay configured (reboot required for this change to take effect)"
else
    echo "  ⚠ Warning: $BOOT_CONFIG not found - please manually add:"
    echo "    $GPIO_OVERLAY"
fi

# Step 4: Install the canonical service file
echo ""
echo "Step 4: Installing service file..."
SERVICE_FILE="/etc/systemd/system/magic-dingus-box-cpp.service"
CANONICAL_UNIT="$SCRIPT_DIR/magic-dingus-box-cpp.service"

# The unit's single source of truth is magic-dingus-box-cpp.service in
# this directory — the same file deploy_cpp.sh installs. This script used
# to generate a THIRD variant from a heredoc: User=root, Restart=always,
# and After=network-online.target — the exact ordering the canonical
# unit's comments document as a measured ~14s black-screen regression —
# with no sd_notify/watchdog and no EnvironmentFile (Media Browser API
# keys never reached the binary). Three hand-maintained unit copies is
# how those divergences went unnoticed.
#
# The canonical unit hardcodes the production install paths
# (/opt/magic_dingus_box, User=magic), so refuse to install it from a
# checkout living anywhere else rather than enable a unit whose
# ExecStart points at a path that does not exist.
if [ "$PROJECT_ROOT_ABS" != "/opt/magic_dingus_box/magic_dingus_box_cpp" ]; then
    echo "ERROR: this checkout is at $PROJECT_ROOT_ABS"
    echo "The canonical unit expects /opt/magic_dingus_box/magic_dingus_box_cpp"
    echo "(User=magic, ExecStart under /opt). Move the install there, or use"
    echo "scripts/deploy_cpp.sh from a dev machine instead."
    exit 1
fi

cp "$CANONICAL_UNIT" "$SERVICE_FILE"

echo "  ✓ Canonical service file installed at: $SERVICE_FILE"
echo "    Source: $CANONICAL_UNIT"

# Step 5: Reload systemd
echo ""
echo "Step 5: Reloading systemd..."
systemctl daemon-reload
echo "  ✓ systemd reloaded"

# Step 6: Enable new service
echo ""
echo "Step 6: Enabling new C++ service for auto-start..."
systemctl enable magic-dingus-box-cpp.service
echo "  ✓ Service enabled for auto-start on boot"

# Step 7: Start new service
echo ""
echo "Step 7: Starting new C++ service..."
if systemctl start magic-dingus-box-cpp.service; then
    echo "  ✓ Service started successfully"
else
    echo "  ✗ Service failed to start. Check logs:"
    echo "    journalctl -u magic-dingus-box-cpp.service -n 50"
    exit 1
fi

# Step 8: Show status
echo ""
echo "=== Setup Complete ==="
echo ""
echo "Service status:"
systemctl status magic-dingus-box-cpp.service --no-pager -l | head -20
echo ""
echo "Useful commands:"
echo "  View logs:        journalctl -u magic-dingus-box-cpp.service -f"
echo "  Stop service:     sudo systemctl stop magic-dingus-box-cpp.service"
echo "  Start service:    sudo systemctl start magic-dingus-box-cpp.service"
echo "  Restart service:  sudo systemctl restart magic-dingus-box-cpp.service"
echo "  Disable auto-boot: sudo systemctl disable magic-dingus-box-cpp.service"
echo "  Enable auto-boot:  sudo systemctl enable magic-dingus-box-cpp.service"
echo ""
echo "The application will now start automatically on every boot!"
echo ""

