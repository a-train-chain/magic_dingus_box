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

echo "✓ All dependencies installed!"

