#!/bin/bash
# Automated N64 Configuration Tester
# Usage: ./test_n64_configs.sh [1|2|3|4|ALL]

ROM="/data/roms/n64/Super Mario 64.n64"
CORE_DIR="/home/alexanderchaney/.config/retroarch/cores"
CORE="${CORE_DIR}/mupen64plus-next_libretro.so"
CONFIG="/tmp/test_n64.cfg"
LOG_BASE="/tmp/test_n64"

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
    local DRIVER=$1
    local CONTEXT=$2
    local RDP=$3
    local RSP=$4
    local RES_X=$5
    local RES_Y=$6
    local THREADED=$7
    local WIN_FULL=$8
    
    echo "Generating Config: Driver=$DRIVER, RDP=$RDP, Threaded=$THREADED, Res=${RES_X}x${RES_Y}"
    
    cat > "$CONFIG" << EOF
# Minimal N64 Config
input_autodetect_enable = "true"
input_joypad_driver = "udev"
input_player1_analog_dpad_mode = "1"
libretro_directory = "$CORE_DIR"
libretro_system_directory = "/home/alexanderchaney/.config/retroarch/system"

# Audio
audio_driver = "alsathread"
audio_device = "plughw:1,0"
audio_enable = "true"
audio_sync = "true"
audio_latency = "64"
audio_resampler = "nearest"

# Video
video_driver = "$DRIVER"
video_context_driver = "$CONTEXT"
video_threaded = "$THREADED"
video_vsync = "true"
video_force_srgb_disable = "true"

# Resolution
video_fullscreen = "true"
video_windowed_fullscreen = "$WIN_FULL"
video_fullscreen_x = "$RES_X"
video_fullscreen_y = "$RES_Y"
video_scale_integer = "false"

# Core Options
mupen64plus-rdp-plugin = "$RDP"
mupen64plus-rsp-plugin = "$RSP"
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
    echo "Duration: 60 seconds"
    echo "=================================================="
    
    # Wake controller
    sudo udevadm trigger --action=change --sysname-match=js*
    
    # Run for 60 seconds
    timeout 60s retroarch -L "$CORE" "$ROM" --config "$CONFIG" --verbose > "$LOGfile" 2>&1
    
    echo "Test $ID Finished."
    cleanup
}

# Test Definitions
run_test_1() {
    create_config "gl" "kms" "gliden64" "hle" "0" "0" "true" "true"
    run_test "1" "GLideN64 + GL + Desktop + Threaded (Baseline)"
}

run_test_2() {
    create_config "gl" "kms" "gliden64" "hle" "0" "0" "false" "true"
    run_test "2" "GLideN64 + GL + Desktop + NON-Threaded (Check Latency)"
}

run_test_3() {
    create_config "gl" "kms" "gliden64" "hle" "640" "480" "false" "false"
    run_test "3" "GLideN64 + GL + 640x480 + NON-Threaded (CRT Mode)"
}

run_test_4() {
    create_config "vulkan" "kms" "parallel" "parallel" "0" "0" "false" "true"
    run_test "4" "ParaLLEl + Vulkan + Desktop (Experimental)"
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
        echo "1: GL + Threaded (Desktop)"
        echo "2: GL + No-Threaded (Desktop)"
        echo "3: GL + No-Threaded (640x480)"
        echo "4: Vulkan (Desktop)"
        exit 1
        ;;
esac
