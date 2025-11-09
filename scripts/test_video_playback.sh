#!/bin/bash
# Test script to monitor performance during video playback
# This script will check CPU/GPU usage before, during, and after video playback

echo "=== Video Playback Performance Test ==="
echo ""
echo "This script will monitor CPU/GPU usage during video playback."
echo "Please play a video (intro or playlist) and observe the metrics."
echo ""
echo "Press Ctrl+C to stop monitoring"
echo ""

RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
MPV_SOCKET="$RUNTIME_DIR/magic/mpv.sock"

# Function to get mpv status
get_mpv_status() {
    if [ -S "$MPV_SOCKET" ]; then
        path=$(echo '{"command": ["get_property", "path"]}' | socat - "$MPV_SOCKET" 2>/dev/null | grep -o '"data":"[^"]*"' | cut -d'"' -f4)
        pause=$(echo '{"command": ["get_property", "pause"]}' | socat - "$MPV_SOCKET" 2>/dev/null | grep -o '"data":true\|"data":false' | cut -d'"' -f4)
        idle=$(echo '{"command": ["get_property", "idle-active"]}' | socat - "$MPV_SOCKET" 2>/dev/null | grep -o '"data":true\|"data":false' | cut -d'"' -f4)
        
        if [ "$idle" = "true" ]; then
            echo "IDLE"
        elif [ -n "$path" ]; then
            if [ "$pause" = "true" ]; then
                echo "PAUSED: $(basename "$path")"
            else
                echo "PLAYING: $(basename "$path")"
            fi
        else
            echo "UNKNOWN"
        fi
    else
        echo "NO_SOCKET"
    fi
}

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

iteration=0
while true; do
    clear
    echo "=== Video Playback Performance Monitor ==="
    echo "Iteration: $iteration | $(date '+%H:%M:%S')"
    echo ""
    
    # System load
    echo -e "${GREEN}=== System Load ===${NC}"
    uptime
    echo ""
    
    # CPU usage by process
    echo -e "${GREEN}=== Top CPU Processes ===${NC}"
    ps aux --sort=-%cpu | head -6 | awk '{printf "%-8s %6s%% CPU %6s%% MEM %s\n", $11, $3, $4, $2}'
    echo ""
    
    # Memory usage
    echo -e "${GREEN}=== Memory Usage ===${NC}"
    free -h | grep -E "Mem|Swap"
    echo ""
    
    # GPU/VideoCore info
    echo -e "${GREEN}=== GPU/VideoCore Status ===${NC}"
    if command -v vcgencmd >/dev/null 2>&1; then
        temp=$(vcgencmd measure_temp | cut -d= -f2)
        throttled=$(vcgencmd get_throttled | cut -d= -f2)
        arm_clock=$(vcgencmd measure_clock arm | cut -d= -f2)
        gpu_clock=$(vcgencmd measure_clock gpu | cut -d= -f2)
        echo "Temperature: $temp"
        echo "Throttled: $throttled"
        echo "ARM Clock: $arm_clock Hz"
        echo "GPU Clock: $gpu_clock Hz"
    fi
    echo ""
    
    # mpv and python processes
    echo -e "${GREEN}=== Magic Dingus Box Processes ===${NC}"
    ps aux | grep -E "python.*magic|mpv.*magic" | grep -v grep | awk '{printf "PID: %-6s CPU: %5s%% MEM: %5s%% CMD: %s\n", $2, $3, $4, $11}'
    echo ""
    
    # Video playback status
    echo -e "${GREEN}=== Video Playback Status ===${NC}"
    status=$(get_mpv_status)
    if echo "$status" | grep -q "PLAYING"; then
        echo -e "${YELLOW}$status${NC}"
    elif echo "$status" | grep -q "IDLE"; then
        echo -e "${RED}$status${NC}"
    else
        echo "$status"
    fi
    echo ""
    
    # Performance summary
    echo -e "${GREEN}=== Performance Summary ===${NC}"
    python_cpu=$(ps aux | grep "python.*magic" | grep -v grep | awk '{sum+=$3} END {print sum+0}')
    mpv_cpu=$(ps aux | grep "mpv.*magic" | grep -v grep | awk '{sum+=$3} END {print sum+0}')
    total_cpu=$(echo "$python_cpu + $mpv_cpu" | bc)
    
    echo "Python CPU: ${python_cpu}%"
    echo "mpv CPU: ${mpv_cpu}%"
    echo "Total CPU: ${total_cpu}%"
    
    if (( $(echo "$total_cpu > 250" | bc -l) )); then
        echo -e "${RED}WARNING: High CPU usage (>250%)${NC}"
    elif (( $(echo "$total_cpu > 150" | bc -l) )); then
        echo -e "${YELLOW}Moderate CPU usage (150-250%)${NC}"
    else
        echo -e "${GREEN}Good CPU usage (<150%)${NC}"
    fi
    echo ""
    
    echo "Refreshing in 2 seconds... (Press Ctrl+C to stop)"
    sleep 2
    iteration=$((iteration + 1))
done

