#!/bin/bash
# Migration script: Switch from Python/X11 system to C++ DRM/KMS system

set -e

echo "=== Magic Dingus Box: Migrating to C++ Kiosk Engine ==="
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo "Please run as root (use sudo)"
    exit 1
fi

# Step 1: Stop old services
echo "Step 1: Stopping old Python services..."
systemctl stop magic-ui.service 2>/dev/null || true
systemctl stop magic-ui-x11.service 2>/dev/null || true
systemctl --user stop magic-ui.service 2>/dev/null || true
systemctl --user stop magic-ui-x11.service 2>/dev/null || true
echo "  ✓ Old services stopped"

# Step 2: Disable old services
echo "Step 2: Disabling old services from auto-start..."
systemctl disable magic-ui.service 2>/dev/null || true
systemctl disable magic-ui-x11.service 2>/dev/null || true
systemctl --user disable magic-ui.service 2>/dev/null || true
systemctl --user disable magic-ui-x11.service 2>/dev/null || true
echo "  ✓ Old services disabled"

# Step 3: Check if C++ app exists
APP_PATH="/opt/magic_dingus_box/magic_dingus_box_cpp/build/magic_dingus_box_cpp"
if [ ! -f "$APP_PATH" ]; then
    echo ""
    echo "ERROR: C++ app not found at $APP_PATH"
    echo "Please build the app first:"
    echo "  cd /opt/magic_dingus_box/magic_dingus_box_cpp"
    echo "  mkdir -p build && cd build"
    echo "  cmake .. && make -j4"
    exit 1
fi
echo "  ✓ C++ app found"

# Step 4: Copy service file
echo "Step 3: Installing new service..."
SERVICE_FILE="/opt/magic_dingus_box/magic_dingus_box_cpp/systemd/magic-dingus-box-cpp.service"
if [ ! -f "$SERVICE_FILE" ]; then
    echo "ERROR: Service file not found at $SERVICE_FILE"
    exit 1
fi

cp "$SERVICE_FILE" /etc/systemd/system/
echo "  ✓ Service file copied"

# Step 5: Update paths in service file if needed
# (Assuming standard installation at /opt/magic_dingus_box)

# Step 6: Reload systemd
echo "Step 4: Reloading systemd..."
systemctl daemon-reload
echo "  ✓ systemd reloaded"

# Step 7: Enable new service
echo "Step 5: Enabling new C++ service..."
systemctl enable magic-dingus-box-cpp.service
echo "  ✓ Service enabled"

# Step 8: Start new service
echo "Step 6: Starting new C++ service..."
if systemctl start magic-dingus-box-cpp.service; then
    echo "  ✓ Service started successfully"
else
    echo "  ✗ Service failed to start. Check logs:"
    echo "    journalctl -u magic-dingus-box-cpp.service -n 50"
    exit 1
fi

# Step 9: Show status
echo ""
echo "=== Migration Complete ==="
echo ""
echo "Service status:"
systemctl status magic-dingus-box-cpp.service --no-pager -l
echo ""
echo "To view logs:"
echo "  journalctl -u magic-dingus-box-cpp.service -f"
echo ""
echo "To stop the service:"
echo "  sudo systemctl stop magic-dingus-box-cpp.service"
echo ""
echo "To revert to old system:"
echo "  sudo systemctl stop magic-dingus-box-cpp.service"
echo "  sudo systemctl disable magic-dingus-box-cpp.service"
echo "  sudo systemctl enable magic-ui-x11.service"
echo "  sudo systemctl start magic-ui-x11.service"

