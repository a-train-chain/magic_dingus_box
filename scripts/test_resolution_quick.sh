#!/bin/bash
# Quick test: Try different resolutions and check what RetroArch actually uses

CONFIG_FILE="$HOME/.config/retroarch/retroarch.cfg"
TEST_LOG="/tmp/resolution_test_results.log"

echo "=== Quick Resolution Test ===" | tee "$TEST_LOG"
echo "Testing resolutions to find lowest that works in fullscreen" | tee -a "$TEST_LOG"
echo "" | tee -a "$TEST_LOG"

# Test resolutions from highest to lowest
RESOLUTIONS=(
    "640x480"   # VGA
    "512x384"   # Quarter VGA
    "320x240"   # Quarter VGA
    "256x240"   # NES native
    "256x224"   # SNES native  
    "240x180"   # Custom
    "192x144"   # Custom lower
    "160x120"   # Very low
    "128x96"    # Extremely low
)

# Backup config
if [ -f "$CONFIG_FILE" ]; then
    BACKUP="${CONFIG_FILE}.backup.$(date +%Y%m%d%H%M%S)"
    cp "$CONFIG_FILE" "$BACKUP"
    echo "Backed up config to: $BACKUP" | tee -a "$TEST_LOG"
fi

echo "Test resolutions:" | tee -a "$TEST_LOG"
for RES in "${RESOLUTIONS[@]}"; do
    echo "  - $RES" | tee -a "$TEST_LOG"
done
echo "" | tee -a "$TEST_LOG"

# Function to set resolution
set_resolution() {
    local WIDTH=$1
    local HEIGHT=$2
    
    # Remove old viewport settings
    sed -i '/^video_custom_viewport/d' "$CONFIG_FILE"
    
    # Add new settings
    echo "video_custom_viewport_width = \"$WIDTH\"" >> "$CONFIG_FILE"
    echo "video_custom_viewport_height = \"$HEIGHT\"" >> "$CONFIG_FILE"
    echo "video_custom_viewport_enable = \"true\"" >> "$CONFIG_FILE"
    echo "video_custom_viewport_x = \"0\"" >> "$CONFIG_FILE"
    echo "video_custom_viewport_y = \"0\"" >> "$CONFIG_FILE"
    
    # Set aspect ratio
    local ASPECT=$(echo "scale=3; $WIDTH / $HEIGHT" | bc 2>/dev/null || echo "1.0")
    sed -i '/^video_aspect_ratio/d' "$CONFIG_FILE"
    echo "video_aspect_ratio = \"$ASPECT\"" >> "$CONFIG_FILE"
    
    # Disable force aspect
    sed -i '/^video_force_aspect/d' "$CONFIG_FILE"
    echo "video_force_aspect = \"false\"" >> "$CONFIG_FILE"
    
    # Enable scaling to fill screen (allow non-integer scaling)
    sed -i '/^video_scale_integer/d' "$CONFIG_FILE"
    echo "video_scale_integer = \"false\"" >> "$CONFIG_FILE"  # Allow non-integer scaling
    sed -i '/^video_scale/d' "$CONFIG_FILE"
    echo "video_scale = \"1.0\"" >> "$CONFIG_FILE"  # Let RetroArch scale automatically
    
    # Windowed fullscreen
    sed -i '/^video_windowed_fullscreen/d' "$CONFIG_FILE"
    echo "video_windowed_fullscreen = \"true\"" >> "$CONFIG_FILE"
    sed -i '/^video_fullscreen/d' "$CONFIG_FILE"
    echo "video_fullscreen = \"false\"" >> "$CONFIG_FILE"
}

echo "Current resolution settings:" | tee -a "$TEST_LOG"
grep -E 'video_custom_viewport|video_aspect_ratio|video_windowed_fullscreen' "$CONFIG_FILE" | head -10 | tee -a "$TEST_LOG"
echo "" | tee -a "$TEST_LOG"

echo "To test a resolution, run:" | tee -a "$TEST_LOG"
echo "  bash scripts/test_resolution_quick.sh 320 240" | tee -a "$TEST_LOG"
echo "" | tee -a "$TEST_LOG"

if [ $# -eq 2 ]; then
    WIDTH=$1
    HEIGHT=$2
    echo "Setting resolution to ${WIDTH}x${HEIGHT}..." | tee -a "$TEST_LOG"
    set_resolution "$WIDTH" "$HEIGHT"
    echo "✅ Resolution set to ${WIDTH}x${HEIGHT}" | tee -a "$TEST_LOG"
    echo "Launch a game and check logs to see what RetroArch actually uses." | tee -a "$TEST_LOG"
    echo "Check: tail -50 /tmp/magic_retroarch_launch.log | grep -i resolution" | tee -a "$TEST_LOG"
else
    echo "Usage: $0 <width> <height>" | tee -a "$TEST_LOG"
    echo "Example: $0 320 240" | tee -a "$TEST_LOG"
fi

