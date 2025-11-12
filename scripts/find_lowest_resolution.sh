#!/bin/bash
# Find the lowest resolution that RetroArch will respect and scale to fill screen

CONFIG_FILE="$HOME/.config/retroarch/retroarch.cfg"
TEST_LOG="/tmp/resolution_find.log"
RETROARCH_LOG="/tmp/magic_retroarch_launch.log"

echo "=== Finding Lowest Resolution That Fills Screen ===" | tee "$TEST_LOG"
echo "Testing resolutions from high to low" | tee -a "$TEST_LOG"
echo "Looking for lowest resolution RetroArch respects and scales up" | tee -a "$TEST_LOG"
echo "" | tee -a "$TEST_LOG"

# Test resolutions from reasonable to very low
# We'll test these in order and see which ones RetroArch actually uses
RESOLUTIONS=(
    "640x480"   # VGA - should definitely work
    "512x384"   # Quarter VGA
    "320x240"   # Quarter VGA - common low res
    "256x240"   # NES native
    "256x224"   # SNES native
    "240x180"   # Custom low
    "192x144"   # Custom lower
    "160x120"   # Very low
    "128x96"    # Extremely low
    "128x120"   # What we tried before
)

# Function to set resolution with proper scaling settings
set_resolution() {
    local WIDTH=$1
    local HEIGHT=$2
    
    # Remove old viewport settings
    sed -i '/^video_custom_viewport/d' "$CONFIG_FILE"
    sed -i '/^video_aspect_ratio/d' "$CONFIG_FILE"
    sed -i '/^video_force_aspect/d' "$CONFIG_FILE"
    sed -i '/^video_scale/d' "$CONFIG_FILE"
    sed -i '/^video_scale_integer/d' "$CONFIG_FILE"
    
    # Set custom viewport
    echo "video_custom_viewport_width = \"$WIDTH\"" >> "$CONFIG_FILE"
    echo "video_custom_viewport_height = \"$HEIGHT\"" >> "$CONFIG_FILE"
    echo "video_custom_viewport_enable = \"true\"" >> "$CONFIG_FILE"
    echo "video_custom_viewport_x = \"0\"" >> "$CONFIG_FILE"
    echo "video_custom_viewport_y = \"0\"" >> "$CONFIG_FILE"
    
    # Set aspect ratio to match viewport
    local ASPECT=$(echo "scale=3; $WIDTH / $HEIGHT" | bc 2>/dev/null || echo "1.0")
    echo "video_aspect_ratio = \"$ASPECT\"" >> "$CONFIG_FILE"
    
    # Don't force aspect - let it scale
    echo "video_force_aspect = \"false\"" >> "$CONFIG_FILE"
    
    # Enable scaling to fill screen
    echo "video_scale_integer = \"false\"" >> "$CONFIG_FILE"  # Allow non-integer scaling
    echo "video_scale = \"1.0\"" >> "$CONFIG_FILE"  # Let RetroArch scale automatically
    
    # Windowed fullscreen
    sed -i '/^video_windowed_fullscreen/d' "$CONFIG_FILE"
    echo "video_windowed_fullscreen = \"true\"" >> "$CONFIG_FILE"
    sed -i '/^video_fullscreen/d' "$CONFIG_FILE"
    echo "video_fullscreen = \"false\"" >> "$CONFIG_FILE"
}

echo "Test plan - we'll test these resolutions:" | tee -a "$TEST_LOG"
for RES in "${RESOLUTIONS[@]}"; do
    echo "  - $RES" | tee -a "$TEST_LOG"
done
echo "" | tee -a "$TEST_LOG"
echo "For each resolution:" | tee -a "$TEST_LOG"
echo "1. Set resolution in config" | tee -a "$TEST_LOG"
echo "2. Launch a game" | tee -a "$TEST_LOG"
echo "3. Check logs to see what RetroArch actually uses" | tee -a "$TEST_LOG"
echo "4. Note if it fills the screen" | tee -a "$TEST_LOG"
echo "" | tee -a "$TEST_LOG"

# Test each resolution
for RES in "${RESOLUTIONS[@]}"; do
    WIDTH=$(echo "$RES" | cut -d'x' -f1)
    HEIGHT=$(echo "$RES" | cut -d'x' -f2)
    
    echo "========================================" | tee -a "$TEST_LOG"
    echo "TEST: ${WIDTH}x${HEIGHT}" | tee -a "$TEST_LOG"
    echo "========================================" | tee -a "$TEST_LOG"
    
    set_resolution "$WIDTH" "$HEIGHT"
    echo "✅ Resolution configured: ${WIDTH}x${HEIGHT}" | tee -a "$TEST_LOG"
    echo "   Aspect ratio: $(echo "scale=3; $WIDTH / $HEIGHT" | bc)" | tee -a "$TEST_LOG"
    echo "" | tee -a "$TEST_LOG"
    echo "📋 INSTRUCTIONS:" | tee -a "$TEST_LOG"
    echo "   1. Launch a game now" | tee -a "$TEST_LOG"
    echo "   2. Check if it fills the screen" | tee -a "$TEST_LOG"
    echo "   3. Press Enter here when done testing..." | tee -a "$TEST_LOG"
    read -r
    
    # Check logs
    if [ -f "$RETROARCH_LOG" ]; then
        echo "" | tee -a "$TEST_LOG"
        echo "📊 LOG ANALYSIS:" | tee -a "$TEST_LOG"
        ACTUAL_RES=$(tail -100 "$RETROARCH_LOG" | grep -iE "Set video size|Using resolution|viewport|Geometry" | tail -5)
        echo "$ACTUAL_RES" | tee -a "$TEST_LOG"
        echo "" | tee -a "$TEST_LOG"
        echo "Did it fill the screen? (y/n)" | tee -a "$TEST_LOG"
        read -r FILLED
        echo "Filled screen: $FILLED" >> "$TEST_LOG"
    else
        echo "⚠️  RetroArch log not found" | tee -a "$TEST_LOG"
    fi
    
    echo "" | tee -a "$TEST_LOG"
done

echo "========================================" | tee -a "$TEST_LOG"
echo "Testing complete! Results saved to: $TEST_LOG" | tee -a "$TEST_LOG"
echo "Review the log to find the lowest working resolution." | tee -a "$TEST_LOG"

