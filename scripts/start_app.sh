#!/bin/bash
# Simple startup script for Magic Dingus Box
# This starts MPV and the UI in the background

set -e

# Wait for display to be ready
sleep 5

# Set display environment
export DISPLAY=:0
export XDG_RUNTIME_DIR=/run/user/1000

# Start MPV in background
/usr/bin/mpv --idle=yes --no-osc --no-osd-bar --keep-open=yes \
  --no-config \
  --input-ipc-server=/tmp/mpv-magic.sock \
  --input-vo-keyboard=no --input-default-bindings=no \
  --vo=gpu --hwdec=auto \
  --hwdec-codecs=all \
  --gpu-context=drm \
  --force-window=no \
  --vd-lavc-threads=4 \
  --video-sync=audio \
  --interpolation=no \
  --profile=fast &

# Wait for MPV to start
sleep 2

# Start the UI
cd /home/alexanderchaney/magic_dingus_box
/home/alexanderchaney/magic_dingus_box/venv/bin/python -m magic_dingus_box.main

