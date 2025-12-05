#!/bin/bash
# N64 Audio/Sync Optimization Tester
# Base Video: GL + GLideN64 + 640x480 + No Threading (Proven Stable)
# Goal: Fix Choppy Audio

ROM="/data/roms/n64/Super Mario 64.n64"
CORE_DIR="/home/alexanderchaney/.config/retroarch/cores"
CORE="${CORE_DIR}/mupen64plus-next_libretro.so"
CONFIG="/tmp/test_n64_audio.cfg"
LOG_BASE="/tmp/test_n64_audio"

echo "Stopping main service..."
sudo systemctl stop magic-dingus-box-cpp.service
sleep 2

cleanup() {
    pkill -9 retroarch
    pkill -9 gst-launch-1.0
    sleep 2
}

create_config() {
    local AUDIO_DRIVER=$1
    local AUDIO_SYNC=$2
    local VSYNC=$3
    local RESAMPLER=$4
    local LATENCY=$5
    
    echo "Generating Config: Audio=$AUDIO_DRIVER, Sync=$AUDIO_SYNC, VSync=$VSYNC"
    
    cat > "$CONFIG" << EOF
input_autodetect_enable = "true"
input_joypad_driver = "udev"
input_player1_analog_dpad_mode = "1"
libretro_directory = "$CORE_DIR"
libretro_system_directory = "/home/alexanderchaney/.config/retroarch/system"

# Video (Proven Stable)
video_driver = "gl"
video_context_driver = "kms"
video_threaded = "false"
video_fullscreen = "true"
video_windowed_fullscreen = "false"
video_fullscreen_x = "640"
video_fullscreen_y = "480"
video_scale_integer = "false"
video_force_srgb_disable = "true"
mupen64plus-rdp-plugin = "gliden64"
mupen64plus-rsp-plugin = "hle"
mupen64plus-NativeResFactor = "1"
mupen64plus-next-pak1 = "memory"

# Variable Settings
audio_driver = "$AUDIO_DRIVER"
audio_sync = "$AUDIO_SYNC"
video_vsync = "$VSYNC"
audio_resampler = "$RESAMPLER"
audio_latency = "$LATENCY"
audio_device = "plughw:1,0"
audio_enable = "true"
EOF
}

run_test() {
    local ID=$1
    local DESC=$2
    local LOGfile="${LOG_BASE}_${ID}.log"
    echo "=================================================="
    echo "RUNNING TEST $ID: $DESC"
    echo "Duration: 45 seconds"
    echo "=================================================="
    sudo udevadm trigger --action=change --sysname-match=js*
    timeout 45s retroarch -L "$CORE" "$ROM" --config "$CONFIG" --verbose > "$LOGfile" 2>&1
    echo "Test $ID Finished."
    cleanup
}

# Tests
run_test_1() {
    # Baseline: alsathread, sync=true, vsync=true
    create_config "alsathread" "true" "true" "nearest" "64"
    run_test "1" "Baseline (Alsathread + VSync + AudioSync)"
}

run_test_2() {
    # Tiny Alsa: Direct alsa, lower latency
    create_config "alsa" "true" "true" "nearest" "64"
    run_test "2" "Direct ALSA (Not Threaded)"
}

run_test_3() {
    # No VSync: Is GPU stalling the audio?
    create_config "alsathread" "true" "false" "nearest" "64"
    run_test "3" "No VSync (Fast Video, Tearing?)"
}

run_test_4() {
    # Higher Latency Buffer: 128ms (More buffer = less chops)
    create_config "alsathread" "true" "true" "nearest" "128"
    run_test "4" "Larger Audio Buffer (128ms)"
}

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
esac

