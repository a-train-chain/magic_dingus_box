#!/bin/bash
# Magic Dingus Box - Auto Update and Restart Script
# Run this on your Pi to pull latest changes and restart services

set -e  # Exit on error

echo "═══════════════════════════════════════════════════════"
echo "  Magic Dingus Box - Update & Restart"
echo "═══════════════════════════════════════════════════════"
echo ""

# Get the directory where the script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "📁 Project directory: $PROJECT_DIR"
cd "$PROJECT_DIR"

# Check if we're in a git repository
if [ ! -d ".git" ]; then
    echo "❌ Error: Not in a git repository"
    exit 1
fi

echo ""
echo "⬇️  Pulling latest changes from GitHub..."
git fetch origin
git pull origin main

echo ""
echo "🔍 Checking GPU memory allocation..."

# Detect correct boot config location
if [ -f /boot/firmware/config.txt ]; then
    BOOT_CONFIG="/boot/firmware/config.txt"
elif [ -f /boot/config.txt ]; then
    BOOT_CONFIG="/boot/config.txt"
else
    BOOT_CONFIG="/boot/config.txt or /boot/firmware/config.txt"
fi

GPU_MEM=$(vcgencmd get_mem gpu | cut -d= -f2 | cut -d M -f1)
echo "   Current GPU memory: ${GPU_MEM}MB"

if [ "$GPU_MEM" -lt 128 ]; then
    echo "⚠️  WARNING: GPU memory is less than 128MB!"
    echo "   Video decoding may not work properly."
    echo "   Recommended: Edit $BOOT_CONFIG and add:"
    echo "   gpu_mem=512"
    echo "   start_x=1"
    echo ""
    read -p "Continue anyway? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
elif [ "$GPU_MEM" -lt 512 ]; then
    echo "⚠️  GPU memory is adequate but 512MB recommended for best performance"
    echo "   Edit $BOOT_CONFIG and set: gpu_mem=512"
else
    echo "✅ GPU memory allocation looks good"
fi

echo ""
echo "📋 Deploying updated service files..."

# Copy service files if they exist
if [ -f "$PROJECT_DIR/systemd/magic-mpv.service" ]; then
    sudo cp "$PROJECT_DIR/systemd/magic-mpv.service" /etc/systemd/system/
    echo "   ✓ magic-mpv.service"
fi

if [ -f "$PROJECT_DIR/systemd/magic-ui.service" ]; then
    sudo cp "$PROJECT_DIR/systemd/magic-ui.service" /etc/systemd/system/
    echo "   ✓ magic-ui.service"
fi

echo ""
echo "🔄 Reloading systemd daemon..."
sudo systemctl daemon-reload

echo ""
echo "🔃 Restarting services..."
echo "   Stopping services..."
sudo systemctl stop magic-ui.service || true
sudo systemctl stop magic-mpv.service || true

sleep 2

echo "   Starting mpv service..."
sudo systemctl start magic-mpv.service

sleep 2

echo "   Starting UI service..."
sudo systemctl start magic-ui.service

sleep 2

echo ""
echo "✅ Services restarted!"
echo ""
echo "📊 Service Status:"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# Check MPV service
if systemctl is-active --quiet magic-mpv.service; then
    echo "✅ magic-mpv.service is running"
else
    echo "❌ magic-mpv.service failed to start"
    sudo systemctl status magic-mpv.service --no-pager -n 10
fi

# Check UI service
if systemctl is-active --quiet magic-ui.service; then
    echo "✅ magic-ui.service is running"
else
    echo "❌ magic-ui.service failed to start"
    sudo systemctl status magic-ui.service --no-pager -n 10
fi

echo ""
echo "═══════════════════════════════════════════════════════"
echo "✨ Update complete!"
echo ""
echo "Services Status:"
if systemctl is-active --quiet magic-mpv.service && systemctl is-active --quiet magic-ui.service; then
    echo "  ✅ Both services are running"
else
    echo "  ⚠️  Services may need manual start:"
    echo "     sudo systemctl start magic-mpv.service magic-ui.service"
fi
echo ""
echo "To view logs:"
echo "  journalctl -u magic-mpv.service -f"
echo "  journalctl -u magic-ui.service -f"
echo ""
echo "To check for hardware decoding:"
echo "  journalctl -u magic-mpv.service -n 50 | grep -i 'hwdec\\|hardware'"
echo ""
echo "📺 If you see 'no signal' on your monitor:"
echo "   Run: ./scripts/fix_hdmi.sh"
echo "   Then: sudo reboot"
echo "═══════════════════════════════════════════════════════"

