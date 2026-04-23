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
  gstreamer1.0-gl

if [[ $INCLUDE_MEDIA_BROWSER -eq 1 ]]; then
    echo "Installing Media Browser dependencies..."
    sudo apt install -y \
      libtorrent-rasterbar-dev \
      libsqlite3-dev \
      libcurl4-openssl-dev \
      libpugixml-dev
fi

echo "✓ All dependencies installed!"
