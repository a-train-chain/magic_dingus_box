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
  libsystemd-dev \
  pulseaudio \
  pulseaudio-utils \
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

# --- Persistent journal -------------------------------------------------------
# Without this the box cannot explain its own reboots.
#
# Raspberry Pi OS deliberately ships
# /usr/lib/systemd/journald.conf.d/40-rpi-volatile-storage.conf with
# `Storage=volatile`, to spare the SD card. The consequence is that the journal
# lives entirely in /run (RAM) and every reboot erases it. Observed 2026-07-29
# on a Pi 5 that rebooted mid-deploy: /var/log/journal existed but held 0 journal
# files, `journalctl --disk-usage` reported 6.5 MB all under /run, and
# `journalctl --list-boots` knew exactly one boot. There was no prior boot, no
# shutdown reason and no watchdog history — the cause of that reboot is
# permanently unknowable. On a unit in a customer's house that is the difference
# between "it rebooted, here is why" and a shrug.
#
# We override the distro default ON PURPOSE, and mitigate the thing it was
# protecting: the caps below bound total size (200M), per-file size (20M), file
# count (10) and age (1 month), and journald compresses by default. A kiosk emits
# a few MB per boot, so this buys several boots of history for a trivial amount
# of flash wear. If SD longevity ever becomes the pressing concern, lower
# SystemMaxUse rather than reverting to volatile — losing all history is not a
# wear-levelling strategy.
#
# THE FILENAME PREFIX IS LOAD-BEARING. Drop-ins apply in lexical order and the
# last assignment wins, so this MUST sort after 40-rpi-volatile-storage.conf.
# A first attempt named 10-mdb-persistent.conf was silently overridden by the
# distro file and the journal stayed in RAM — `systemd-analyze cat-config
# systemd/journald.conf` showed Storage=persistent followed by Storage=volatile.
# Hence 99-.
JOURNALD_DROPIN=/etc/systemd/journald.conf.d/99-mdb-persistent.conf
if [[ -d /etc/systemd ]]; then
    if [[ -f "$JOURNALD_DROPIN" ]] \
       && grep -qE '^\s*Storage\s*=\s*persistent' "$JOURNALD_DROPIN" \
       && [[ -n "$(find /var/log/journal -name '*.journal' -print -quit 2>/dev/null)" ]]; then
        echo "Persistent journal already configured."
    else
        echo "Enabling persistent journal (so reboots stay diagnosable)..."
        # Remove the mis-ordered name an earlier version of this script wrote,
        # so a box provisioned with it does not keep a dead file around.
        sudo rm -f /etc/systemd/journald.conf.d/10-mdb-persistent.conf
        sudo mkdir -p /etc/systemd/journald.conf.d
        sudo tee "$JOURNALD_DROPIN" >/dev/null <<'JOURNALCFG'
# Managed by Magic Dingus Box install_deps.sh — see the rationale there.
#
# Overrides Raspberry Pi OS's 40-rpi-volatile-storage.conf. The 99- prefix is
# required: drop-ins load in lexical order and the last assignment wins, so a
# lower number is silently beaten by the distro's Storage=volatile.
[Journal]
Storage=persistent
SystemMaxUse=200M
SystemMaxFileSize=20M
SystemMaxFiles=10
MaxRetentionSec=1month
JOURNALCFG
        sudo mkdir -p /var/log/journal
        sudo systemd-tmpfiles --create --prefix /var/log/journal 2>/dev/null || true
        sudo systemctl restart systemd-journald
        # Move what is currently in /run onto disk so THIS boot is retained too,
        # rather than the history starting at the next reboot.
        sudo journalctl --flush 2>/dev/null || true
        if [[ -n "$(find /var/log/journal -name '*.journal' -print -quit 2>/dev/null)" ]]; then
            echo "  ✓ journal now persists across reboots"
        else
            echo "  ⚠ journald restarted but no on-disk journal appeared."
            echo "    Check load order:  systemd-analyze cat-config systemd/journald.conf"
            echo "    A later drop-in setting Storage=volatile will win over this one."
        fi
    fi
else
    echo "  ⚠ /etc/systemd not present — skipping persistent-journal setup"
fi

echo "✓ All dependencies installed!"
