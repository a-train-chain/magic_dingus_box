#!/usr/bin/env bash
# Quick script to install all dependencies on Pi
#
# Usage:
#   ./install_deps.sh                  # install base kiosk deps
#   ./install_deps.sh --media-browser  # also install Media Browser deps

set -e

INCLUDE_MEDIA_BROWSER=0
for arg in "$@"; do
    case "$arg" in
        --media-browser) INCLUDE_MEDIA_BROWSER=1 ;;
    esac
done

echo "Installing C++ Kiosk Engine dependencies..."

sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  ninja-build \
  pkg-config \
  libdrm-dev \
  libgbm-dev \
  libegl1-mesa-dev \
  libgles2-mesa-dev \
  libevdev-dev \
  libgpiod-dev \
  libyaml-cpp-dev \
  libjsoncpp-dev \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  libgstreamer-plugins-bad1.0-dev \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly \
  gstreamer1.0-gl \
  gstreamer1.0-libav \
  python3-evdev \
  python3-pip \
  dnsmasq

# Phone Remote — flask-sock for WebSocket support is not in Debian Bookworm
# repos. Install via pip with --break-system-packages so the system python
# (which /usr/bin/python3 -m magic_dingus_box.web.wsgi uses) can import it.
echo "Installing flask-sock for Phone Remote WebSocket support..."
sudo pip3 install --break-system-packages 'flask-sock>=0.7.0'

# dnsmasq is the DHCP server for the USB-Gadget (usb0) interface.
# Without it, an operator plugging in a Mac/PC via USB-C wouldn't get
# an IP automatically — they'd have to manually configure 10.55.0.2/24.
# With dnsmasq listening on usb0, the host receives 10.55.0.10+ and can
# reach the Content Manager at http://10.55.0.1:5000 immediately.
#
# Config is shipped from this repo at scripts/data/dnsmasq-usb0.conf
# and dropped into /etc/dnsmasq.d/ (additive — doesn't affect the base
# /etc/dnsmasq.conf, which stays whatever Debian ships with). The
# config restricts dnsmasq to listen ONLY on usb0 and disables the DNS
# server (port=0) so it doesn't fight systemd-resolved on wlan0.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
USB_DNSMASQ_CONF="${SCRIPT_DIR}/data/dnsmasq-usb0.conf"
if [[ -f "$USB_DNSMASQ_CONF" ]]; then
    echo "Configuring dnsmasq for USB-Gadget DHCP..."
    sudo cp "$USB_DNSMASQ_CONF" /etc/dnsmasq.d/usb0.conf
    sudo chmod 644 /etc/dnsmasq.d/usb0.conf
    sudo systemctl enable dnsmasq.service
    sudo systemctl restart dnsmasq.service
    echo "  ✓ dnsmasq configured and running on usb0"
else
    echo "  ⚠ ${USB_DNSMASQ_CONF} not found — skipping dnsmasq config"
fi

# Content Manager port-80 redirect: tiny Python service that 302s any
# request from port 80 to port 5000 (where Flask actually listens).
# Lets the operator type `magicpi.local` (or `10.55.0.1`, or the Pi's
# Wi-Fi IP) without remembering the port number — landing on the
# Content Manager directly.
REDIRECT_UNIT_SRC="$(cd "${SCRIPT_DIR}/.." && pwd)/systemd/content-manager-redirect.service"
if [[ -f "$REDIRECT_UNIT_SRC" ]]; then
    echo "Installing content-manager-redirect service..."
    sudo cp "$REDIRECT_UNIT_SRC" /etc/systemd/system/content-manager-redirect.service
    sudo chmod 644 /etc/systemd/system/content-manager-redirect.service
    sudo systemctl daemon-reload
    sudo systemctl enable content-manager-redirect.service
    sudo systemctl restart content-manager-redirect.service
    echo "  ✓ port-80 → :5000 redirect running"
else
    echo "  ⚠ ${REDIRECT_UNIT_SRC} not found — skipping redirect service"
fi

if [[ $INCLUDE_MEDIA_BROWSER -eq 1 ]]; then
    echo "Installing Media Browser dependencies..."
    sudo apt install -y \
      libtorrent-rasterbar-dev \
      libsqlite3-dev \
      libcurl4-openssl-dev \
      libpugixml-dev
fi

echo "✓ All dependencies installed!"
