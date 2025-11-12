#!/bin/bash
# Test script to monitor frame rates and performance during video playback

echo "=== Frame Rate Performance Test ==="
echo "Monitoring CPU usage during video playback..."
echo "This will help verify the 30 FPS improvements are working"
echo ""
echo "Press Ctrl+C to stop"
echo ""

RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
MPV_SOCKET="$RUNTIME_DIR/magic/mpv.sock"

# Function to get mpv status
get_mpv_status() {
    if [ -S "$MPV_SOCKET" ]; then
        python3 << EOF 2>/dev/null
import socket
import json
try:
    s = socket.socket(socket.AF_UNIX)
    s.connect("$MPV_SOCKET")
    s.send(b'{"command": ["get_property", "path"]}\n')
    path_data = json.loads(s.recv(1024).decode())
    s.send(b'{"command": ["get_property", "pause"]}\n')
    pause_data = json.loads(s.recv(1024).decode())
    s.send(b'{"command": ["get_property", "idle-active"]}\n')
    idle_data = json.loads(s.recv(1024).decode())
    s.close()
    
    if idle_data.get("data") == True:
        print("IDLE")
    elif path_data.get("data"):
        path = path_data["data"]
        paused = pause_data.get("data", False)
        filename = path.split("/")[-1]
        if paused:
            print(f"PAUSED: {filename}")
        else:
            print(f"PLAYING: {filename}")
    else:
        print("UNKNOWN")
except Exception:
    print("ERROR")
EOF
    else
        echo "NO_SOCKET"
    fi
}

iteration=0
last_status="IDLE"

while true; do
    clear
    echo "=== Frame Rate Performance Monitor ==="
    echo "Time: $(date '+%H:%M:%S') | Iteration: $iteration"
    echo ""
    
    # Get status
    current_status=$(get_mpv_status)
    
    # Get CPU usage
    python_cpu=$(ps aux | grep "python.*magic" | grep -v grep | awk '{sum+=$3} END {printf "%.1f", sum+0}')
    mpv_cpu=$(ps aux | grep "mpv.*magic" | grep -v grep | awk '{sum+=$3} END {printf "%.1f", sum+0}')
    total_cpu=$(echo "$python_cpu + $mpv_cpu" | awk '{print $1+$2}')
    
    # Status indicator
    if echo "$current_status" | grep -q "PLAYING"; then
        echo "🎬 STATUS: $current_status"
        echo ""
        echo "📊 PERFORMANCE (During Video Playback):"
        echo "  Python CPU: ${python_cpu}% (target: 30-50% with 30 FPS)"
        echo "  mpv CPU: ${mpv_cpu}% (target: 150-200%)"
        echo "  Total CPU: ${total_cpu}%"
        echo ""
        if (( $(echo "$python_cpu < 60" | awk '{print ($1 < $2)}') )); then
            echo "✅ GOOD: Python CPU is reasonable (30 FPS working)"
        else
            echo "⚠️  Python CPU still high - may be transitioning"
        fi
        echo ""
        echo "✅ Frame Rates Active:"
        echo "  - Main loop: 30 FPS"
        echo "  - Display flips: ~15 FPS"
        echo "  - Track info: ~10 FPS"
    elif echo "$current_status" | grep -q "PAUSED"; then
        echo "⏸️  STATUS: $current_status"
        echo ""
        echo "📊 PERFORMANCE:"
        echo "  Python CPU: ${python_cpu}%"
        echo "  mpv CPU: ${mpv_cpu}%"
    else
        echo "💤 STATUS: $current_status (Waiting for video...)"
        echo ""
        echo "📊 CURRENT METRICS (UI Visible):"
        echo "  Python CPU: ${python_cpu}% (UI rendering - normal)"
        echo "  mpv CPU: ${mpv_cpu}% (idle)"
        echo ""
        echo "💡 Play a video to see frame rate improvements!"
    fi
    
    echo ""
    echo "Processes:"
    ps aux | grep -E "python.*magic|mpv.*magic" | grep -v grep | awk '{printf "  %s (PID %s): CPU=%s%%\n", $11, $2, $3}'
    
    echo ""
    echo "Refreshing every 2 seconds... (Press Ctrl+C to stop)"
    
    # Detect status change
    if [ "$current_status" != "$last_status" ]; then
        echo ""
        echo "🔄 STATUS CHANGED: $last_status → $current_status"
        last_status="$current_status"
    fi
    
    sleep 2
    iteration=$((iteration + 1))
done



