#!/usr/bin/env bash
set -euo pipefail

# Usage: bash scripts/setup_pi.sh [username]
# If username omitted, current $USER is used.

USER_NAME="${1:-$USER}"
APP_DIR="/opt/magic_dingus_box"
DATA_DIR="/data"

echo "[1/5] Installing packages..."
sudo apt update
sudo apt install -y \
  xserver-xorg lightdm lightdm-gtk-greeter openbox x11-xserver-utils \
  git git-lfs python3-venv python3-pip mpv jq rsync

sudo systemctl set-default graphical.target
sudo systemctl enable --now lightdm
sudo -u "$USER_NAME" git lfs install || true

echo "[2/5] Deploying app to $APP_DIR and creating venv..."
sudo mkdir -p "$APP_DIR"
sudo rsync -a --delete ./ "$APP_DIR"/
sudo chown -R "$USER_NAME":"$USER_NAME" "$APP_DIR"

sudo -u "$USER_NAME" -H bash -lc "python3 -m venv '$APP_DIR/venv'"
sudo -u "$USER_NAME" -H bash -lc "'$APP_DIR/venv/bin/pip' install -U pip wheel"
sudo -u "$USER_NAME" -H bash -lc "'$APP_DIR/venv/bin/pip' install -r '$APP_DIR/requirements.txt'"

echo "[3/5] Preparing data at $DATA_DIR ..."
sudo mkdir -p "$DATA_DIR"
sudo rsync -a "$APP_DIR/dev_data/" "$DATA_DIR/" || true

echo "[4/5] Installing systemd services..."
sudo cp "$APP_DIR/systemd/magic-mpv.service" /etc/systemd/system/
sudo cp "$APP_DIR/systemd/magic-ui.service" /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable magic-mpv.service magic-ui.service
sudo systemctl restart magic-mpv.service magic-ui.service

echo "[5/5] Checking GPU memory configuration..."

# Detect correct boot config location
if [ -f /boot/firmware/config.txt ]; then
    BOOT_CONFIG="/boot/firmware/config.txt"
elif [ -f /boot/config.txt ]; then
    BOOT_CONFIG="/boot/config.txt"
else
    BOOT_CONFIG="/boot/config.txt (or /boot/firmware/config.txt)"
fi

GPU_MEM=$(vcgencmd get_mem gpu | cut -d= -f2 | cut -d M -f1)
if [ "$GPU_MEM" -lt 256 ]; then
    echo ""
    echo "⚠️  WARNING: GPU memory is only ${GPU_MEM}MB!"
    echo "    Hardware video decoding requires at least 256MB (512MB recommended)"
    echo ""
    echo "    To fix this, add to $BOOT_CONFIG:"
    echo "    gpu_mem=512"
    echo "    start_x=1"
    echo ""
    echo "    See boot_config_template.txt for full configuration"
    echo ""
elif [ "$GPU_MEM" -lt 512 ]; then
    echo "✅ GPU memory (${GPU_MEM}MB) is adequate, but 512MB recommended for best performance"
else
    echo "✅ GPU memory (${GPU_MEM}MB) is properly configured"
fi

echo ""
echo "Done! App should appear on HDMI after login."
echo "You can check status with: sudo systemctl status magic-ui magic-mpv"

