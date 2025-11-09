#!/bin/bash
# Quick performance test script - runs monitoring for 30 seconds

echo "=== Performance Test - 30 seconds ==="
echo "Starting monitoring..."
echo ""

# Start monitoring in background
/opt/magic_dingus_box/scripts/monitor_performance.sh &
MONITOR_PID=$!

# Wait 30 seconds
sleep 30

# Kill monitor
kill $MONITOR_PID 2>/dev/null

echo ""
echo "=== Test Complete ==="
echo "Check the output above for CPU/GPU usage patterns"

