#!/bin/bash
# Test different resolutions to find the lowest that works in fullscreen

CONFIG_FILE="$HOME/.config/retroarch/retroarch.cfg"
LOG_FILE="/tmp/resolution_test.log"

echo "=== Testing Different Resolutions ===" | tee "$LOG_FILE"

# Test resolutions (width x height)
# Starting with common low resolutions and working down
RESOLUTIONS=(
    "320x240"   # Quarter VGA
    "256x240"   # NES native
    "256x224"   # SNES native
    "240x180"   # Custom low
    "192x144"   # Custom lower
    "160x120"   # Very low
    "128x96"    # Extremely low
    "128x120"   # What we tried before
)

# Backup config
if [ -f "$CONFIG_FILE" ]; then
    cp "$CONFIG_FILE" "${CONFIG_FILE}.backup.$(date +%Y%m%d%H%M%S)"
fi

echo "" | tee -a "$LOG_FILE"
echo "Testing resolutions. Check logs after each test." | tee -a "$LOG_FILE"
echo "Log file: $LOG_FILE" | tee -a "$LOG_FILE"
echo "" | tee -a "$LOG_FILE"

for RES in "${RESOLUTIONS[@]}"; do
    WIDTH=$(echo "$RES" | cut -d'x' -f1)
    HEIGHT=$(echo "$RES" | cut -d'x' -f2)
    
    echo "========================================" | tee -a "$LOG_FILE"
    echo "Testing: ${WIDTH}x${HEIGHT}" | tee -a "$LOG_FILE"
    echo "========================================" | tee -a "$LOG_FILE"
    
    # Update config with this resolution
    sed -i '/^video_custom_viewport_width/d' "$CONFIG_FILE"
    sed -i '/^video_custom_viewport_height/d' "$CONFIG_FILE"
    sed -i '/^video_custom_viewport_enable/d' "$CONFIG_FILE"
    sed -i '/^video_custom_viewport_x/d' "$CONFIG_FILE"
    sed -i '/^video_custom_viewport_y/d' "$CONFIG_FILE"
    
    echo "video_custom_viewport_width = \"$WIDTH\"" >> "$CONFIG_FILE"
    echo "video_custom_viewport_height = \"$HEIGHT\"" >> "$CONFIG_FILE"
    echo "video_custom_viewport_enable = \"true\"" >> "$CONFIG_FILE"
    echo "video_custom_viewport_x = \"0\"" >> "$CONFIG_FILE"
    echo "video_custom_viewport_y = \"0\"" >> "$CONFIG_FILE"
    
    # Calculate aspect ratio
    ASPECT=$(echo "scale=3; $WIDTH / $HEIGHT" | bc)
    sed -i '/^video_aspect_ratio/d' "$CONFIG_FILE"
    echo "video_aspect_ratio = \"$ASPECT\"" >> "$CONFIG_FILE"
    
    echo "Configured: ${WIDTH}x${HEIGHT} (aspect: $ASPECT)" | tee -a "$LOG_FILE"
    echo "Ready to test. Launch a game and check logs." | tee -a "$LOG_FILE"
    echo "Press Enter to continue to next resolution test..." | tee -a "$LOG_FILE"
    read -r
done

echo "" | tee -a "$LOG_FILE"
echo "All resolution tests complete!" | tee -a "$LOG_FILE"
echo "Check $LOG_FILE for results." | tee -a "$LOG_FILE"

