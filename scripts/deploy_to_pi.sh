#!/bin/bash
set -euo pipefail

# Quick deployment script to sync changes to Pi and restart services
# Usage: ./scripts/deploy_to_pi.sh [pi_hostname_or_ip]

PI_HOST="${1:-magicpi.local}"
PI_USER="${2:-alexanderchaney}"
APP_DIR="/opt/magic_dingus_box"

echo "Deploying to $PI_USER@$PI_HOST..."

# Sync code changes
rsync -avz --delete \
  --exclude='.git' \
  --exclude='venv' \
  --exclude='__pycache__' \
  --exclude='*.pyc' \
  --exclude='dev_data' \
  --exclude='dist' \
  ./ "$PI_USER@$PI_HOST:$APP_DIR/"

# On the Pi: pull latest, restart services
ssh "$PI_USER@$PI_HOST" bash <<EOF
set -e
cd $APP_DIR

# Pull latest from git (if using git deployment)
if [ -d .git ]; then
  git pull origin main || echo "Git pull failed, continuing with rsync'd files"
fi

# Ensure wrapper script and RetroPie install script are executable
if [ -f scripts/launch_retroarch.sh ]; then
  chmod +x scripts/launch_retroarch.sh
  echo "Made wrapper script executable"
fi
if [ -f scripts/install_retropie_cores.sh ]; then
  chmod +x scripts/install_retropie_cores.sh
  echo "Made RetroPie install script executable"
fi

# Check if RetroArch is installed
if [ ! -f /usr/bin/retroarch ] && [ ! -f /opt/retropie/emulators/retroarch/bin/retroarch ]; then
  echo "WARNING: RetroArch not found. Install RetroPie cores with: sudo bash scripts/install_retropie_cores.sh"
fi

# Restart services to pick up changes
sudo systemctl daemon-reload
systemctl --user restart magic-ui.service || \
  sudo systemctl restart magic-mpv-x11.service magic-ui-x11.service || \
sudo systemctl restart magic-mpv.service magic-ui.service

echo "Deployment complete. Services restarted."
EOF

echo "Done! Services restarted on Pi."

