#!/bin/bash
# N64 Resolution Scaler Tester
# Purpose: Find the best balance between sharp scaling and performance.
# Internal Resolution is ALWAYS Native (1x / 320x240) for performance.
# We are changing the OUTPUT Resolution (what the Pi sends to the TV).

ROM="/data/roms/n64/Super Mario 64.n64"
CORE_DIR="/home/magic/.config/retroarch/cores"
CORE="${CORE_DIR}/mupen64plus-next_libretro.so"
CONFIG="/tmp/test_n64_res.cfg"
LOG_BASE="/tmp/test_n64_res"

# Stop main service
echo "Stopping main service..."
sudo systemctl stop magic-dingus-box-cpp.service
sleep 2

# Cleanup function
cleanup() {
    pkill -9 retroarch
    pkill -9 gst-launch-1.0
    sleep 2
}

# Configuration Generator
create_config() {
    local RES_X=$1
    local RES_Y=$2
    local WIN_FULL=$3
    
    echo "Generating Config: Output Resolution=${RES_X}x${RES_Y}"
    
    cat > "$CONFIG" << EOF
# Core: GLideN64 + GL (Proven Best)
input_autodetect_enable = "true"
input_joypad_driver = "udev"
input_player1_analog_dpad_mode = "1"
libretro_directory = "$CORE_DIR"
libretro_system_directory = "/home/magic/.config/retroarch/system"

# Audio: alsathread (Proven Best)
audio_driver = "alsathread"
audio_device = "plughw:1,0"
audio_enable = "true"
audio_sync = "true"
audio_latency = "64"
audio_resampler = "nearest"

# Video: GL + No-Threaded (Proven Best in Test 3)
video_driver = "gl"
video_context_driver = "kms"
video_threaded = "false"
video_vsync = "true"
video_force_srgb_disable = "true"

# Scaling / Resolution
video_fullscreen = "true"
video_windowed_fullscreen = "$WIN_FULL"
video_fullscreen_x = "$RES_X"
video_fullscreen_y = "$RES_Y"
video_scale_integer = "false"
video_aspect_ratio_auto = "true"

# Core Options: Native Internal Resolution (Crucial for Speed)
mupen64plus-rdp-plugin = "gliden64"
mupen64plus-rsp-plugin = "hle"
mupen64plus-NativeResFactor = "1"
mupen64plus-next-pak1 = "memory"
EOF
}

# Runner
run_test() {
    local ID=$1
    local DESC=$2
    local LOGfile="${LOG_BASE}_${ID}.log"
    
    echo "=================================================="
    echo "RUNNING TEST $ID: $DESC"
    echo "Duration: 45 seconds"
    echo "=================================================="
    
    # Wake controller
    sudo udevadm trigger --action=change --sysname-match=js*
    
    # Run for 45 seconds
    timeout 45s retroarch -L "$CORE" "$ROM" --config "$CONFIG" --verbose > "$LOGfile" 2>&1
    
    echo "Test $ID Finished."
    cleanup
}

# Test Definitions
run_test_1() {
    # The "Winner" from previous round
    create_config "640" "480" "false"
    run_test "1" "640x480 Output (CRT Mode) - The Control"
}

run_test_2() {
    create_config "1280" "720" "false"
    run_test "2" "1280x720 Output (720p) - Sharper?"
}

run_test_3() {
    create_config "1920" "1080" "false"
    run_test "3" "1920x1080 Output (1080p) - HD?"
}

run_test_4() {
    # Desktop Mode (matches Test 2 from previous round, but let's retry as reference)
    create_config "0" "0" "true"
    run_test "4" "Desktop Resolution (Scaled by GPU)"
}

# Execution Logic
case "$1" in
    1) run_test_1 ;;
    2) run_test_2 ;;
    3) run_test_3 ;;
    4) run_test_4 ;;
    ALL)
        cleanup
        run_test_1
        run_test_2
        run_test_3
        run_test_4
        ;;
    *)
        echo "Usage: $0 [1|2|3|4|ALL]"
        echo "1: 640x480"
        echo "2: 1280x720"
        echo "3: 1920x1080"
        echo "4: Desktop"
        exit 1
        ;;
esac

