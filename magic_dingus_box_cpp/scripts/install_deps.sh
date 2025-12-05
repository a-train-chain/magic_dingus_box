#!/usr/bin/env bash
# Quick script to install all dependencies on Pi

set -e

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
  libyaml-cpp-dev \
  libjsoncpp-dev \
  libmpv-dev

echo "✓ All dependencies installed!"

