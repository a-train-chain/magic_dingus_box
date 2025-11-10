#!/bin/bash
set -euo pipefail

# Quick deployment script to sync changes to Pi and restart services
# Usage: ./scripts/deploy_to_pi.sh [pi_hostname_or_ip]

PI_HOST="${1:-raspberrypi.local}"
PI_USER="${2:-pi}"
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

# Restart services to pick up changes
sudo systemctl daemon-reload
sudo systemctl restart magic-mpv.service magic-ui.service

echo "Deployment complete. Services restarted."
EOF

echo "Done! Services restarted on Pi."

