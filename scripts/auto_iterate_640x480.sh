#!/bin/bash
# Automatically iterate to find settings that make 640x480 fill screen

CONFIG_FILE="$HOME/.config/retroarch/retroarch.cfg"
TEST_LOG="/tmp/auto_iterate_640x480.log"
RETROARCH_LOG="/tmp/magic_retroarch_launch.log"
TEST_ROM="/opt/magic_dingus_box/dev_data/roms/nes/Super Mario Bros. 3.nes"
TEST_CORE="nestopia_libretro"

echo "=== Auto-Iterate: Find 640x480 Settings ===" | tee "$TEST_LOG"
echo "Will test and adjust until screen is filled" | tee -a "$TEST_LOG"
echo "" | tee -a "$TEST_LOG"

test_and_check() {
    local SCALE=$1
    local APPROACH=$2
    
    echo "" | tee -a "$TEST_LOG"
    echo "========================================" | tee -a "$TEST_LOG"
    echo "Testing: Scale=$SCALE, Approach=$APPROACH" | tee -a "$TEST_LOG"
    echo "========================================" | tee -a "$TEST_LOG"
    
    # Clean config
    sed -i '/^video_custom_viewport/d' "$CONFIG_FILE"
    sed -i '/^video_scale/d' "$CONFIG_FILE"
    sed -i '/^video_scale_integer/d' "$CONFIG_FILE"
    sed -i '/^video_aspect_ratio/d' "$CONFIG_FILE"
    sed -i '/^video_force_aspect/d' "$CONFIG_FILE"
    
    if [ "$APPROACH" = "viewport" ]; then
        echo "video_custom_viewport_width = \"640\"" >> "$CONFIG_FILE"
        echo "video_custom_viewport_height = \"480\"" >> "$CONFIG_FILE"
        echo "video_custom_viewport_enable = \"true\"" >> "$CONFIG_FILE"
    else
        echo "video_custom_viewport_enable = \"false\"" >> "$CONFIG_FILE"
    fi
    
    echo "video_scale = \"$SCALE\"" >> "$CONFIG_FILE"
    echo "video_scale_integer = \"false\"" >> "$CONFIG_FILE"
    echo "video_windowed_fullscreen = \"true\"" >> "$CONFIG_FILE"
    echo "video_fullscreen = \"false\"" >> "$CONFIG_FILE"
    
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
    
    echo "Window: ${W}x${H}" | tee -a "$TEST_LOG"
    
    # Check if fills screen (within 50 pixels of 1920x1080)
    if [ -n "$W" ] && [ -n "$H" ] && [ "$W" -gt 1870 ] && [ "$H" -gt 1030 ]; then
        echo "✅ SUCCESS! Fills screen (${W}x${H})" | tee -a "$TEST_LOG"
        pkill -9 retroarch 2>/dev/null
        pkill -9 -f launch_retroarch.sh 2>/dev/null
        rm -f /tmp/magic_retroarch_active.lock
        sudo systemctl restart magic-ui.service >/dev/null 2>&1
        sleep 3
        return 0
    else
        echo "❌ Does not fill screen (${W}x${H})" | tee -a "$TEST_LOG"
        pkill -9 retroarch 2>/dev/null
        pkill -9 -f launch_retroarch.sh 2>/dev/null
        rm -f /tmp/magic_retroarch_active.lock
        sudo systemctl restart magic-ui.service >/dev/null 2>&1
        sleep 3
        return 1
    fi
}

# Try different approaches
echo "Testing scale-based approach (no viewport)..." | tee -a "$TEST_LOG"
for SCALE in 7.5 8.0 8.5 9.0 4.82 5.0 6.0; do
    if test_and_check "$SCALE" "scale"; then
        echo "" | tee -a "$TEST_LOG"
        echo "✅ FOUND WORKING SETTINGS!" | tee -a "$TEST_LOG"
        echo "Scale: $SCALE" | tee -a "$TEST_LOG"
        echo "Approach: Scale from native resolution" | tee -a "$TEST_LOG"
        exit 0
    fi
done

echo "" | tee -a "$TEST_LOG"
echo "Testing viewport + scale approach..." | tee -a "$TEST_LOG"
for SCALE in 3.0 2.5 2.0 1.5; do
    if test_and_check "$SCALE" "viewport"; then
        echo "" | tee -a "$TEST_LOG"
        echo "✅ FOUND WORKING SETTINGS!" | tee -a "$TEST_LOG"
        echo "Scale: $SCALE" | tee -a "$TEST_LOG"
        echo "Approach: Viewport 640x480 + scale" | tee -a "$TEST_LOG"
        exit 0
    fi
done

echo "" | tee -a "$TEST_LOG"
echo "❌ Could not find working settings" | tee -a "$TEST_LOG"
echo "Check $TEST_LOG for details" | tee -a "$TEST_LOG"

