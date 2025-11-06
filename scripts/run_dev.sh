#!/bin/bash
set -euo pipefail

# Magic Dingus Box - Development Runner
# Starts mpv and the UI in one command (macOS dev only)

SOCKET_PATH="/tmp/mpv-magic.sock"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "🎬 Starting Magic Dingus Box development environment..."

# Clean up any existing mpv process and socket
echo "Cleaning up existing processes..."
pkill -f "mpv.*mpv-magic.sock" || true
rm -f "$SOCKET_PATH"

# Start mpv in the background
echo "Starting mpv..."
mpv --idle=yes --geometry=720x480+0+0 --no-osd-bar --keep-open=yes \
  --input-ipc-server="$SOCKET_PATH" \
  --vf=scale=720:480:force_original_aspect_ratio=increase,crop=720:480,setdar=4/3 \
  > /dev/null 2>&1 &

MPV_PID=$!
echo "mpv started (PID: $MPV_PID)"

# Wait for socket to be ready
echo "Waiting for mpv socket..."
for i in {1..10}; do
  if [ -S "$SOCKET_PATH" ]; then
    echo "✓ Socket ready"
    break
  fi
  sleep 0.5
done

# Set environment and start UI
export MPV_SOCKET="$SOCKET_PATH"
cd "$REPO_ROOT"

echo "Starting UI..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# Trap to cleanup on exit
trap "echo 'Cleaning up...'; kill $MPV_PID 2>/dev/null || true; rm -f $SOCKET_PATH" EXIT

python -m magic_dingus_box.main

