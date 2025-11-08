#!/bin/bash
# Fix HDMI "no signal" issue by configuring boot config properly

set -e

echo "═══════════════════════════════════════════════════════"
echo "  Fix HDMI Display Output"
echo "═══════════════════════════════════════════════════════"
echo ""

# Detect boot config location
if [ -f /boot/firmware/config.txt ]; then
    BOOT_CONFIG="/boot/firmware/config.txt"
elif [ -f /boot/config.txt ]; then
    BOOT_CONFIG="/boot/config.txt"
else
    echo "❌ Error: Cannot find boot config file"
    exit 1
fi

echo "📝 Boot config: $BOOT_CONFIG"
echo ""

# Backup original
echo "💾 Creating backup..."
sudo cp "$BOOT_CONFIG" "${BOOT_CONFIG}.backup.$(date +%Y%m%d_%H%M%S)"

echo ""
echo "🔧 Applying HDMI fixes..."

# Function to add or update config line
add_or_update_config() {
    local key="$1"
    local value="$2"
    
    if sudo grep -q "^${key}=" "$BOOT_CONFIG"; then
        # Update existing
        sudo sed -i "s/^${key}=.*/${key}=${value}/" "$BOOT_CONFIG"
        echo "   ✓ Updated: ${key}=${value}"
    elif sudo grep -q "^#${key}=" "$BOOT_CONFIG"; then
        # Uncomment and update
        sudo sed -i "s/^#${key}=.*/${key}=${value}/" "$BOOT_CONFIG"
        echo "   ✓ Enabled: ${key}=${value}"
    else
        # Add new
        echo "${key}=${value}" | sudo tee -a "$BOOT_CONFIG" > /dev/null
        echo "   ✓ Added: ${key}=${value}"
    fi
}

# Remove/disable composite video settings that conflict with HDMI
echo ""
echo "🚫 Removing conflicting settings..."
sudo sed -i 's/^enable_tvout=/#enable_tvout=/' "$BOOT_CONFIG" 2>/dev/null || true
sudo sed -i 's/^sdtv_mode=/#sdtv_mode=/' "$BOOT_CONFIG" 2>/dev/null || true
sudo sed -i 's/^sdtv_aspect=/#sdtv_aspect=/' "$BOOT_CONFIG" 2>/dev/null || true
sudo sed -i 's/^hdmi_ignore_hotplug=/#hdmi_ignore_hotplug=/' "$BOOT_CONFIG" 2>/dev/null || true

# Remove OLD generic HDMI settings (without :0) that conflict with port-specific settings
echo "🧹 Removing duplicate generic HDMI settings..."
sudo sed -i '/^hdmi_force_hotplug=[0-9]$/d' "$BOOT_CONFIG" 2>/dev/null || true
sudo sed -i '/^hdmi_drive=[0-9]$/d' "$BOOT_CONFIG" 2>/dev/null || true
sudo sed -i '/^hdmi_group=[0-9]$/d' "$BOOT_CONFIG" 2>/dev/null || true
sudo sed -i '/^hdmi_mode=[0-9]$/d' "$BOOT_CONFIG" 2>/dev/null || true

echo ""
echo "✅ Configuring HDMI settings..."

# Required settings
add_or_update_config "gpu_mem" "512"
add_or_update_config "start_x" "1"

# HDMI force settings for port 0 (port closest to power/USB-C)
# Using :0 suffix to explicitly target HDMI port 0
# Only force hotplug and drive mode - let Pi auto-detect resolution
add_or_update_config "hdmi_force_hotplug:0" "1"
add_or_update_config "hdmi_drive:0" "2"

# Remove any forced resolution settings that may be incompatible (including port-specific ones)
sudo sed -i 's/^hdmi_group:0=/#hdmi_group:0=/' "$BOOT_CONFIG" 2>/dev/null || true
sudo sed -i 's/^hdmi_mode:0=/#hdmi_mode:0=/' "$BOOT_CONFIG" 2>/dev/null || true
sudo sed -i 's/^hdmi_group/#hdmi_group/' "$BOOT_CONFIG" 2>/dev/null || true
sudo sed -i 's/^hdmi_mode/#hdmi_mode/' "$BOOT_CONFIG" 2>/dev/null || true

echo ""
echo "═══════════════════════════════════════════════════════"
echo "✨ HDMI configuration complete!"
echo ""
echo "Settings applied:"
echo "  • GPU Memory: 512MB (for video decoding)"
echo "  • H.264 codec enabled"
echo "  • HDMI port 0 forced on (port closest to power)"
echo "  • Resolution: Auto-detect (best for your display)"
echo ""
echo "⚠️  You MUST reboot for changes to take effect:"
echo "     sudo reboot"
echo ""
echo "After reboot, check:"
echo "  vcgencmd get_mem gpu     # Should show 512M"
echo ""
echo "To verify clean config:"
echo "  sudo cat $BOOT_CONFIG | grep -v '^#' | grep hdmi"
echo "  # Should only show port-specific settings (:0)"
echo "═══════════════════════════════════════════════════════"

