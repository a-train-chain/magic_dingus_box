#!/bin/bash
# Automated resolution testing with game launches and performance monitoring

CONFIG_FILE="$HOME/.config/retroarch/retroarch.cfg"
TEST_LOG="/tmp/auto_resolution_performance_test.log"
RETROARCH_LOG="/tmp/magic_retroarch_launch.log"
TEST_ROM="/opt/magic_dingus_box/dev_data/roms/nes/Super Mario Bros. 3.nes"
TEST_CORE="nestopia_libretro"
RESULTS_FILE="/tmp/resolution_test_results.txt"

echo "=== Automated Resolution Performance Test ===" | tee "$TEST_LOG"
echo "Testing resolutions with actual game launches" | tee -a "$TEST_LOG"
echo "Results will be saved to: $RESULTS_FILE" | tee -a "$TEST_LOG"
echo "" | tee -a "$TEST_LOG"

# Test resolutions from high to low
RESOLUTIONS=(
    "640x480"
    "512x384"
    "320x240"
    "256x240"
    "256x224"
    "240x180"
    "192x144"
    "160x120"
    "128x96"
)

# Function to set resolution
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
    
    # Set aspect ratio
    local ASPECT=$(echo "scale=3; $WIDTH / $HEIGHT" | bc 2>/dev/null || echo "1.0")
    echo "video_aspect_ratio = \"$ASPECT\"" >> "$CONFIG_FILE"
    echo "video_force_aspect = \"false\"" >> "$CONFIG_FILE"
    
    # Enable scaling
    echo "video_scale_integer = \"false\"" >> "$CONFIG_FILE"
    echo "video_scale = \"1.0\"" >> "$CONFIG_FILE"
    
    # Windowed fullscreen
    sed -i '/^video_windowed_fullscreen/d' "$CONFIG_FILE"
    echo "video_windowed_fullscreen = \"true\"" >> "$CONFIG_FILE"
    sed -i '/^video_fullscreen/d' "$CONFIG_FILE"
    echo "video_fullscreen = \"false\"" >> "$CONFIG_FILE"
}

# Function to launch game and monitor
test_resolution() {
    local WIDTH=$1
    local HEIGHT=$2
    local RES="${WIDTH}x${HEIGHT}"
    
    echo "" | tee -a "$TEST_LOG"
    echo "========================================" | tee -a "$TEST_LOG"
    echo "Testing: $RES" | tee -a "$TEST_LOG"
    echo "========================================" | tee -a "$TEST_LOG"
    
    # Set resolution
    set_resolution "$WIDTH" "$HEIGHT"
    echo "✅ Resolution set to $RES" | tee -a "$TEST_LOG"
    
    # Clear old log
    > "$RETROARCH_LOG"
    
    # Launch game via wrapper script
    echo "🎮 Launching game..." | tee -a "$TEST_LOG"
    /opt/magic_dingus_box/scripts/launch_retroarch.sh "$TEST_ROM" "$TEST_CORE" "" "magic-ui.service" >> "$TEST_LOG" 2>&1 &
    LAUNCH_PID=$!
    
    # Wait for RetroArch to start
    echo "⏳ Waiting for RetroArch to start..." | tee -a "$TEST_LOG"
    sleep 5
    
    # Check if RetroArch is running
    if ! ps aux | grep -q "[r]etroarch"; then
        echo "❌ RetroArch failed to start" | tee -a "$TEST_LOG"
        return 1
    fi
    
    # Monitor for 10 seconds
    echo "📊 Monitoring performance (10 seconds)..." | tee -a "$TEST_LOG"
    sleep 10
    
    # Get RetroArch PID
    RETROARCH_PID=$(ps aux | grep "[r]etroarch" | grep -v grep | awk '{print $2}' | head -1)
    
    if [ -z "$RETROARCH_PID" ]; then
        echo "❌ RetroArch process not found" | tee -a "$TEST_LOG"
        return 1
    fi
    
    # Check CPU usage
    echo "💻 Checking CPU usage..." | tee -a "$TEST_LOG"
    CPU_USAGE=$(ps -p "$RETROARCH_PID" -o %cpu --no-headers 2>/dev/null | tr -d ' ' || echo "N/A")
    
    # Check memory usage
    MEM_USAGE=$(ps -p "$RETROARCH_PID" -o %mem --no-headers 2>/dev/null | tr -d ' ' || echo "N/A")
    
    # Check resolution from logs
    echo "📐 Checking actual resolution used..." | tee -a "$TEST_LOG"
    ACTUAL_RES=$(tail -100 "$RETROARCH_LOG" | grep -iE "Set video size|Using resolution" | tail -1 || echo "Not found")
    
    # Check window geometry
    WINDOW_GEOM=$(DISPLAY=:0 xdotool search --name RetroArch 2>/dev/null | head -1 | xargs -I {} xdotool getwindowgeometry {} 2>/dev/null | grep Geometry | awk '{print $2}' || echo "N/A")
    
    # Record results
    echo "" | tee -a "$TEST_LOG"
    echo "📊 RESULTS for $RES:" | tee -a "$TEST_LOG"
    echo "   Configured: ${WIDTH}x${HEIGHT}" | tee -a "$TEST_LOG"
    echo "   Actual resolution: $ACTUAL_RES" | tee -a "$TEST_LOG"
    echo "   Window geometry: $WINDOW_GEOM" | tee -a "$TEST_LOG"
    echo "   CPU usage: ${CPU_USAGE}%" | tee -a "$TEST_LOG"
    echo "   Memory usage: ${MEM_USAGE}%" | tee -a "$TEST_LOG"
    
    # Save to results file
    echo "$RES|${WIDTH}x${HEIGHT}|$ACTUAL_RES|$WINDOW_GEOM|${CPU_USAGE}%|${MEM_USAGE}%" >> "$RESULTS_FILE"
    
    # Kill RetroArch
    echo "🛑 Stopping RetroArch..." | tee -a "$TEST_LOG"
    kill -9 "$RETROARCH_PID" 2>/dev/null
    pkill -9 -f launch_retroarch.sh 2>/dev/null
    rm -f /tmp/magic_retroarch_active.lock
    sleep 2
    
    # Restart UI
    echo "🔄 Restarting UI..." | tee -a "$TEST_LOG"
    sudo systemctl restart magic-ui.service >/dev/null 2>&1
    sleep 3
    
    echo "✅ Test complete for $RES" | tee -a "$TEST_LOG"
}

# Initialize results file
echo "Resolution|Configured|Actual|Window|CPU|Memory" > "$RESULTS_FILE"

# Test each resolution
for RES in "${RESOLUTIONS[@]}"; do
    WIDTH=$(echo "$RES" | cut -d'x' -f1)
    HEIGHT=$(echo "$RES" | cut -d'x' -f2)
    
    test_resolution "$WIDTH" "$HEIGHT"
    
    # Small delay between tests
    sleep 2
done

echo "" | tee -a "$TEST_LOG"
echo "========================================" | tee -a "$TEST_LOG"
echo "Testing Complete!" | tee -a "$TEST_LOG"
echo "========================================" | tee -a "$TEST_LOG"
echo "" | tee -a "$TEST_LOG"
echo "Results summary:" | tee -a "$TEST_LOG"
cat "$RESULTS_FILE" | tee -a "$TEST_LOG"
echo "" | tee -a "$TEST_LOG"
echo "Full log: $TEST_LOG" | tee -a "$TEST_LOG"
echo "Results: $RESULTS_FILE" | tee -a "$TEST_LOG"

