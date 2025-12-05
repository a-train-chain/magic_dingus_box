#!/bin/bash
# N64 640x480 Optimization Tester
# Purpose: Find the best renderer/driver for the target 640x480 resolution.
# All tests run at 640x480 Output + Native Internal Res.

ROM="/data/roms/n64/Super Mario 64.n64"
CORE_DIR="/home/alexanderchaney/.config/retroarch/cores"
CORE="${CORE_DIR}/mupen64plus-next_libretro.so"
CONFIG="/tmp/test_n64_opt.cfg"
LOG_BASE="/tmp/test_n64_opt"

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
    local RDP=$2
    local RSP=$3
    local THREADED=$4
    
    echo "Generating Config: Driver=$DRIVER, RDP=$RDP, Threaded=$THREADED"
    
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
video_context_driver = "kms"
video_threaded = "$THREADED"
video_vsync = "true"
video_force_srgb_disable = "true"

# Resolution (Fixed at 640x480)
video_fullscreen = "true"
video_windowed_fullscreen = "false"
video_fullscreen_x = "640"
video_fullscreen_y = "480"
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
    create_config "gl" "gliden64" "hle" "false"
    run_test "1" "GL + GLideN64 (No Threading) - The Current Best"
}

run_test_2() {
    create_config "vulkan" "parallel" "parallel" "false"
    run_test "2" "Vulkan + ParaLLEl (No Threading)"
}

run_test_3() {
    create_config "gl" "gliden64" "hle" "true"
    run_test "3" "GL + GLideN64 (THREADED) - Does threading help Audio?"
}

run_test_4() {
    create_config "gl" "angrylion" "hle" "false"
    run_test "4" "GL + Angrylion (Software) - Accurate but slow?"
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
        echo "1: GL + GLideN64"
        echo "2: Vulkan + ParaLLEl"
        echo "3: GL + GLideN64 (Threaded)"
        echo "4: GL + Angrylion"
        exit 1
        ;;
esac

