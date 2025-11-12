#!/bin/bash
# Automated resolution testing - tests resolutions and checks logs

CONFIG_FILE="$HOME/.config/retroarch/retroarch.cfg"
TEST_LOG="/tmp/auto_resolution_test.log"
RETROARCH_LOG="/tmp/magic_retroarch_launch.log"

echo "=== Automated Resolution Testing ===" | tee "$TEST_LOG"
echo "This will test different resolutions and check what RetroArch actually uses" | tee -a "$TEST_LOG"
echo "" | tee -a "$TEST_LOG"

# Test resolutions from reasonable to very low
RESOLUTIONS=(
    "640x480"
    "512x384"
    "320x240"
    "256x240"
    "256x224"
    "240x180"
    "192x144"
    "160x120"
)

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
}

echo "Test plan:" | tee -a "$TEST_LOG"
for RES in "${RESOLUTIONS[@]}"; do
    echo "  - $RES" | tee -a "$TEST_LOG"
done
echo "" | tee -a "$TEST_LOG"
echo "For each resolution:" | tee -a "$TEST_LOG"
echo "1. Set resolution in config" | tee -a "$TEST_LOG"
echo "2. You launch a game" | tee -a "$TEST_LOG"
echo "3. Check logs to see what RetroArch used" | tee -a "$TEST_LOG"
echo "" | tee -a "$TEST_LOG"

# Test each resolution
for RES in "${RESOLUTIONS[@]}"; do
    WIDTH=$(echo "$RES" | cut -d'x' -f1)
    HEIGHT=$(echo "$RES" | cut -d'x' -f2)
    
    echo "========================================" | tee -a "$TEST_LOG"
    echo "Testing: ${WIDTH}x${HEIGHT}" | tee -a "$TEST_LOG"
    echo "========================================" | tee -a "$TEST_LOG"
    
    set_resolution "$WIDTH" "$HEIGHT"
    echo "✅ Resolution set to ${WIDTH}x${HEIGHT}" | tee -a "$TEST_LOG"
    echo "" | tee -a "$TEST_LOG"
    echo "Now launch a game, then press Enter to check logs..." | tee -a "$TEST_LOG"
    read -r
    
    # Check logs
    if [ -f "$RETROARCH_LOG" ]; then
        echo "Checking RetroArch logs..." | tee -a "$TEST_LOG"
        ACTUAL_RES=$(tail -100 "$RETROARCH_LOG" | grep -iE "Set video size|Using resolution|viewport" | tail -3)
        echo "Actual resolution used:" | tee -a "$TEST_LOG"
        echo "$ACTUAL_RES" | tee -a "$TEST_LOG"
    else
        echo "⚠️  RetroArch log not found" | tee -a "$TEST_LOG"
    fi
    
    echo "" | tee -a "$TEST_LOG"
done

echo "========================================" | tee -a "$TEST_LOG"
echo "Testing complete! Results in: $TEST_LOG" | tee -a "$TEST_LOG"

