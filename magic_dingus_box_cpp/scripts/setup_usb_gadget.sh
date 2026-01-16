#!/usr/bin/env bash
#
# Setup USB Ethernet Gadget Mode for Magic Dingus Box
# This allows direct USB connection from a laptop for faster uploads
#
# Usage: sudo ./setup_usb_gadget.sh
#
# After running, reboot the Pi. Then:
#   - Connect USB-C cable from laptop to Pi
#   - Access web manager at http://10.55.0.1:5000
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

# Step 2: Add modules-load to cmdline.txt (more reliable than /etc/modules)
echo "Step 2: Configuring kernel modules in cmdline.txt..."

# Detect cmdline.txt location
if [ -f /boot/firmware/cmdline.txt ]; then
    CMDLINE_TXT="/boot/firmware/cmdline.txt"
elif [ -f /boot/cmdline.txt ]; then
    CMDLINE_TXT="/boot/cmdline.txt"
else
    echo "ERROR: Cannot find cmdline.txt"
    exit 1
fi

if grep -q "modules-load=dwc2,g_ether" "$CMDLINE_TXT"; then
    echo "  ✓ modules-load already configured in cmdline.txt"
else
    # Add modules-load after rootwait
    sed -i 's/rootwait/rootwait modules-load=dwc2,g_ether/' "$CMDLINE_TXT"
    echo "  ✓ Added modules-load=dwc2,g_ether to cmdline.txt"
fi

# Step 3: Create NetworkManager connection for usb0
echo "Step 3: Configuring static IP for USB network via NetworkManager..."

# Check if connection already exists
if nmcli connection show usb0 &>/dev/null; then
    echo "  ✓ NetworkManager 'usb0' connection already exists"
else
    nmcli connection add type ethernet con-name usb0 ifname usb0 \
        ipv4.method manual ipv4.addresses 10.55.0.1/24
    nmcli connection modify usb0 connection.autoconnect yes
    echo "  ✓ Created NetworkManager connection for usb0 with IP 10.55.0.1"
fi

# Step 5: Create a helper script to check USB gadget status
echo "Step 5: Creating status check script..."
cat > /opt/magic_dingus_box/scripts/usb_gadget_status.sh << 'EOF'
#!/bin/bash
# Check USB Ethernet Gadget status
# Provides detailed diagnostics and platform-specific troubleshooting

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║           Magic Dingus Box - Connection Status               ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

# Check USB interface
echo "USB Connection:"
echo "───────────────"
if ip link show usb0 &>/dev/null; then
    STATE=$(cat /sys/class/net/usb0/carrier 2>/dev/null || echo "0")
    OPERSTATE=$(cat /sys/class/net/usb0/operstate 2>/dev/null || echo "unknown")
    
    if [ "$STATE" = "1" ] && [ "$OPERSTATE" = "up" ]; then
        echo "  ✓ USB cable connected and active"
        IP=$(ip addr show usb0 | grep "inet " | awk '{print $2}' | cut -d/ -f1)
        echo "  ✓ Pi IP: ${IP:-10.55.0.1}"
        echo ""
        echo "  → Access Content Manager at: http://${IP:-10.55.0.1}:5000"
    else
        echo "  ⚠ USB interface exists but no cable detected"
        echo ""
        echo "  To connect:"
        echo "    1. Use a USB-C data cable (not charge-only)"
        echo "    2. Connect your laptop to Pi's USB-C port"
        echo "    3. Wait 5-10 seconds"
    fi
else
    echo "  ✗ USB interface not found"
    echo "  → USB gadget mode may not be configured. Run setup_usb_gadget.sh"
fi

echo ""
echo "WiFi Connection:"
echo "────────────────"
if ip link show wlan0 &>/dev/null; then
    IP=$(ip addr show wlan0 | grep "inet " | awk '{print $2}' | cut -d/ -f1)
    SSID=$(iwgetid -r 2>/dev/null || echo "")
    if [ -n "$IP" ]; then
        echo "  ✓ Connected to: $SSID"
        echo "  ✓ IP: $IP"
        echo ""
        echo "  → Access Content Manager at: http://$IP:5000"
    else
        echo "  ⚠ WiFi interface up but not connected"
    fi
else
    echo "  ✗ WiFi interface not found"
fi

echo ""
echo "Web Service:"
echo "────────────"
if systemctl is-active --quiet magic-dingus-web.service; then
    echo "  ✓ Content Manager running on port 5000"
else
    echo "  ✗ Content Manager not running"
    echo "  → Start with: sudo systemctl start magic-dingus-web"
fi

# Platform-specific help if USB is up but no carrier
STATE=$(cat /sys/class/net/usb0/carrier 2>/dev/null || echo "0")
if [ "$STATE" = "0" ] && ip link show usb0 &>/dev/null; then
    echo ""
    echo "Troubleshooting (if your computer is connected):"
    echo "─────────────────────────────────────────────────"
    echo ""
    echo "  macOS:"
    echo "    System Preferences → Network → RNDIS/Ethernet Gadget"
    echo "    Set 'Configure IPv4' to 'Using DHCP' → Apply"
    echo "    Or run: sudo networksetup -setdhcp 'RNDIS/Ethernet Gadget'"
    echo ""
    echo "  Windows:"
    echo "    Usually auto-configures. If not, check Device Manager."
    echo ""
    echo "  Linux:"
    echo "    Run: sudo dhclient usb0"
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
echo "  3. Open browser to: http://10.55.0.1:5000"
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
