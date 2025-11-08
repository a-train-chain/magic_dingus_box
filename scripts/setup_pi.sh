#!/usr/bin/env bash
set -euo pipefail

# Usage: bash scripts/setup_pi.sh [username]
# If username omitted, current $USER is used.

USER_NAME="${1:-$USER}"
USER_UID="$(id -u "$USER_NAME" 2>/dev/null || echo 1000)"
APP_DIR="/opt/magic_dingus_box"
DATA_DIR="/data"

echo "[1/6] Installing packages..."
sudo apt update
sudo apt install -y \
  xserver-xorg lightdm lightdm-gtk-greeter openbox x11-xserver-utils \
  python3-venv python3-pip mpv jq rsync \
  libsdl2-2.0-0 libsdl2-image-2.0-0 libsdl2-ttf-2.0-0 \
  python3-gpiozero || true

sudo systemctl set-default graphical.target
sudo systemctl enable --now lightdm

echo "[2/6] Enabling LightDM autologin for $USER_NAME..."
sudo mkdir -p /etc/lightdm/lightdm.conf.d
sudo tee /etc/lightdm/lightdm.conf.d/12-autologin.conf >/dev/null <<EOF
[Seat:*]
autologin-user=$USER_NAME
autologin-session=openbox
EOF

echo "[3/6] Deploying app to $APP_DIR and creating venv..."
sudo mkdir -p "$APP_DIR"
sudo rsync -a --delete ./ "$APP_DIR"/
sudo chown -R "$USER_NAME":"$USER_NAME" "$APP_DIR"

sudo -u "$USER_NAME" -H bash -lc "python3 -m venv '$APP_DIR/venv'"
sudo -u "$USER_NAME" -H bash -lc "'$APP_DIR/venv/bin/pip' install -U pip wheel"
sudo -u "$USER_NAME" -H bash -lc "'$APP_DIR/venv/bin/pip' install -r '$APP_DIR/requirements.txt'"

echo "[4/6] Preparing data at $DATA_DIR ..."
sudo mkdir -p "$DATA_DIR"
sudo rsync -a "$APP_DIR/dev_data/" "$DATA_DIR/" || true

echo "[5/6] Installing systemd services (rendered for user $USER_NAME)..."
# Render units with correct user, XAUTHORITY and runtime dir
MPV_UNIT_DST="/etc/systemd/system/magic-mpv.service"
UI_UNIT_DST="/etc/systemd/system/magic-ui.service"

sudo bash -lc "sed -e 's/^User=.*/User=$USER_NAME/' \
  -e 's/^Group=.*/Group=$USER_NAME/' \
  -e 's|^Environment=XDG_RUNTIME_DIR=.*|Environment=XDG_RUNTIME_DIR=/run/user/$USER_UID|' \
  -e 's|^Environment=XAUTHORITY=.*|Environment=XAUTHORITY=/home/$USER_NAME/.Xauthority|' \
  -e 's/--hwdec=auto-safe/--hwdec=auto-copy/' \
  \"$APP_DIR/systemd/magic-mpv.service\" > \"$MPV_UNIT_DST\""

sudo bash -lc "sed -e 's/^User=.*/User=$USER_NAME/' \
  -e 's/^Group=.*/Group=$USER_NAME/' \
  -e 's|^Environment=XDG_RUNTIME_DIR=.*|Environment=XDG_RUNTIME_DIR=/run/user/$USER_UID|' \
  -e 's|^Environment=XAUTHORITY=.*|Environment=XAUTHORITY=/home/$USER_NAME/.Xauthority|' \
  \"$APP_DIR/systemd/magic-ui.service\" > \"$UI_UNIT_DST\""

sudo systemctl daemon-reload
sudo systemctl enable magic-mpv.service magic-ui.service
sudo systemctl restart magic-mpv.service magic-ui.service

echo "[6/6] Configuring HDMI and GPU memory in boot config..."
# Detect boot config path (Bookworm: /boot/firmware/config.txt, older: /boot/config.txt)
BOOT_CFG="/boot/firmware/config.txt"
if [ ! -f "$BOOT_CFG" ]; then
  BOOT_CFG="/boot/config.txt"
fi
sudo touch "$BOOT_CFG"
sudo cp "$BOOT_CFG" "${BOOT_CFG}.bak.$(date +%Y%m%d%H%M%S)" || true
ensure_cfg() {
  local key="$1"
  local value="$2"
  if ! grep -qE "^\s*${key}\s*=" "$BOOT_CFG"; then
    echo "${key}=${value}" | sudo tee -a "$BOOT_CFG" >/dev/null
  else
    sudo sed -i "s|^\s*${key}\s*=.*|${key}=${value}|" "$BOOT_CFG"
  fi
}
# KMS driver (default in modern Raspberry Pi OS)
ensure_cfg "dtoverlay" "vc4-kms-v3d-pi4"
ensure_cfg "max_framebuffers" "2"
# Force HDMI even if no EDID at boot, drive HDMI with audio
ensure_cfg "hdmi_force_hotplug" "1"
ensure_cfg "hdmi_drive" "2"
# Prefer 1080p60 as baseline
ensure_cfg "hdmi_group" "1"
ensure_cfg "hdmi_mode" "16"
# Boost signal if needed
ensure_cfg "config_hdmi_boost" "7"
# Allocate GPU memory (MB)
ensure_cfg "gpu_mem" "512"

echo "Done. App should appear on HDMI after autologin."
echo "You can check status with: sudo systemctl status magic-ui magic-mpv"
echo "If HDMI was previously blank, reboot is recommended: sudo reboot"

