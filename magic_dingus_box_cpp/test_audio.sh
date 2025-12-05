#!/bin/bash
# Comprehensive RetroArch HDMI Audio Testing Script
# Tests different audio drivers and devices to find working configuration

set -e

echo "=== RetroArch HDMI Audio Testing Script ==="
echo "Testing various audio configurations for Raspberry Pi HDMI output"
echo ""

# Test audio devices available
echo "=== Available Audio Devices ==="
aplay -l 2>/dev/null | grep -E "(card|device)" | head -10
echo ""

echo "=== ALSA Device List ==="
aplay -L | grep -E "(hdmi|plughw|sysdefault)" | grep vc4hdmi | head -10
echo ""

# Test basic audio functionality
echo "=== Testing Basic Audio ==="
if speaker-test -c 2 -l 1 -t sine >/dev/null 2>&1; then
    echo "✅ Basic ALSA audio works"
else
    echo "❌ Basic ALSA audio failed"
fi
echo ""

# Test different RetroArch audio configurations
echo "=== Testing RetroArch Audio Configurations ==="

# Test configurations
declare -a configs=(
    "alsa:hdmi:CARD=vc4hdmi0,DEV=0"
    "alsa:plughw:CARD=vc4hdmi0,DEV=0"
    "alsa:sysdefault:CARD=vc4hdmi0"
    "alsathread:hdmi:CARD=vc4hdmi0,DEV=0"
    "alsathread:plughw:CARD=vc4hdmi0,DEV=0"
    "alsathread:sysdefault:CARD=vc4hdmi0"
)

ROM_PATH="/data/roms/nes/Super Mario Bros. 3.nes"
CORE_PATH="/usr/lib/aarch64-linux-gnu/libretro/nestopia_libretro.so"

if [ ! -f "$ROM_PATH" ]; then
    echo "❌ ROM not found at $ROM_PATH"
    exit 1
fi

if [ ! -f "$CORE_PATH" ]; then
    echo "❌ Core not found at $CORE_PATH"
    exit 1
fi

echo "✅ ROM and core found"
echo ""

for config in "${configs[@]}"; do
    IFS=':' read -r driver device <<< "$config"

    echo "----------------------------------------"
    echo "Testing: driver=$driver, device=$device"
    echo "----------------------------------------"

    # Create test config
    CONFIG_FILE="/tmp/test_audio_$driver_$(echo $device | tr ':/' '_').cfg"
    cat > "$CONFIG_FILE" << EOF
# Test audio configuration
video_driver = "gl"
video_fullscreen = "true"
video_fullscreen_x = "640"
video_fullscreen_y = "480"
input_joypad_driver = "udev"
input_autodetect_enable = "true"
audio_driver = "$driver"
audio_device = "$device"
audio_enable = "true"
audio_sync = "true"
audio_volume = "1.000000"
libretro_directory = "/usr/lib/aarch64-linux-gnu/libretro"
EOF

    echo "Config created: $CONFIG_FILE"

    # Test RetroArch with timeout
    echo "Launching RetroArch with audio test..."
    timeout 8 retroarch -L "$CORE_PATH" "$ROM_PATH" --config "$CONFIG_FILE" --verbose 2>&1 | grep -E "(ALSA|Audio|audio|ALSA:|Audio:)" | head -5

    # Check exit code
    EXIT_CODE=$?
    if [ $EXIT_CODE -eq 124 ]; then
        echo "⏰ TIMEOUT: RetroArch started but timed out (audio may be working)"
        echo "✅ POSSIBLE SUCCESS: $driver:$device"
    elif [ $EXIT_CODE -eq 0 ]; then
        echo "✅ SUCCESS: $driver:$device initialized without errors"
    else
        echo "❌ FAILED: $driver:$device failed (exit code: $EXIT_CODE)"
    fi

    # Cleanup
    rm -f "$CONFIG_FILE"
    echo ""
    sleep 1
done

echo "=== Test Summary ==="
echo "Check the output above for configurations marked as SUCCESS or POSSIBLE SUCCESS"
echo "The working configuration(s) should be used in the RetroArch launcher."
echo ""
echo "To test manually: retroarch -L [core] [rom] --config [config_file] --verbose"
