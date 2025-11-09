#!/bin/bash
# Install VLC with hardware acceleration support for Raspberry Pi
# This script installs minimal VLC components for headless operation

set -e

echo "Installing VLC with hardware acceleration support..."

# Update package list
sudo apt update

# Install VLC core components without GUI dependencies
sudo apt install -y --no-install-recommends \
    vlc-bin \
    vlc-plugin-base \
    python3-vlc

echo ""
echo "VLC installation complete!"
echo ""
echo "Checking GPU memory allocation..."

# Check GPU memory setting
GPU_MEM=$(vcgencmd get_mem gpu | cut -d= -f2 | cut -d M -f1)
echo "Current GPU memory: ${GPU_MEM}M"

if [ "$GPU_MEM" -lt 256 ]; then
    echo ""
    echo "WARNING: GPU memory is less than 256MB"
    echo "For smooth video playback, you should allocate at least 256MB (512MB recommended for 1080p)"
    echo ""
    echo "To increase GPU memory:"
    echo "  1. Edit /boot/firmware/config.txt (or /boot/config.txt on older systems)"
    echo "  2. Add or modify: gpu_mem=512"
    echo "  3. Reboot"
fi

echo ""
echo "Testing VLC installation..."

# Test VLC version
VLC_VERSION=$(cvlc --version 2>&1 | head -n 1)
echo "VLC version: $VLC_VERSION"

# Test python-vlc
echo ""
echo "Testing python-vlc library..."
python3 -c "import vlc; print('python-vlc version:', vlc.libvlc_get_version().decode())" || {
    echo "ERROR: python-vlc import failed"
    exit 1
}

echo ""
echo "VLC is ready for hardware-accelerated playback!"
echo ""
echo "Next steps:"
echo "  1. Ensure gpu_mem is set to 512 in /boot/firmware/config.txt"
echo "  2. Reboot if you changed GPU memory"
echo "  3. Test playback: cvlc --fullscreen --vout=drm /path/to/video.mp4"

