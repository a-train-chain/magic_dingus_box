#!/bin/bash
# Setup desktop autostart (alternative to systemd services)

set -e

echo "═══════════════════════════════════════════════════════"
echo "  Setup Desktop Autostart for Magic Dingus Box"
echo "═══════════════════════════════════════════════════════"
echo ""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Create autostart directory
mkdir -p ~/.config/autostart

# Copy desktop file
echo "📋 Installing autostart desktop file..."
cp "$PROJECT_DIR/autostart/magic-dingus-box.desktop" ~/.config/autostart/

# Make startup script executable
chmod +x "$PROJECT_DIR/scripts/start_app.sh"

echo ""
echo "✅ Desktop autostart configured!"
echo ""
echo "The app will now start automatically when you login."
echo ""
echo "To disable autostart:"
echo "  rm ~/.config/autostart/magic-dingus-box.desktop"
echo ""
echo "To test without rebooting:"
echo "  $PROJECT_DIR/scripts/start_app.sh"
echo "═══════════════════════════════════════════════════════"

