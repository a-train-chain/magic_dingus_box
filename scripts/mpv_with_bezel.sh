#!/bin/bash
# Wrapper script for mpv that conditionally loads bezel file based on what exists

RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
BEZEL_DIR="$RUNTIME_DIR/magic"

# Build mpv command with performance optimizations
MPV_CMD=(
    /usr/bin/mpv
    --idle=yes
    --no-osc
    --no-osd-bar
    --keep-open=yes
    --no-config
    --input-ipc-server="${RUNTIME_DIR}/magic/mpv.sock"
    --input-vo-keyboard=no
    --input-default-bindings=no
    --vo=x11
    --hwdec=rpi4
    --force-window=no
    --no-border
    --audio-device="${AUDIO_DEVICE:-alsa/plughw:CARD=vc4hdmi0,DEV=0}"
    --vd-lavc-threads=4
    --cache=yes
    --cache-secs=30
    --demuxer-max-bytes=500M
    --demuxer-max-back-bytes=500M
    --video-sync=display-resample
    --video-latency-hacks=yes
    --sws-fast=yes
    --vd-lavc-dr=yes
    --vd-lavc-fast=yes
    --vd-lavc-skiploopfilter=all
    --vd-lavc-skipframe=nonref
)

# Add external-file for bezel if it exists (check common formats)
if [ -f "$BEZEL_DIR/bezel.png" ]; then
    MPV_CMD+=(--external-file="$BEZEL_DIR/bezel.png")
elif [ -f "$BEZEL_DIR/bezel.mpg" ]; then
    MPV_CMD+=(--external-file="$BEZEL_DIR/bezel.mpg")
elif [ -f "$BEZEL_DIR/bezel.mp4" ]; then
    MPV_CMD+=(--external-file="$BEZEL_DIR/bezel.mp4")
elif [ -f "$BEZEL_DIR/bezel.mpeg" ]; then
    MPV_CMD+=(--external-file="$BEZEL_DIR/bezel.mpeg")
fi

# Execute mpv
exec "${MPV_CMD[@]}"

