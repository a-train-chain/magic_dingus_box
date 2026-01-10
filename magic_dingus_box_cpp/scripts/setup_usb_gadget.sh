#!/usr/bin/env bash
#
# Setup USB Ethernet Gadget Mode for Magic Dingus Box
# This allows direct USB connection from a laptop for faster uploads
#
# Usage: sudo ./setup_usb_gadget.sh
#
# After running, reboot the Pi. Then:
#   - Connect USB-C cable from laptop to Pi
#   - Access web manager at http://192.168.7.1:5000
#
set -euo pipefail

# Must run as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root: sudo $0"
    exit 1
fi

echo "=== Setting up USB Ethernet Gadget Mode ==="
echo ""

# Detect config.txt location (differs between Raspberry Pi OS versions)
if [ -f /boot/firmware/config.txt ]; then
    CONFIG_TXT="/boot/firmware/config.txt"
elif [ -f /boot/config.txt ]; then
    CONFIG_TXT="/boot/config.txt"
else
    echo "ERROR: Cannot find config.txt"
    exit 1
fi

echo "Using config: $CONFIG_TXT"

# Step 1: Enable dwc2 overlay in config.txt
echo "Step 1: Enabling dwc2 device tree overlay..."
if grep -q "^dtoverlay=dwc2" "$CONFIG_TXT"; then
    echo "  ✓ dwc2 overlay already enabled"
else
    # Add after [all] section if it exists, otherwise at end
    if grep -q "^\[all\]" "$CONFIG_TXT"; then
        sed -i '/^\[all\]/a dtoverlay=dwc2' "$CONFIG_TXT"
    else
        echo "dtoverlay=dwc2" >> "$CONFIG_TXT"
    fi
    echo "  ✓ Added dtoverlay=dwc2 to $CONFIG_TXT"
fi

# Step 2: Add modules to load at boot
echo "Step 2: Configuring kernel modules..."
MODULES_FILE="/etc/modules"

if grep -q "^dwc2" "$MODULES_FILE"; then
    echo "  ✓ dwc2 module already configured"
else
    echo "dwc2" >> "$MODULES_FILE"
    echo "  ✓ Added dwc2 to $MODULES_FILE"
fi

if grep -q "^g_ether" "$MODULES_FILE"; then
    echo "  ✓ g_ether module already configured"
else
    echo "g_ether" >> "$MODULES_FILE"
    echo "  ✓ Added g_ether to $MODULES_FILE"
fi

# Step 3: Create systemd-networkd configuration for usb0
echo "Step 3: Configuring static IP for USB network..."
mkdir -p /etc/systemd/network

cat > /etc/systemd/network/10-usb-gadget.network << 'EOF'
[Match]
Name=usb0

[Network]
Address=192.168.7.1/24
DHCPServer=yes

[DHCPServer]
PoolOffset=100
PoolSize=20
EmitDNS=no
EOF

echo "  ✓ Created /etc/systemd/network/10-usb-gadget.network"

# Step 4: Enable systemd-networkd (might already be enabled)
echo "Step 4: Enabling systemd-networkd..."
systemctl enable systemd-networkd 2>/dev/null || true
echo "  ✓ systemd-networkd enabled"

# Step 5: Create a helper script to check USB gadget status
echo "Step 5: Creating status check script..."
cat > /opt/magic_dingus_box/scripts/usb_gadget_status.sh << 'EOF'
#!/bin/bash
# Check USB Ethernet Gadget status

echo "USB Ethernet Gadget Status"
echo "=========================="

# Check if usb0 interface exists
if ip link show usb0 &>/dev/null; then
    echo "✓ USB interface (usb0) is UP"
    echo ""
    ip addr show usb0 | grep -E "inet |state"
    echo ""
    echo "Access Content Manager at: http://192.168.7.1:5000"
else
    echo "✗ USB interface (usb0) not detected"
    echo ""
    echo "To use USB mode:"
    echo "  1. Connect a USB-C cable from your computer to the Pi"
    echo "  2. Wait 5-10 seconds"
    echo "  3. Run this script again"
fi

echo ""
echo "WiFi Status:"
if ip link show wlan0 &>/dev/null; then
    IP=$(ip addr show wlan0 | grep "inet " | awk '{print $2}' | cut -d/ -f1)
    if [ -n "$IP" ]; then
        echo "✓ WiFi connected: $IP"
        echo "Access Content Manager at: http://$IP:5000"
    else
        echo "✗ WiFi interface exists but no IP assigned"
    fi
else
    echo "✗ WiFi interface not found"
fi
EOF

chmod +x /opt/magic_dingus_box/scripts/usb_gadget_status.sh
echo "  ✓ Created /opt/magic_dingus_box/scripts/usb_gadget_status.sh"

echo ""
echo "=== Setup Complete ==="
echo ""
echo "IMPORTANT: You must REBOOT for changes to take effect!"
echo ""
echo "After reboot, USB Ethernet Gadget mode will be active."
echo ""
echo "To use:"
echo "  1. Connect USB-C cable from laptop to Pi's USB-C port"
echo "  2. Wait 5-10 seconds for the network to initialize"
echo "  3. Open browser to: http://192.168.7.1:5000"
echo ""
echo "WiFi access will continue to work as before."
echo ""
read -p "Reboot now? (y/N): " answer
if [[ "$answer" =~ ^[Yy]$ ]]; then
    echo "Rebooting..."
    reboot
else
    echo "Please reboot manually when ready: sudo reboot"
fi
