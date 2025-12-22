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

# Step 4: Create service file with correct paths
echo ""
echo "Step 4: Creating service file..."
SERVICE_FILE="/etc/systemd/system/magic-dingus-box-cpp.service"

# Create service file with detected paths
cat > "$SERVICE_FILE" << EOF
[Unit]
Description=Magic Dingus Box C++ Kiosk Engine
After=network-online.target systemd-user-sessions.service
Wants=network-online.target
# Start early, before X11 (we use DRM/KMS directly)
Before=graphical.target lightdm.service
# Don't require X11 - we bypass it entirely
Conflicts=lightdm.service

[Service]
Type=simple
# Run as root for DRM access (required for drmSetMaster)
User=root
Group=root
# Working directory where executable and assets are located
WorkingDirectory=$BUILD_DIR_ABS
# Path to executable
ExecStartPre=/bin/bash -c 'systemctl stop lightdm.service || true'
ExecStart=$APP_PATH
# Restart on failure
Restart=always
RestartSec=5
# Give service time to start properly on boot
StartLimitIntervalSec=300
StartLimitBurst=5
# Standard output/error logging
StandardOutput=journal
StandardError=journal
# Environment (if needed)
Environment=HOME=/root
Environment=DISPLAY=
# Kill all processes in the service's cgroup on stop
KillMode=mixed
KillSignal=SIGTERM
TimeoutStopSec=10

[Install]
# Start at multi-user target (before X11)
WantedBy=multi-user.target
EOF

echo "  ✓ Service file created at: $SERVICE_FILE"
echo "    Working directory: $BUILD_DIR_ABS"
echo "    Executable: $APP_PATH"

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

