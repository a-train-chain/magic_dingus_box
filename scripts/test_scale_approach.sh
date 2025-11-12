#!/bin/bash
# Test scaling approach - scale from core's native resolution

CONFIG_FILE="$HOME/.config/retroarch/retroarch.cfg"
TEST_LOG="/tmp/scale_test.log"
RETROARCH_LOG="/tmp/magic_retroarch_launch.log"
TEST_ROM="/opt/magic_dingus_box/dev_data/roms/nes/Super Mario Bros. 3.nes"
TEST_CORE="nestopia_libretro"

echo "=== Testing Scale-Based Approach ===" | tee "$TEST_LOG"
echo "NES core native: 256x224" | tee -a "$TEST_LOG"
echo "Target: Scale to fill 1920x1080 screen" | tee -a "$TEST_LOG"
echo "" | tee -a "$TEST_LOG"

# Calculate scale factors
# 1920 / 256 = 7.5 (width)
# 1080 / 224 = 4.82 (height)
# Use smaller to maintain aspect: ~4.8x

test_scale() {
    local SCALE=$1
    local DESCRIPTION=$2
    
    echo "" | tee -a "$TEST_LOG"
    echo "========================================" | tee -a "$TEST_LOG"
    echo "Testing: Scale = $SCALE ($DESCRIPTION)" | tee -a "$TEST_LOG"
    echo "========================================" | tee -a "$TEST_LOG"
    
    # Backup
    cp "$CONFIG_FILE" "${CONFIG_FILE}.backup.$(date +%s)"
    
    # Remove old settings
    sed -i '/^video_custom_viewport/d' "$CONFIG_FILE"
    sed -i '/^video_scale/d' "$CONFIG_FILE"
    sed -i '/^video_scale_integer/d' "$CONFIG_FILE"
    sed -i '/^video_aspect_ratio/d' "$CONFIG_FILE"
    sed -i '/^video_force_aspect/d' "$CONFIG_FILE"
    
    # Disable custom viewport - let RetroArch use core's native resolution
    echo "video_custom_viewport_enable = \"false\"" >> "$CONFIG_FILE"
    
    # Set scale
    echo "video_scale = \"$SCALE\"" >> "$CONFIG_FILE"
    echo "video_scale_integer = \"false\"" >> "$CONFIG_FILE"
    
    # Windowed fullscreen
    sed -i '/^video_windowed_fullscreen/d' "$CONFIG_FILE"
    echo "video_windowed_fullscreen = \"true\"" >> "$CONFIG_FILE"
    sed -i '/^video_fullscreen/d' "$CONFIG_FILE"
    echo "video_fullscreen = \"false\"" >> "$CONFIG_FILE"
    
    echo "✅ Scale set to $SCALE" | tee -a "$TEST_LOG"
    
    # Clear log
    > "$RETROARCH_LOG"
    
    # Launch game
    echo "🎮 Launching game..." | tee -a "$TEST_LOG"
    /opt/magic_dingus_box/scripts/launch_retroarch.sh "$TEST_ROM" "$TEST_CORE" "" "magic-ui.service" >> "$TEST_LOG" 2>&1 &
    sleep 8
    
    if ! ps aux | grep -q "[r]etroarch"; then
        echo "❌ RetroArch failed to start" | tee -a "$TEST_LOG"
        return 1
    fi
    
    sleep 5
    
    # Check resolution
    echo "📐 Checking resolution..." | tee -a "$TEST_LOG"
    ACTUAL_RES=$(tail -100 "$RETROARCH_LOG" | grep -iE "Set video size|Using resolution" | tail -2)
    echo "$ACTUAL_RES" | tee -a "$TEST_LOG"
    
    # Check window size
    WINDOW_ID=$(DISPLAY=:0 xdotool search --name RetroArch 2>/dev/null | head -1)
    if [ -n "$WINDOW_ID" ]; then
        WINDOW_SIZE=$(DISPLAY=:0 xdotool getwindowgeometry "$WINDOW_ID" 2>/dev/null | grep Geometry | awk '{print $2}')
        echo "Window size: $WINDOW_SIZE" | tee -a "$TEST_LOG"
        
        # Extract width and height
        W=$(echo "$WINDOW_SIZE" | cut -d'x' -f1)
        H=$(echo "$WINDOW_SIZE" | cut -d'x' -f2)
        
        # Check if close to fullscreen (within 100 pixels)
        if [ -n "$W" ] && [ -n "$H" ] && [ "$W" -gt 1800 ] && [ "$H" -gt 1000 ]; then
            echo "✅ FILLS SCREEN! (Window: ${W}x${H})" | tee -a "$TEST_LOG"
            FILLS_SCREEN=true
        else
            echo "❌ Does not fill screen (Window: ${W}x${H})" | tee -a "$TEST_LOG"
            FILLS_SCREEN=false
        fi
    else
        echo "⚠️  Could not find window" | tee -a "$TEST_LOG"
        FILLS_SCREEN=false
    fi
    
    # Kill RetroArch
    pkill -9 retroarch 2>/dev/null
    pkill -9 -f launch_retroarch.sh 2>/dev/null
    rm -f /tmp/magic_retroarch_active.lock
    sleep 2
    
    # Restart UI
    sudo systemctl restart magic-ui.service >/dev/null 2>&1
    sleep 3
    
    if [ "$FILLS_SCREEN" = true ]; then
        echo "✅ SUCCESS! Scale $SCALE works!" | tee -a "$TEST_LOG"
        return 0
    else
        return 1
    fi
}

# Test different scale factors
# 256x224 * 7.5 = 1920x1680 (too tall)
# 256x224 * 4.82 = 1234x1080 (fits height)
# Try values around 4.8-7.5
SCALES=("7.5" "6.0" "5.0" "4.82" "4.5" "8.0")

for SCALE in "${SCALES[@]}"; do
    if test_scale "$SCALE" "Scale factor"; then
        echo "" | tee -a "$TEST_LOG"
        echo "========================================" | tee -a "$TEST_LOG"
        echo "✅ FOUND WORKING SCALE: $SCALE" | tee -a "$TEST_LOG"
        echo "========================================" | tee -a "$TEST_LOG"
        exit 0
    fi
done

echo "" | tee -a "$TEST_LOG"
echo "❌ Scale approach didn't work. Trying window size approach..." | tee -a "$TEST_LOG"

