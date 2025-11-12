#!/bin/bash
# Iterative test to get 640x480 scaling to fill screen

CONFIG_FILE="$HOME/.config/retroarch/retroarch.cfg"
TEST_LOG="/tmp/iterative_resolution_test.log"
RETROARCH_LOG="/tmp/magic_retroarch_launch.log"
TEST_ROM="/opt/magic_dingus_box/dev_data/roms/nes/Super Mario Bros. 3.nes"
TEST_CORE="nestopia_libretro"

echo "=== Iterative 640x480 Resolution Test ===" | tee "$TEST_LOG"
echo "Testing different settings to get 640x480 to scale and fill screen" | tee -a "$TEST_LOG"
echo "" | tee -a "$TEST_LOG"

# Test configurations to try
declare -a TEST_CONFIGS=(
    "viewport_only"
    "viewport_with_scale"
    "viewport_no_aspect"
    "viewport_windowed"
    "no_viewport_scale"
)

test_config() {
    local CONFIG_NAME=$1
    local WIDTH=640
    local HEIGHT=480
    
    echo "" | tee -a "$TEST_LOG"
    echo "========================================" | tee -a "$TEST_LOG"
    echo "Testing: $CONFIG_NAME" | tee -a "$TEST_LOG"
    echo "========================================" | tee -a "$TEST_LOG"
    
    # Backup config
    cp "$CONFIG_FILE" "${CONFIG_FILE}.backup.$(date +%s)"
    
    # Remove old settings
    sed -i '/^video_custom_viewport/d' "$CONFIG_FILE"
    sed -i '/^video_aspect_ratio/d' "$CONFIG_FILE"
    sed -i '/^video_force_aspect/d' "$CONFIG_FILE"
    sed -i '/^video_scale/d' "$CONFIG_FILE"
    sed -i '/^video_scale_integer/d' "$CONFIG_FILE"
    sed -i '/^video_windowed_fullscreen/d' "$CONFIG_FILE"
    sed -i '/^video_fullscreen/d' "$CONFIG_FILE"
    
    case "$CONFIG_NAME" in
        "viewport_only")
            echo "video_custom_viewport_width = \"$WIDTH\"" >> "$CONFIG_FILE"
            echo "video_custom_viewport_height = \"$HEIGHT\"" >> "$CONFIG_FILE"
            echo "video_custom_viewport_enable = \"true\"" >> "$CONFIG_FILE"
            echo "video_windowed_fullscreen = \"true\"" >> "$CONFIG_FILE"
            echo "video_fullscreen = \"false\"" >> "$CONFIG_FILE"
            ;;
        "viewport_with_scale")
            echo "video_custom_viewport_width = \"$WIDTH\"" >> "$CONFIG_FILE"
            echo "video_custom_viewport_height = \"$HEIGHT\"" >> "$CONFIG_FILE"
            echo "video_custom_viewport_enable = \"true\"" >> "$CONFIG_FILE"
            echo "video_scale_integer = \"false\"" >> "$CONFIG_FILE"
            echo "video_scale = \"3.0\"" >> "$CONFIG_FILE"
            echo "video_windowed_fullscreen = \"true\"" >> "$CONFIG_FILE"
            echo "video_fullscreen = \"false\"" >> "$CONFIG_FILE"
            ;;
        "viewport_no_aspect")
            echo "video_custom_viewport_width = \"$WIDTH\"" >> "$CONFIG_FILE"
            echo "video_custom_viewport_height = \"$HEIGHT\"" >> "$CONFIG_FILE"
            echo "video_custom_viewport_enable = \"true\"" >> "$CONFIG_FILE"
            echo "video_aspect_ratio = \"1.333\"" >> "$CONFIG_FILE"
            echo "video_force_aspect = \"false\"" >> "$CONFIG_FILE"
            echo "video_scale_integer = \"false\"" >> "$CONFIG_FILE"
            echo "video_windowed_fullscreen = \"true\"" >> "$CONFIG_FILE"
            echo "video_fullscreen = \"false\"" >> "$CONFIG_FILE"
            ;;
        "viewport_windowed")
            echo "video_custom_viewport_width = \"$WIDTH\"" >> "$CONFIG_FILE"
            echo "video_custom_viewport_height = \"$HEIGHT\"" >> "$CONFIG_FILE"
            echo "video_custom_viewport_enable = \"true\"" >> "$CONFIG_FILE"
            echo "video_windowed_fullscreen = \"false\"" >> "$CONFIG_FILE"
            echo "video_fullscreen = \"false\"" >> "$CONFIG_FILE"
            ;;
        "no_viewport_scale")
            echo "video_custom_viewport_enable = \"false\"" >> "$CONFIG_FILE"
            echo "video_scale = \"0.333\"" >> "$CONFIG_FILE"
            echo "video_scale_integer = \"false\"" >> "$CONFIG_FILE"
            echo "video_windowed_fullscreen = \"true\"" >> "$CONFIG_FILE"
            echo "video_fullscreen = \"false\"" >> "$CONFIG_FILE"
            ;;
    esac
    
    echo "✅ Configuration set: $CONFIG_NAME" | tee -a "$TEST_LOG"
    echo "Settings:" | tee -a "$TEST_LOG"
    grep -E 'video_custom_viewport|video_aspect|video_scale|video_windowed|video_fullscreen' "$CONFIG_FILE" | grep -v '^#' | tail -10 | tee -a "$TEST_LOG"
    
    # Clear log
    > "$RETROARCH_LOG"
    
    # Launch game
    echo "🎮 Launching game..." | tee -a "$TEST_LOG"
    /opt/magic_dingus_box/scripts/launch_retroarch.sh "$TEST_ROM" "$TEST_CORE" "" "magic-ui.service" >> "$TEST_LOG" 2>&1 &
    sleep 8
    
    # Check if running
    if ! ps aux | grep -q "[r]etroarch"; then
        echo "❌ RetroArch failed to start" | tee -a "$TEST_LOG"
        return 1
    fi
    
    # Wait a bit for it to stabilize
    sleep 5
    
    # Check resolution from logs
    echo "📐 Checking resolution..." | tee -a "$TEST_LOG"
    ACTUAL_RES=$(tail -100 "$RETROARCH_LOG" | grep -iE "Set video size|Using resolution|viewport" | tail -3)
    echo "$ACTUAL_RES" | tee -a "$TEST_LOG"
    
    # Check window size
    WINDOW_ID=$(DISPLAY=:0 xdotool search --name RetroArch 2>/dev/null | head -1)
    if [ -n "$WINDOW_ID" ]; then
        WINDOW_SIZE=$(DISPLAY=:0 xdotool getwindowgeometry "$WINDOW_ID" 2>/dev/null | grep Geometry | awk '{print $2}')
        echo "Window size: $WINDOW_SIZE" | tee -a "$TEST_LOG"
        
        # Check if it's close to fullscreen (1920x1080)
        if echo "$WINDOW_SIZE" | grep -q "1920x1080\|1918x1053\|1412x1080"; then
            echo "✅ FILLS SCREEN!" | tee -a "$TEST_LOG"
            FILLS_SCREEN=true
        else
            echo "❌ Does not fill screen (window: $WINDOW_SIZE)" | tee -a "$TEST_LOG"
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
        echo "✅ SUCCESS! Configuration '$CONFIG_NAME' works!" | tee -a "$TEST_LOG"
        return 0
    else
        echo "❌ Configuration '$CONFIG_NAME' did not fill screen" | tee -a "$TEST_LOG"
        return 1
    fi
}

# Test each configuration
for CONFIG in "${TEST_CONFIGS[@]}"; do
    if test_config "$CONFIG"; then
        echo "" | tee -a "$TEST_LOG"
        echo "========================================" | tee -a "$TEST_LOG"
        echo "✅ FOUND WORKING CONFIGURATION: $CONFIG" | tee -a "$TEST_LOG"
        echo "========================================" | tee -a "$TEST_LOG"
        echo "" | tee -a "$TEST_LOG"
        echo "Current settings:" | tee -a "$TEST_LOG"
        grep -E 'video_custom_viewport|video_aspect|video_scale|video_windowed|video_fullscreen' "$CONFIG_FILE" | grep -v '^#' | tail -10 | tee -a "$TEST_LOG"
        exit 0
    fi
done

echo "" | tee -a "$TEST_LOG"
echo "❌ None of the configurations worked. Trying manual approach..." | tee -a "$TEST_LOG"

