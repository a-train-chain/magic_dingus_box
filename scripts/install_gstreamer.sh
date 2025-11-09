#!/bin/bash

echo "Installing GStreamer with hardware acceleration support..."

# Update package list
sudo apt update

# Install GStreamer core and plugins
sudo apt install -y \
    gstreamer1.0-tools \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    gstreamer1.0-x \
    gstreamer1.0-alsa \
    python3-gst-1.0 \
    gstreamer1.0-gl \
    gstreamer1.0-gtk3

# Install V4L2 hardware decode plugin (for Raspberry Pi hardware acceleration)
sudo apt install -y \
    gstreamer1.0-omx \
    gstreamer1.0-v4l2

echo "GStreamer installation complete!"

echo "Checking GPU memory allocation..."
GPU_MEM=$(vcgencmd get_config gpu_mem | cut -d'=' -f2)
echo "Current GPU memory: ${GPU_MEM}M"

if [ "$GPU_MEM" -lt 128 ]; then
  echo ""
  echo "WARNING: GPU memory is less than 128MB"
  echo "For smooth video playback, you should allocate at least 128MB (256MB recommended)"
  echo ""
  echo "To increase GPU memory:"
  echo "  1. Edit /boot/firmware/config.txt (or /boot/config.txt on older systems)"
  echo "  2. Add or modify: gpu_mem=256"
  echo "  3. Reboot"
  echo ""
fi

echo "Testing GStreamer installation..."
gst-launch-1.0 --version | head -n 1

echo "Testing python-gst library..."
python3 -c "import gi; gi.require_version('Gst', '1.0'); from gi.repository import Gst; Gst.init(None); print(f'GStreamer version: {Gst.version_string()}')"

echo "GStreamer is ready for hardware-accelerated playback!"

echo "Next steps:"
echo "  1. Ensure gpu_mem is set appropriately in /boot/firmware/config.txt"
echo "  2. Reboot if you changed GPU memory"
echo "  3. Test playback: gst-launch-1.0 playbin uri=file:///path/to/video.mp4"

