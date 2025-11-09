#!/bin/bash
# Transcode videos to 30fps for smoother playback on Raspberry Pi 4
# Uses hardware-accelerated encoding when available

set -e

MEDIA_DIR="${1:-/data/media}"
BACKUP_DIR="${MEDIA_DIR}/backup_original"
OUTPUT_DIR="${MEDIA_DIR}"

# Create backup directory
mkdir -p "$BACKUP_DIR"

# Video extensions to process
VIDEO_EXTS=("mp4" "mkv" "avi" "mov" "m4v")

# Find all video files
find_videos() {
    find "$MEDIA_DIR" -maxdepth 1 -type f \( \
        -iname "*.mp4" -o \
        -iname "*.mkv" -o \
        -iname "*.avi" -o \
        -iname "*.mov" -o \
        -iname "*.m4v" \
    \) ! -name "*.30fps.*"
}

# Check if video is already 30fps or less
is_30fps_or_less() {
    local file="$1"
    local fps=$(ffprobe -v error -select_streams v:0 -show_entries stream=r_frame_rate -of default=noprint_wrappers=1:nokey=1 "$file" 2>/dev/null | head -1)
    
    if [ -z "$fps" ]; then
        return 1  # Unknown frame rate, transcode to be safe
    fi
    
    # Parse fps (format: 30/1 or 60/1)
    local num=$(echo "$fps" | cut -d'/' -f1)
    local den=$(echo "$fps" | cut -d'/' -f2)
    
    if [ -z "$den" ] || [ "$den" = "0" ]; then
        return 1  # Invalid, transcode
    fi
    
    # Calculate fps value using awk (more portable than bc)
    local fps_value=$(awk "BEGIN {printf \"%.2f\", $num/$den}")
    
    # Check if fps <= 30 (using awk for comparison)
    if awk "BEGIN {exit !($fps_value <= 30)}"; then
        return 0  # Already 30fps or less
    else
        return 1  # Needs transcoding
    fi
}

# Transcode a single video file
transcode_video() {
    local input="$1"
    local basename=$(basename "$input")
    local dirname=$(dirname "$input")
    local name_no_ext="${basename%.*}"
    local ext="${basename##*.}"
    
    local output="${dirname}/${name_no_ext}.30fps.${ext}"
    local temp_output="${output}.tmp"
    
    echo "Processing: $basename"
    
    # Check if already processed
    if [ -f "$output" ]; then
        echo "  Already transcoded, skipping..."
        return 0
    fi
    
    # Backup original if not already backed up
    if [ ! -f "${BACKUP_DIR}/${basename}" ]; then
        echo "  Backing up original..."
        cp "$input" "${BACKUP_DIR}/${basename}"
    fi
    
    # Get video info
    local width=$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of default=noprint_wrappers=1:nokey=1 "$input" 2>/dev/null | head -1)
    local height=$(ffprobe -v error -select_streams v:0 -show_entries stream=height -of default=noprint_wrappers=1:nokey=1 "$input" 2>/dev/null | head -1)
    local codec=$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of default=noprint_wrappers=1:nokey=1 "$input" 2>/dev/null | head -1)
    
    echo "  Original: ${width}x${height}, codec: $codec"
    
    # Try hardware-accelerated encoding first (h264_v4l2m2m on Pi)
    # Fallback to software encoding if hardware fails
    if command -v v4l2-ctl >/dev/null 2>&1 && [ -e /dev/video11 ]; then
        echo "  Attempting hardware-accelerated encoding..."
        # Scale to 720p for better performance on Pi 4
        # Calculate scaled dimensions maintaining aspect ratio
        local scale_filter="scale=720:-2"
        if ffmpeg -y -i "$input" \
            -c:v h264_v4l2m2m \
            -vf "$scale_filter" \
            -b:v 3M \
            -maxrate 5M \
            -bufsize 6M \
            -r 30 \
            -g 60 \
            -keyint_min 30 \
            -c:a copy \
            -movflags +faststart \
            -f mp4 \
            "$temp_output" 2>&1 | tee /tmp/ffmpeg.log; then
            mv "$temp_output" "$output"
            echo "  ✓ Successfully transcoded with hardware encoding"
            return 0
        else
            echo "  Hardware encoding failed, trying software encoding..."
            rm -f "$temp_output"
        fi
    fi
    
    # Software encoding fallback
    echo "  Using software encoding (libx264)..."
    # Scale to 720p for better performance on Pi 4
    local scale_filter="scale=720:-2"
    if ffmpeg -y -i "$input" \
        -c:v libx264 \
        -vf "$scale_filter" \
        -preset medium \
        -crf 23 \
        -r 30 \
        -g 60 \
        -keyint_min 30 \
        -c:a copy \
        -movflags +faststart \
        -f mp4 \
        "$temp_output" 2>&1 | tee /tmp/ffmpeg.log; then
        mv "$temp_output" "$output"
        echo "  ✓ Successfully transcoded with software encoding"
        return 0
    else
        echo "  ✗ Transcoding failed"
        rm -f "$temp_output"
        return 1
    fi
}

# Main processing
main() {
    echo "Video Transcoding Script - Convert to 30fps"
    echo "============================================"
    echo "Media directory: $MEDIA_DIR"
    echo "Backup directory: $BACKUP_DIR"
    echo ""
    
    local count=0
    local processed=0
    local skipped=0
    local failed=0
    
    while IFS= read -r video; do
        count=$((count + 1))
        
        if is_30fps_or_less "$video"; then
            echo "[$count] Skipping (already 30fps or less): $(basename "$video")"
            skipped=$((skipped + 1))
            continue
        fi
        
        if transcode_video "$video"; then
            processed=$((processed + 1))
        else
            failed=$((failed + 1))
        fi
        
        echo ""
    done < <(find_videos)
    
    echo "============================================"
    echo "Summary:"
    echo "  Total videos found: $count"
    echo "  Transcoded: $processed"
    echo "  Skipped (already 30fps): $skipped"
    echo "  Failed: $failed"
    echo ""
    echo "Original files backed up to: $BACKUP_DIR"
    echo "Transcoded files: *.30fps.*"
}

# Run main function
main

