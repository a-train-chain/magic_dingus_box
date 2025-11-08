#!/bin/bash
set -euo pipefail

USER_NAME="${1:-$USER}"
USER_UID="$(id -u "$USER_NAME" 2>/dev/null || echo 1000)"

sudo apt-get update
sudo apt-get install -y \
  python3 python3-venv python3-pip \
  mpv python3-gpiozero \
  libsdl2-2.0-0 libsdl2-image-2.0-0 libsdl2-ttf-2.0-0 \
  libasound2

python3 -m venv /opt/magic_dingus_box/.venv
source /opt/magic_dingus_box/.venv/bin/activate
pip install --upgrade pip
pip install -r /opt/magic_dingus_box/requirements.txt

# Render systemd services for target user
sudo bash -lc "sed -e 's/^User=.*/User=$USER_NAME/' \
  -e 's/^Group=.*/Group=$USER_NAME/' \
  -e 's|^Environment=XDG_RUNTIME_DIR=.*|Environment=XDG_RUNTIME_DIR=/run/user/$USER_UID|' \
  -e 's|^Environment=XAUTHORITY=.*|Environment=XAUTHORITY=/home/$USER_NAME/.Xauthority|' \
  -e 's/--hwdec=auto-safe/--hwdec=auto-copy/' \
  /opt/magic_dingus_box/systemd/magic-mpv.service > /etc/systemd/system/magic-mpv.service"

sudo bash -lc "sed -e 's/^User=.*/User=$USER_NAME/' \
  -e 's/^Group=.*/Group=$USER_NAME/' \
  -e 's|^Environment=XDG_RUNTIME_DIR=.*|Environment=XDG_RUNTIME_DIR=/run/user/$USER_UID|' \
  -e 's|^Environment=XAUTHORITY=.*|Environment=XAUTHORITY=/home/$USER_NAME/.Xauthority|' \
  /opt/magic_dingus_box/systemd/magic-ui.service > /etc/systemd/system/magic-ui.service"

sudo systemctl daemon-reload
sudo systemctl enable magic-mpv.service magic-ui.service

sudo mkdir -p /data/{playlists,media,logs}
echo "Install complete. Configure /boot/config.txt for NTSC composite and reboot."

