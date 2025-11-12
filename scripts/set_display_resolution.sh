#!/bin/bash
# Set display resolution to 1280x720 using xrandr (fallback method)
# This script can be run at startup if config.txt changes don't take effect

DISPLAY=:0
export DISPLAY

# Find the connected HDMI output
HDMI_OUTPUT=$(xrandr 2>/dev/null | grep -E "connected.*HDMI" | awk '{print $1}' | head -1)

if [ -z "$HDMI_OUTPUT" ]; then
    # Try to find any connected output
    HDMI_OUTPUT=$(xrandr 2>/dev/null | grep "connected" | awk '{print $1}' | head -1)
fi

if [ -z "$HDMI_OUTPUT" ]; then
    echo "ERROR: No display output found"
    exit 1
fi

echo "Setting display resolution to 1280x720 on $HDMI_OUTPUT"

# Try to set 1280x720 mode
# First, check if the mode exists
if xrandr 2>/dev/null | grep -q "1280x720"; then
    # Mode exists, use it
    xrandr --output "$HDMI_OUTPUT" --mode 1280x720 --rate 60 2>/dev/null
    echo "✅ Set resolution to 1280x720 using existing mode"
else
    # Mode doesn't exist, create it
    xrandr --newmode "1280x720_60.00" 74.50 1280 1344 1472 1664 720 723 728 748 -hsync +vsync 2>/dev/null
    xrandr --addmode "$HDMI_OUTPUT" "1280x720_60.00" 2>/dev/null
    xrandr --output "$HDMI_OUTPUT" --mode "1280x720_60.00" 2>/dev/null
    echo "✅ Created and set resolution to 1280x720"
fi

# Verify
CURRENT_MODE=$(xrandr 2>/dev/null | grep -A1 "^$HDMI_OUTPUT" | grep -oE '[0-9]+x[0-9]+' | head -1)
echo "Current resolution: $CURRENT_MODE"

