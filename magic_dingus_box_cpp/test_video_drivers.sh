#!/bin/bash
# Comprehensive RetroArch Video Driver Test for Magic Dingus Box
# Tests all available video drivers systematically

set -e

echo "=== RetroArch Video Driver Test for Magic Dingus Box ==="
echo "Testing all available video drivers in DRM/KMS environment"
echo ""

ROM_PATH="/data/roms/nes/Super Mario Bros. 3.nes"
CORE_PATH="/usr/lib/aarch64-linux-gnu/libretro/nestopia_libretro.so"

if [ ! -f "$ROM_PATH" ]; then
    echo "ERROR: ROM not found at $ROM_PATH"
    exit 1
fi

if [ ! -f "$CORE_PATH" ]; then
    echo "ERROR: Core not found at $CORE_PATH"
    exit 1
fi

echo "✓ ROM found: $ROM_PATH"
echo "✓ Core found: $CORE_PATH"
echo ""

# Test each video driver
DRIVERS=("gl" "vulkan" "sdl2" "null")

for driver in "${DRIVERS[@]}"; do
    echo "=========================================="
    echo "Testing video driver: $driver"
    echo "=========================================="

    # Create config for this driver
    CONFIG_FILE="/tmp/test_retroarch_$driver.cfg"
    cat > "$CONFIG_FILE" << EOF
# Test config for $driver driver
video_driver = "$driver"
video_fullscreen = "true"
input_joypad_driver = "udev"
input_keyboard_layout = "us"
libretro_directory = "/usr/lib/aarch64-linux-gnu/libretro"
menu_show_online_updater = "false"
EOF

    if [ "$driver" = "sdl2" ]; then
        cat >> "$CONFIG_FILE" << EOF
# SDL2 environment variables
SDL_RENDER_DRIVER=opengles2
SDL_VIDEO_DRIVER=wayland
XDG_RUNTIME_DIR=/run/user/1000
WAYLAND_DISPLAY=wayland-0
DISPLAY=:0
EOF
    fi

    echo "Config created: $CONFIG_FILE"

    # Test the driver
    echo "Launching RetroArch with $driver driver..."
    echo "Command: timeout 10 retroarch -L \"$CORE_PATH\" \"$ROM_PATH\" --config \"$CONFIG_FILE\" --verbose"

    # Run with timeout to prevent hanging
    timeout 10 retroarch -L "$CORE_PATH" "$ROM_PATH" --config "$CONFIG_FILE" --verbose 2>&1 | head -50

    RETROARCH_EXIT=$?
    echo "RetroArch exited with code: $RETROARCH_EXIT"

    if [ $RETROARCH_EXIT -eq 0 ]; then
        echo "✅ SUCCESS: $driver driver initialized successfully!"
    elif [ $RETROARCH_EXIT -eq 124 ]; then
        echo "⏰ TIMEOUT: $driver driver started but timed out (may be working)"
    else
        echo "❌ FAILED: $driver driver failed to initialize (exit code: $RETROARCH_EXIT)"
    fi

    # Clean up
    rm -f "$CONFIG_FILE"
    echo ""
    sleep 2
done

echo "=========================================="
echo "Test Summary:"
echo "- gl: OpenGL (should work with DRM/KMS)"
echo "- vulkan: Vulkan (may work with DRM)"
echo "- sdl2: SDL2 (needs display server)"
echo "- null: Null driver (for testing cores only)"
echo ""
echo "Check the output above to see which drivers worked."
echo "=========================================="
