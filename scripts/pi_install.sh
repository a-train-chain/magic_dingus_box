#!/bin/bash
set -euo pipefail

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

sudo install -m 0644 /opt/magic_dingus_box/systemd/magic-mpv.service /etc/systemd/system/
sudo install -m 0644 /opt/magic_dingus_box/systemd/magic-ui.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable magic-mpv.service magic-ui.service

sudo mkdir -p /data/{playlists,media,logs}

echo ""
echo "Install complete!"
echo ""
echo "⚠️  IMPORTANT: Configure GPU memory for hardware video decoding"
echo ""
echo "Edit /boot/config.txt and add:"
echo "  gpu_mem=512"
echo "  start_x=1"
echo ""
echo "Then reboot: sudo reboot"
echo ""
echo "See boot_config_template.txt for full configuration options."

