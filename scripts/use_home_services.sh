#!/bin/bash
# Switch to using home directory service files
# Run this on your Pi if your code is in ~/magic_dingus_box

set -e

echo "═══════════════════════════════════════════════════════"
echo "  Switching to Home Directory Service Files"
echo "═══════════════════════════════════════════════════════"
echo ""

# Get the directory where the script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "📁 Project directory: $PROJECT_DIR"

# Check if venv exists in home directory
if [ ! -d "$HOME/magic_dingus_box/venv" ]; then
    echo "⚠️  Warning: Virtual environment not found at $HOME/magic_dingus_box/venv"
    echo "Creating virtual environment..."
    cd "$HOME/magic_dingus_box"
    python3 -m venv venv
    source venv/bin/activate
    pip install --upgrade pip
    pip install -r requirements.txt
    echo "✅ Virtual environment created"
fi

echo ""
echo "🔄 Stopping existing services..."
sudo systemctl stop magic-ui.service magic-mpv.service || true

echo ""
echo "📋 Deploying home directory service files..."
sudo cp "$PROJECT_DIR/systemd/magic-mpv-home.service" /etc/systemd/system/magic-mpv.service
sudo cp "$PROJECT_DIR/systemd/magic-ui-home.service" /etc/systemd/system/magic-ui.service

echo ""
echo "🔄 Reloading systemd daemon..."
sudo systemctl daemon-reload

echo ""
echo "✅ Enabling services for auto-start on boot..."
sudo systemctl enable magic-mpv.service
sudo systemctl enable magic-ui.service

echo ""
echo "🚀 Starting services..."
sudo systemctl start magic-mpv.service
sleep 2
sudo systemctl start magic-ui.service
sleep 2

echo ""
echo "📊 Service Status:"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if systemctl is-active --quiet magic-mpv.service; then
    echo "✅ magic-mpv.service is running"
else
    echo "❌ magic-mpv.service failed"
    sudo systemctl status magic-mpv.service --no-pager -n 10
fi

if systemctl is-active --quiet magic-ui.service; then
    echo "✅ magic-ui.service is running"
else
    echo "❌ magic-ui.service failed"
    sudo systemctl status magic-ui.service --no-pager -n 10
fi

echo ""
echo "═══════════════════════════════════════════════════════"
echo "✨ Setup complete!"
echo ""
echo "Services will now start automatically on boot."
echo "To view logs:"
echo "  journalctl -u magic-mpv.service -f"
echo "  journalctl -u magic-ui.service -f"
echo "═══════════════════════════════════════════════════════"

