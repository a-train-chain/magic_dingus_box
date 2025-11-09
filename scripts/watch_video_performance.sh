#!/bin/bash
# Real-time video playback performance monitor
# Watches for video activity and reports performance metrics

echo "=== Video Playback Performance Monitor ==="
echo "Monitoring for video playback activity..."
echo "Press Ctrl+C to stop"
echo ""

RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
MPV_SOCKET="$RUNTIME_DIR/magic/mpv.sock"

# Function to get mpv playback status
get_mpv_status() {
    if [ -S "$MPV_SOCKET" ]; then
        # Try to get status via Python (more reliable than socat)
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
except Exception as e:
    print("ERROR")
EOF
    else
        echo "NO_SOCKET"
    fi
}

iteration=0
last_status="IDLE"

while true; do
    # Get current status
    current_status=$(get_mpv_status)
    
    # Get CPU usage
    python_cpu=$(ps aux | grep "python.*magic" | grep -v grep | awk '{sum+=$3} END {printf "%.1f", sum+0}')
    mpv_cpu=$(ps aux | grep "mpv.*magic" | grep -v grep | awk '{sum+=$3} END {printf "%.1f", sum+0}')
    
    # Get GPU temp
    gpu_temp=$(vcgencmd measure_temp | cut -d= -f2)
    
    # Clear and display
    clear
    echo "=== Video Playback Performance Monitor ==="
    echo "Time: $(date '+%H:%M:%S') | Iteration: $iteration"
    echo ""
    
    # Status indicator
    if echo "$current_status" | grep -q "PLAYING"; then
        echo "🎬 STATUS: $current_status"
        echo ""
        echo "📊 PERFORMANCE METRICS:"
        echo "  Python CPU: ${python_cpu}% (should be ~10-20% when video playing)"
        echo "  mpv CPU: ${mpv_cpu}% (should be ~150-200% during playback)"
        echo "  GPU Temp: $gpu_temp"
        echo ""
        if (( $(echo "$python_cpu < 30" | bc -l 2>/dev/null || echo 0) )); then
            echo "✅ OPTIMIZATIONS WORKING: Python CPU is low!"
        else
            echo "⚠️  Python CPU still high - may be transitioning or UI visible"
        fi
    elif echo "$current_status" | grep -q "PAUSED"; then
        echo "⏸️  STATUS: $current_status"
        echo ""
        echo "📊 PERFORMANCE METRICS:"
        echo "  Python CPU: ${python_cpu}%"
        echo "  mpv CPU: ${mpv_cpu}%"
        echo "  GPU Temp: $gpu_temp"
    else
        echo "💤 STATUS: $current_status (Waiting for video playback...)"
        echo ""
        echo "📊 CURRENT METRICS:"
        echo "  Python CPU: ${python_cpu}% (UI rendering - normal)"
        echo "  mpv CPU: ${mpv_cpu}% (idle)"
        echo "  GPU Temp: $gpu_temp"
        echo ""
        echo "💡 To test: Play intro video or select a playlist video"
    fi
    
    echo ""
    echo "Processes:"
    ps aux | grep -E "python.*magic|mpv.*magic" | grep -v grep | awk '{printf "  %s (PID %s): CPU=%s%% MEM=%s%%\n", $11, $2, $3, $4}'
    
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

