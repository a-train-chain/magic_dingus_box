#!/bin/bash
# Performance monitoring script for Magic Dingus Box
# Monitors CPU, GPU, memory, and process usage during video playback

echo "=== Magic Dingus Box Performance Monitor ==="
echo "Press Ctrl+C to stop"
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

while true; do
    clear
    echo "=== $(date) ==="
    echo ""
    
    # System load
    echo -e "${GREEN}=== System Load ===${NC}"
    uptime
    echo ""
    
    # CPU usage by process
    echo -e "${GREEN}=== Top CPU Processes ===${NC}"
    ps aux --sort=-%cpu | head -6 | awk '{printf "%-8s %6s%% %s\n", $11, $3, $2}'
    echo ""
    
    # Memory usage
    echo -e "${GREEN}=== Memory Usage ===${NC}"
    free -h
    echo ""
    
    # GPU/VideoCore info
    echo -e "${GREEN}=== GPU/VideoCore Status ===${NC}"
    if command -v vcgencmd >/dev/null 2>&1; then
        echo "Temperature: $(vcgencmd measure_temp | cut -d= -f2)"
        echo "Throttled: $(vcgencmd get_throttled | cut -d= -f2)"
        echo "ARM Clock: $(vcgencmd measure_clock arm | cut -d= -f2) Hz"
        echo "GPU Clock: $(vcgencmd measure_clock gpu | cut -d= -f2) Hz"
        echo "Core Voltage: $(vcgencmd measure_volts core | cut -d= -f2)"
    else
        echo "vcgencmd not available"
    fi
    echo ""
    
    # mpv and python processes
    echo -e "${GREEN}=== Magic Dingus Box Processes ===${NC}"
    ps aux | grep -E "python.*magic|mpv.*magic" | grep -v grep | awk '{printf "PID: %-6s CPU: %5s%% MEM: %5s%% CMD: %s\n", $2, $3, $4, $11}'
    echo ""
    
    # Check if video is playing
    echo -e "${GREEN}=== Video Playback Status ===${NC}"
    RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
    MPV_SOCKET="$RUNTIME_DIR/magic/mpv.sock"
    if [ -S "$MPV_SOCKET" ]; then
        # Try to get mpv status via JSON IPC
        echo '{"command": ["get_property", "path"]}' | socat - "$MPV_SOCKET" 2>/dev/null | grep -q "null" && echo "Status: Idle" || echo "Status: Playing"
        echo '{"command": ["get_property", "pause"]}' | socat - "$MPV_SOCKET" 2>/dev/null | grep -q "true" && echo "Paused: Yes" || echo "Paused: No"
    else
        echo "mpv socket not found"
    fi
    echo ""
    
    # Network I/O (if applicable)
    echo -e "${GREEN}=== Network I/O ===${NC}"
    if command -v ifstat >/dev/null 2>&1; then
        ifstat -t 1 1 | tail -1
    else
        cat /proc/net/dev | grep -E "eth0|wlan0" | awk '{printf "%s: RX: %s TX: %s\n", $1, $2, $10}'
    fi
    echo ""
    
    echo "Refreshing in 2 seconds..."
    sleep 2
done

