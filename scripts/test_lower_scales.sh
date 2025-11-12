#!/bin/bash
# Test lower scaling factors to see if RetroArch applies them

CONFIG_FILE="$HOME/.config/retroarch/retroarch.cfg"
TEST_LOG="/tmp/test_lower_scales.log"
RETROARCH_LOG="/tmp/magic_retroarch_launch.log"
TEST_ROM="/opt/magic_dingus_box/dev_data/roms/nes/Super Mario Bros. 3.nes"
TEST_CORE="nestopia_libretro"

echo "=== Testing Lower Scaling Factors ===" | tee "$TEST_LOG"
echo "NES native: 256x224" | tee -a "$TEST_LOG"
echo "Testing if lower scales work better" | tee -a "$TEST_LOG"
echo "" | tee -a "$TEST_LOG"

test_scale() {
    local SCALE=$1
    
    echo "" | tee -a "$TEST_LOG"
    echo "========================================" | tee -a "$TEST_LOG"
    echo "Testing: Scale = $SCALE" | tee -a "$TEST_LOG"
    echo "========================================" | tee -a "$TEST_LOG"
    
    # Clean config
    sed -i '/^video_scale/d' "$CONFIG_FILE"
    sed -i '/^video_scale_integer/d' "$CONFIG_FILE"
    sed -i '/^aspect_ratio_index/d' "$CONFIG_FILE"
    sed -i '/^video_aspect_ratio/d' "$CONFIG_FILE"
    sed -i '/^video_force_aspect/d' "$CONFIG_FILE"
    sed -i '/^video_custom_viewport/d' "$CONFIG_FILE"
    
    # Set scale
    echo "video_scale = \"$SCALE\"" >> "$CONFIG_FILE"
    echo "video_scale_integer = \"false\"" >> "$CONFIG_FILE"
    echo "aspect_ratio_index = \"23\"" >> "$CONFIG_FILE"  # Full
    echo "video_force_aspect = \"false\"" >> "$CONFIG_FILE"
    echo "video_custom_viewport_enable = \"false\"" >> "$CONFIG_FILE"
    echo "video_windowed_fullscreen = \"true\"" >> "$CONFIG_FILE"
    echo "video_fullscreen = \"false\"" >> "$CONFIG_FILE"
    
    echo "✅ Scale set to $SCALE" | tee -a "$TEST_LOG"
    
    # Clear log
    > "$RETROARCH_LOG"
    
    # Launch
    echo "🎮 Launching..." | tee -a "$TEST_LOG"
    /opt/magic_dingus_box/scripts/launch_retroarch.sh "$TEST_ROM" "$TEST_CORE" "" "magic-ui.service" >> "$TEST_LOG" 2>&1 &
    sleep 10
    
    if ! ps aux | grep -q "[r]etroarch"; then
        echo "❌ Failed to start" | tee -a "$TEST_LOG"
        pkill -9 -f launch_retroarch.sh 2>/dev/null
        sudo systemctl restart magic-ui.service >/dev/null 2>&1
        sleep 3
        return 1
    fi
    
    sleep 5
    
    # Check window
    WINDOW_ID=$(DISPLAY=:0 xdotool search --name RetroArch 2>/dev/null | head -1)
    if [ -z "$WINDOW_ID" ]; then
        echo "❌ Window not found" | tee -a "$TEST_LOG"
        pkill -9 retroarch 2>/dev/null
        pkill -9 -f launch_retroarch.sh 2>/dev/null
        rm -f /tmp/magic_retroarch_active.lock
        sudo systemctl restart magic-ui.service >/dev/null 2>&1
        sleep 3
        return 1
    fi
    
    WINDOW_SIZE=$(DISPLAY=:0 xdotool getwindowgeometry "$WINDOW_ID" 2>/dev/null | grep Geometry | awk '{print $2}')
    W=$(echo "$WINDOW_SIZE" | cut -d'x' -f1)
    H=$(echo "$WINDOW_SIZE" | cut -d'x' -f2)
    
    # Check resolution from logs
    ACTUAL_RES=$(tail -100 "$RETROARCH_LOG" | grep -iE "Set video size|Using resolution" | tail -1)
    
    echo "Window: ${W}x${H}" | tee -a "$TEST_LOG"
    echo "Resolution: $ACTUAL_RES" | tee -a "$TEST_LOG"
    
    # Check if it's larger than native (256x224)
    if [ -n "$W" ] && [ -n "$H" ]; then
        if [ "$W" -gt 256 ] || [ "$H" -gt 224 ]; then
            echo "✅ SCALING WORKING! Window is larger than native (${W}x${H} vs 256x224)" | tee -a "$TEST_LOG"
            if [ "$W" -gt 1800 ] && [ "$H" -gt 1000 ]; then
                echo "✅✅ FILLS SCREEN! (${W}x${H})" | tee -a "$TEST_LOG"
                pkill -9 retroarch 2>/dev/null
                pkill -9 -f launch_retroarch.sh 2>/dev/null
                rm -f /tmp/magic_retroarch_active.lock
                sudo systemctl restart magic-ui.service >/dev/null 2>&1
                sleep 3
                return 0
            else
                echo "⚠️  Scaled but doesn't fill screen yet" | tee -a "$TEST_LOG"
            fi
        else
            echo "❌ Not scaling - still at native size" | tee -a "$TEST_LOG"
        fi
    fi
    
    # Kill RetroArch
    pkill -9 retroarch 2>/dev/null
    pkill -9 -f launch_retroarch.sh 2>/dev/null
    rm -f /tmp/magic_retroarch_active.lock
    sleep 2
    
    # Restart UI
    sudo systemctl restart magic-ui.service >/dev/null 2>&1
    sleep 3
    
    return 1
}

# Test lower scale factors
SCALES=("1.5" "2.0" "2.5" "3.0" "4.0" "4.82" "5.0" "6.0" "7.5" "8.0")

for SCALE in "${SCALES[@]}"; do
    if test_scale "$SCALE"; then
        echo "" | tee -a "$TEST_LOG"
        echo "========================================" | tee -a "$TEST_LOG"
        echo "✅ FOUND WORKING SCALE: $SCALE" | tee -a "$TEST_LOG"
        echo "========================================" | tee -a "$TEST_LOG"
        exit 0
    fi
done

echo "" | tee -a "$TEST_LOG"
echo "❌ None of the scales worked" | tee -a "$TEST_LOG"
echo "Check $TEST_LOG for details" | tee -a "$TEST_LOG"

