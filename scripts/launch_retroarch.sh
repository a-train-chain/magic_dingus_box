#!/bin/bash
# Wrapper script to launch RetroArch with full process isolation
# Stops Magic Dingus Box UI, runs RetroArch, then restarts UI
# Can also launch Core Downloader if CORE is "menu"

# Use set +e to allow script to continue even if some commands fail
# We'll handle errors explicitly where needed
set +e
set -u  # Still fail on undefined variables

ROM_PATH="$1"
CORE="$2"
OVERLAY_PATH="$3"
SERVICE_NAME="${4:-magic-ui.service}"

# Check if this is a Core Downloader request (CORE == "menu")
IS_CORE_DOWNLOADER=false
if [ "$CORE" = "menu" ] || [ -z "$ROM_PATH" ] || [ "$ROM_PATH" = "menu" ]; then
    IS_CORE_DOWNLOADER=true
fi

LOG_FILE="/tmp/magic_retroarch_launch.log"
echo "$(date): Launching RetroArch wrapper" >> "$LOG_FILE"
echo "  ROM: $ROM_PATH" >> "$LOG_FILE"
echo "  Core: $CORE" >> "$LOG_FILE"
echo "  Overlay: $OVERLAY_PATH" >> "$LOG_FILE"
echo "  Service: $SERVICE_NAME" >> "$LOG_FILE"
echo "  Is Core Downloader: $IS_CORE_DOWNLOADER" >> "$LOG_FILE"

# CRITICAL: Check if we're in CRT mode and set X server resolution to 720x480
# This ensures the X server matches the display resolution for proper rendering
echo "$(date): Checking display mode and setting X server resolution if needed" >> "$LOG_FILE"
export DISPLAY=:0

# Check display mode from environment variable or settings file
DISPLAY_MODE="${MAGIC_DISPLAY_MODE:-}"
if [ -z "$DISPLAY_MODE" ]; then
    # Try to read from settings file
    SETTINGS_FILE="/data/settings.json"
    if [ ! -f "$SETTINGS_FILE" ]; then
        SETTINGS_FILE="$HOME/.config/magic_dingus_box/settings.json"
    fi
    if [ -f "$SETTINGS_FILE" ]; then
        DISPLAY_MODE=$(grep -o '"display_mode"[[:space:]]*:[[:space:]]*"[^"]*"' "$SETTINGS_FILE" 2>/dev/null | cut -d'"' -f4 || echo "")
    fi
fi
# Default to crt_native if not set
DISPLAY_MODE="${DISPLAY_MODE:-crt_native}"
echo "$(date): Display mode: $DISPLAY_MODE" >> "$LOG_FILE"

# If in CRT mode, set X server resolution to 720x480
if [ "$DISPLAY_MODE" = "crt_native" ]; then
    echo "$(date): CRT mode detected - setting X server resolution to 720x480" >> "$LOG_FILE"
    
    # Find the connected HDMI output
    HDMI_OUTPUT=$(DISPLAY=:0 xrandr 2>/dev/null | grep -E "connected.*HDMI" | awk '{print $1}' | head -1)
    if [ -z "$HDMI_OUTPUT" ]; then
        # Try to find any connected output
        HDMI_OUTPUT=$(DISPLAY=:0 xrandr 2>/dev/null | grep "connected" | awk '{print $1}' | head -1)
    fi
    
    if [ -n "$HDMI_OUTPUT" ]; then
        # Check current resolution
        CURRENT_MODE=$(DISPLAY=:0 xrandr 2>/dev/null | grep -A1 "^$HDMI_OUTPUT" | grep -oE '[0-9]+x[0-9]+' | head -1)
        echo "$(date): Current X server resolution: $CURRENT_MODE on $HDMI_OUTPUT" >> "$LOG_FILE"
        
        # If not already 720x480, set it
        if [ "$CURRENT_MODE" != "720x480" ]; then
            echo "$(date): Setting X server resolution to 720x480 on $HDMI_OUTPUT" >> "$LOG_FILE"
            
            # Check if 720x480 mode exists
            if DISPLAY=:0 xrandr 2>/dev/null | grep -q "720x480"; then
                # Mode exists, use it
                DISPLAY=:0 xrandr --output "$HDMI_OUTPUT" --mode 720x480 --rate 60 2>>"$LOG_FILE"
                echo "$(date): Set X server resolution to 720x480 using existing mode" >> "$LOG_FILE"
            else
                # Mode doesn't exist, create it
                # Modeline for 720x480@60Hz (NTSC-like)
                DISPLAY=:0 xrandr --newmode "720x480_60.00" 27.00 720 736 808 896 480 481 484 497 -hsync +vsync 2>>"$LOG_FILE"
                DISPLAY=:0 xrandr --addmode "$HDMI_OUTPUT" "720x480_60.00" 2>>"$LOG_FILE"
                DISPLAY=:0 xrandr --output "$HDMI_OUTPUT" --mode "720x480_60.00" 2>>"$LOG_FILE"
                echo "$(date): Created and set X server resolution to 720x480" >> "$LOG_FILE"
            fi
            
            # Verify the change
            sleep 0.5
            NEW_MODE=$(DISPLAY=:0 xrandr 2>/dev/null | grep -A1 "^$HDMI_OUTPUT" | grep -oE '[0-9]+x[0-9]+' | head -1)
            if [ "$NEW_MODE" = "720x480" ]; then
                echo "$(date): Successfully set X server resolution to 720x480" >> "$LOG_FILE"
            else
                echo "$(date): WARNING: Failed to set X server resolution to 720x480 (current: $NEW_MODE)" >> "$LOG_FILE"
            fi
        else
            echo "$(date): X server resolution already set to 720x480" >> "$LOG_FILE"
        fi
    else
        echo "$(date): WARNING: Could not find HDMI output to set resolution" >> "$LOG_FILE"
    fi
else
    echo "$(date): Not in CRT mode ($DISPLAY_MODE) - leaving X server resolution unchanged" >> "$LOG_FILE"
fi

# Find RetroArch executable (prefer RetroPie)
RETROARCH_BIN=""
for path in "/opt/retropie/emulators/retroarch/bin/retroarch" "/usr/bin/retroarch"; do
    if [ -f "$path" ]; then
        RETROARCH_BIN="$path"
        break
    fi
done

if [ -z "$RETROARCH_BIN" ]; then
    echo "ERROR: RetroArch not found" >> "$LOG_FILE"
    echo "ERROR: RetroArch not found. Install with: sudo apt install retroarch" >&2
    exit 1
fi

# CRITICAL: Stop MPV first (it might be playing video)
echo "$(date): Stopping MPV processes first" >> "$LOG_FILE"
pkill -9 mpv 2>>"$LOG_FILE" || true
pkill -9 -f "mpv.*socket" 2>>"$LOG_FILE" || true
# Also kill MPV windows explicitly
export DISPLAY=:0
for mpv_win in $(DISPLAY=:0 xdotool search --class mpv 2>/dev/null); do
    DISPLAY=:0 xdotool windowkill "$mpv_win" 2>>"$LOG_FILE" || true
done
sleep 1  # Give MPV time to fully exit

# CRITICAL: Hide/kill UI windows BEFORE stopping service
# This ensures UI windows don't block RetroArch
echo "$(date): Hiding UI windows before stopping service" >> "$LOG_FILE"
export DISPLAY=:0
# Find and hide/kill pygame windows (Magic Dingus Box UI) - be very aggressive
for pygame_window in $(DISPLAY=:0 xdotool search --class pygame 2>/dev/null); do
    echo "$(date): Found pygame window: $pygame_window, hiding it aggressively" >> "$LOG_FILE"
    # Move window far off-screen first (more reliable than killing)
    DISPLAY=:0 xdotool windowmove "$pygame_window" -10000 -10000 2>>"$LOG_FILE" || true
    # Set HIDDEN state
    DISPLAY=:0 xprop -id "$pygame_window" -f _NET_WM_STATE 32a -set _NET_WM_STATE _NET_WM_STATE_HIDDEN 2>>"$LOG_FILE" || true
    # Unmap window (make it invisible)
    DISPLAY=:0 xdotool windowunmap "$pygame_window" 2>>"$LOG_FILE" || true
    # Set BELOW state to ensure it's behind everything
    DISPLAY=:0 xprop -id "$pygame_window" -f _NET_WM_STATE 32a -set _NET_WM_STATE _NET_WM_STATE_BELOW 2>>"$LOG_FILE" || true
    # Finally, try to kill it
    DISPLAY=:0 xdotool windowkill "$pygame_window" 2>>"$LOG_FILE" || true
    # Also try xkill as fallback (if available)
    if command -v xkill >/dev/null 2>&1; then
        DISPLAY=:0 xkill -id "$pygame_window" 2>>"$LOG_FILE" || true
    fi
done
# Also kill by name pattern
for window in $(DISPLAY=:0 xdotool search --name "Magic Dingus Box" 2>/dev/null); do
    # Move off-screen, set HIDDEN, unmap, then kill
    DISPLAY=:0 xdotool windowmove "$window" -10000 -10000 2>>"$LOG_FILE" || true
    DISPLAY=:0 xprop -id "$window" -f _NET_WM_STATE 32a -set _NET_WM_STATE _NET_WM_STATE_HIDDEN 2>>"$LOG_FILE" || true
    DISPLAY=:0 xdotool windowunmap "$window" 2>>"$LOG_FILE" || true
    DISPLAY=:0 xdotool windowkill "$window" 2>>"$LOG_FILE" || true
done
sleep 1  # Give windows time to fully disappear

# CRITICAL: Check if we need to detach BEFORE killing the UI process
# If we're a child of the UI process, killing it will kill us too
PARENT_PID=$(ps -o ppid= -p $$ | tr -d ' ')
PARENT_CMD=$(ps -o cmd= -p "$PARENT_PID" 2>/dev/null || echo "")

# If parent is the UI process, we need to detach FIRST
if echo "$PARENT_CMD" | grep -q "magic_dingus_box.main"; then
    # We're a child of the UI process - detach BEFORE killing it
    echo "$(date): Detaching script from UI service BEFORE stopping it" >> "$LOG_FILE"
    # Use nohup to re-run this script in background, completely detached
    nohup bash "$0" "$ROM_PATH" "$CORE" "$OVERLAY_PATH" "$SERVICE_NAME" >> "$LOG_FILE" 2>&1 &
    DETACHED_PID=$!
    echo "$(date): Script re-executed in detached context (PID: $DETACHED_PID)" >> "$LOG_FILE"
    # Give detached process a moment to start
    sleep 1
    # Exit this instance - the detached copy will continue
    exit 0
fi

# CRITICAL: Create lock file IMMEDIATELY before stopping UI
# This ensures UI processes see the lock file and exit immediately
RETROARCH_LOCK_FILE="/tmp/magic_retroarch_active.lock"
echo "$(date): Creating RetroArch lock file early to prevent UI restart" >> "$LOG_FILE"
# Create lock file with placeholder PID (will be updated after RetroArch launches)
echo "starting" > "$RETROARCH_LOCK_FILE"
echo "$(date): Lock file created (placeholder)" >> "$LOG_FILE"

# Stop Magic Dingus Box UI service smoothly
# CRITICAL: Kill any existing UI processes FIRST, then mask BOTH user AND system services
# This ensures no processes are running that could restart the service
echo "$(date): Killing any existing UI processes before masking" >> "$LOG_FILE"
pkill -9 -f "magic_dingus_box.main" 2>>"$LOG_FILE" || true
# Also kill mpv processes that might be playing intro video
pkill -9 mpv 2>>"$LOG_FILE" || true
sleep 0.5

# CRITICAL: Stop and mask BOTH user AND system services
# The system service has Restart=always, so we MUST mask it
echo "$(date): Stopping and masking user service: $SERVICE_NAME" >> "$LOG_FILE"
systemctl --user unmask "$SERVICE_NAME" 2>>"$LOG_FILE" || echo "$(date): WARNING: Failed to unmask user service" >> "$LOG_FILE"
systemctl --user stop "$SERVICE_NAME" 2>>"$LOG_FILE" || echo "$(date): WARNING: Failed to stop user service" >> "$LOG_FILE"
systemctl --user mask "$SERVICE_NAME" 2>>"$LOG_FILE" || echo "$(date): WARNING: Failed to mask user service" >> "$LOG_FILE"

echo "$(date): Stopping and masking system service: $SERVICE_NAME" >> "$LOG_FILE"
sudo systemctl unmask "$SERVICE_NAME" 2>>"$LOG_FILE" || echo "$(date): WARNING: Failed to unmask system service" >> "$LOG_FILE"
sudo systemctl stop "$SERVICE_NAME" 2>>"$LOG_FILE" || echo "$(date): WARNING: Failed to stop system service" >> "$LOG_FILE"
sudo systemctl mask "$SERVICE_NAME" 2>>"$LOG_FILE" || echo "$(date): WARNING: Failed to mask system service" >> "$LOG_FILE"

# CRITICAL: Also stop and mask mpv service since UI service Requires it
# When mpv restarts, it triggers UI service restart due to Requires=
echo "$(date): Stopping and masking mpv service: magic-mpv.service" >> "$LOG_FILE"
sudo systemctl unmask magic-mpv.service 2>>"$LOG_FILE" || echo "$(date): WARNING: Failed to unmask mpv service" >> "$LOG_FILE"
sudo systemctl stop magic-mpv.service 2>>"$LOG_FILE" || echo "$(date): WARNING: Failed to stop mpv service" >> "$LOG_FILE"
sudo systemctl mask magic-mpv.service 2>>"$LOG_FILE" || echo "$(date): WARNING: Failed to mask mpv service" >> "$LOG_FILE"

echo "$(date): Finished stopping and masking services" >> "$LOG_FILE"

# Final kill of any remaining processes
pkill -9 -f "magic_dingus_box.main" 2>>"$LOG_FILE" || true
pkill -9 mpv 2>>"$LOG_FILE" || true
sleep 0.5

# Give service time to stop gracefully
sleep 1

# Wait for UI process to fully exit (with timeout)
echo "$(date): Waiting for UI process to exit" >> "$LOG_FILE"
UI_EXITED=false
for i in {1..10}; do  # Reduced to 10 attempts (3 seconds) - don't wait too long
    if ! pgrep -f "magic_dingus_box.main" > /dev/null; then
        echo "$(date): UI process exited after ${i} attempts" >> "$LOG_FILE"
        UI_EXITED=true
        break
    fi
    sleep 0.3
done

# If UI process still exists, force kill it immediately
if ! $UI_EXITED; then
    echo "$(date): UI process still running, forcing kill immediately" >> "$LOG_FILE"
    # Kill all UI processes aggressively
    pkill -9 -f "magic_dingus_box.main" 2>>"$LOG_FILE" || true
    sleep 0.5
    # Double-check and kill again if needed
    if pgrep -f "magic_dingus_box.main" > /dev/null; then
        echo "$(date): UI process still exists, killing again" >> "$LOG_FILE"
        pkill -9 -f "magic_dingus_box.main" 2>>"$LOG_FILE" || true
        sleep 0.5
    fi
    # Final check - proceed even if process still exists
    if pgrep -f "magic_dingus_box.main" > /dev/null; then
        echo "$(date): WARNING: UI process still exists after multiple kills, but proceeding anyway" >> "$LOG_FILE"
    else
        echo "$(date): UI process successfully killed" >> "$LOG_FILE"
    fi
fi

# Lock file already created above, will be updated with PID after RetroArch launches
MONITOR_PID=""

# CRITICAL: Ensure all UI windows are gone before launching RetroArch
echo "$(date): Ensuring all UI windows are removed" >> "$LOG_FILE"
# Kill ALL pygame/mpv windows aggressively - multiple passes
for pass in {1..3}; do
    echo "$(date): Window cleanup pass $pass" >> "$LOG_FILE"
    # Kill pygame windows - move off-screen, set HIDDEN, unmap, then kill
    for window in $(DISPLAY=:0 xdotool search --class pygame 2>/dev/null); do
        DISPLAY=:0 xdotool windowmove "$window" -10000 -10000 2>>"$LOG_FILE" || true
        DISPLAY=:0 xprop -id "$window" -f _NET_WM_STATE 32a -set _NET_WM_STATE _NET_WM_STATE_HIDDEN 2>>"$LOG_FILE" || true
        DISPLAY=:0 xdotool windowunmap "$window" 2>>"$LOG_FILE" || true
        DISPLAY=:0 xprop -id "$window" -f _NET_WM_STATE 32a -set _NET_WM_STATE _NET_WM_STATE_BELOW 2>>"$LOG_FILE" || true
        DISPLAY=:0 xdotool windowkill "$window" 2>>"$LOG_FILE" || true
    done
    # Kill mpv windows
    for window in $(DISPLAY=:0 xdotool search --class mpv 2>/dev/null); do
        DISPLAY=:0 xdotool windowkill "$window" 2>>"$LOG_FILE" || true
    done
    # Kill by name pattern - move off-screen, set HIDDEN, unmap, then kill
    for window in $(DISPLAY=:0 xdotool search --name "Magic Dingus Box" 2>/dev/null); do
        DISPLAY=:0 xdotool windowmove "$window" -10000 -10000 2>>"$LOG_FILE" || true
        DISPLAY=:0 xprop -id "$window" -f _NET_WM_STATE 32a -set _NET_WM_STATE _NET_WM_STATE_HIDDEN 2>>"$LOG_FILE" || true
        DISPLAY=:0 xdotool windowunmap "$window" 2>>"$LOG_FILE" || true
        DISPLAY=:0 xdotool windowkill "$window" 2>>"$LOG_FILE" || true
    done
    sleep 0.3
done
# Final check - kill any remaining windows
pkill -9 -f "python.*magic_dingus_box.main" 2>>"$LOG_FILE" || true
sleep 0.5
echo "$(date): UI windows cleared (3 passes), ready to launch RetroArch" >> "$LOG_FILE"

# We're now in a detached context (either via systemd-run or nohup) - continue with RetroArch launch
echo "$(date): Continuing with RetroArch launch (Core Downloader: $IS_CORE_DOWNLOADER)" >> "$LOG_FILE"
echo "$(date): Parent process: $PARENT_CMD" >> "$LOG_FILE"

        # Ensure display is ready and controllers are available
        # CRITICAL: When launched via systemd-run, we may not have X11 environment
        echo "$(date): Preparing display and input devices" >> "$LOG_FILE"
        export DISPLAY="${DISPLAY:-:0}"
        export XAUTHORITY="${XAUTHORITY:-$HOME/.Xauthority}"
        
        # Verify X11 is accessible
        if ! xdpyinfo -display "$DISPLAY" >/dev/null 2>&1; then
            echo "$(date): WARNING: X11 display $DISPLAY not accessible, trying :0" >> "$LOG_FILE"
            export DISPLAY=:0
            if ! xdpyinfo -display "$DISPLAY" >/dev/null 2>&1; then
                echo "$(date): ERROR: Cannot access X11 display" >> "$LOG_FILE"
                exit 1
            fi
        fi
        echo "$(date): X11 display verified: $DISPLAY" >> "$LOG_FILE"

# Release controller devices before RetroArch starts
# CRITICAL: Ensure controllers are fully released and available for RetroArch
echo "$(date): Releasing controller devices for RetroArch" >> "$LOG_FILE"

# First, ensure user has access to input devices (may need to be in 'input' group)
# This is usually already set, but verify
if ! groups | grep -q input; then
    echo "$(date): WARNING: User not in 'input' group - controller access may be limited" >> "$LOG_FILE"
fi

# Reset joystick devices to ensure they're available for RetroArch
# Use multiple methods to ensure devices are released
for js_device in /dev/input/js*; do
    if [ -c "$js_device" ]; then
        # Method 1: udevadm trigger (re-scans device)
        udevadm trigger --action=change --subsystem-match=input "$js_device" 2>>"$LOG_FILE" || true
        
        # Method 2: Ensure device is readable (check permissions)
        if [ ! -r "$js_device" ]; then
            echo "$(date): WARNING: Device $js_device is not readable" >> "$LOG_FILE"
        fi
        
        # Method 3: Use udevadm settle to ensure device is ready
        udevadm settle --timeout=1 2>>"$LOG_FILE" || true
        
        echo "$(date): Reset joystick device: $js_device" >> "$LOG_FILE"
    fi
done

# Reset event devices (evdev uses these)
# Limit to first 10 devices to avoid hanging on systems with many devices
EVENT_COUNT=0
for event_device in /dev/input/event*; do
    [ "$EVENT_COUNT" -ge 10 ] && break  # Limit to 10 devices
    if [ -c "$event_device" ]; then
        # Use timeout to prevent hanging
        if timeout 0.5 udevadm info --query=property --name="$event_device" 2>/dev/null | grep -q "ID_INPUT_JOYSTICK=1"; then
            # Method 1: udevadm trigger
            timeout 0.5 udevadm trigger --action=change --subsystem-match=input "$event_device" 2>>"$LOG_FILE" || true
            
            # Method 2: Ensure device is readable
            if [ ! -r "$event_device" ]; then
                echo "$(date): WARNING: Event device $event_device is not readable" >> "$LOG_FILE"
            fi
            
            echo "$(date): Reset joystick event device: $event_device" >> "$LOG_FILE"
        fi
        EVENT_COUNT=$((EVENT_COUNT + 1))
    fi
done

# Final settle to ensure all devices are ready
udevadm settle --timeout=2 2>>"$LOG_FILE" || true

echo "$(date): Finished releasing controller devices" >> "$LOG_FILE"
sleep 1  # Increased pause to ensure devices are fully ready for RetroArch

# Ensure RetroArch config directory is defined (needed for core path resolution)
CONFIG_DIR="$HOME/.config/retroarch"
CONFIG_FILE="$CONFIG_DIR/retroarch.cfg"
mkdir -p "$CONFIG_DIR"

# Prepare RetroArch command
# On Linux, cores don't have the _libretro suffix in the -L argument
# Skip core name if this is Core Downloader
if [ "$IS_CORE_DOWNLOADER" = "true" ]; then
    CORE_NAME=""
else
    CORE_NAME=$(echo "$CORE" | sed 's/_libretro$//')
    
    # CRITICAL: Find the full path to the core file
    # Check multiple locations: user cores, RetroPie cores, system cores
    CORE_PATH=""
    CORE_SEARCH_DIRS=(
        "$CONFIG_DIR/cores"
        "/opt/retropie/libretrocores"
        "/usr/lib/aarch64-linux-gnu/libretro"
        "/usr/lib/arm-linux-gnueabihf/libretro"
    )
    
    # Try to find core with various naming conventions
    for search_dir in "${CORE_SEARCH_DIRS[@]}"; do
        if [ -d "$search_dir" ]; then
            # Try exact match first
            if [ -f "$search_dir/${CORE_NAME}_libretro.so" ]; then
                CORE_PATH="$search_dir/${CORE_NAME}_libretro.so"
                break
            fi
            # Try with hyphen instead of underscore
            CORE_HYPHEN=$(echo "$CORE_NAME" | tr '_' '-')
            if [ -f "$search_dir/${CORE_HYPHEN}_libretro.so" ]; then
                CORE_PATH="$search_dir/${CORE_HYPHEN}_libretro.so"
                break
            fi
            # Try mupen64plus-next (special case - has hyphen in name)
            if [ "$CORE_NAME" = "mupen64plus-next" ] && [ -f "$search_dir/mupen64plus-next_libretro.so" ]; then
                CORE_PATH="$search_dir/mupen64plus-next_libretro.so"
                break
            fi
        fi
    done
    
    # If we found a core path, use it; otherwise use core name (let RetroArch find it)
    if [ -n "$CORE_PATH" ]; then
        echo "$(date): Found core at: $CORE_PATH" >> "$LOG_FILE"
        CORE_NAME="$CORE_PATH"
    else
        echo "$(date): Core path not found, using core name: $CORE_NAME (RetroArch will search)" >> "$LOG_FILE"
    fi
fi

# Ensure RetroArch config has proper controller settings
# (CONFIG_DIR and CONFIG_FILE are already defined above)

# Update config to ensure controller autodetection is enabled
if [ -f "$CONFIG_FILE" ]; then
    # CRITICAL: Remove any existing input_autodetect_enable lines first to avoid duplicates
    sed -i '/^input_autodetect_enable/d' "$CONFIG_FILE"
    # Ensure autodetect is enabled
    echo "input_autodetect_enable = \"true\"" >> "$CONFIG_FILE"
    
    # CRITICAL: Remove any existing input_joypad_driver lines first
    sed -i '/^input_joypad_driver/d' "$CONFIG_FILE"
    # Ensure joypad driver is set to udev (best for Linux)
    echo "input_joypad_driver = \"udev\"" >> "$CONFIG_FILE"
    
    # CRITICAL: Remove any existing input_auto_game_focus lines first
    sed -i '/^input_auto_game_focus/d' "$CONFIG_FILE"
    # Ensure auto game focus is enabled (allows controller to work in menus)
    echo "input_auto_game_focus = \"true\"" >> "$CONFIG_FILE"
    
    # CRITICAL: Remove any existing menu_driver lines first
    sed -i '/^menu_driver/d' "$CONFIG_FILE"
    # Set menu driver to ozone (better controller support)
    echo "menu_driver = \"ozone\"" >> "$CONFIG_FILE"
    
    # CRITICAL: Configure menu toggle button - use Select+Start combo
    # RetroArch button IDs: Select=2, Start=3
    sed -i '/^input_menu_toggle_gamepad_combo/d' "$CONFIG_FILE"
    echo "input_menu_toggle_gamepad_combo = \"2+3\"" >> "$CONFIG_FILE"  # Select (2) + Start (3)
    
    # Controller menu toggle is configured via input_menu_toggle_gamepad_combo above
    
    # CRITICAL: Enable hotkey but don't block controller input
    sed -i '/^input_enable_hotkey/d' "$CONFIG_FILE"
    echo "input_enable_hotkey = \"true\"" >> "$CONFIG_FILE"
    
    # CRITICAL: Set hotkey block delay to 0 (don't block controller)
    sed -i '/^input_hotkey_block_delay/d' "$CONFIG_FILE"
    echo "input_hotkey_block_delay = \"0\"" >> "$CONFIG_FILE"
    
    # CRITICAL: Set player 1 controller to use first available joystick (index 0)
    sed -i '/^input_player1_joypad_index/d' "$CONFIG_FILE"
    echo "input_player1_joypad_index = \"0\"" >> "$CONFIG_FILE"
    
    # CRITICAL: Enable automatic controller configuration
    sed -i '/^input_autoconfig_enable/d' "$CONFIG_FILE"
    echo "input_autoconfig_enable = \"true\"" >> "$CONFIG_FILE"
    
    # CRITICAL: Set autoconfig directory path (empty = default)
    sed -i '/^input_joypad_driver_mapping_dir/d' "$CONFIG_FILE"
    echo "input_joypad_driver_mapping_dir = \"\"" >> "$CONFIG_FILE"
    
    # CRITICAL: Ensure controller input is enabled (not blocked)
    sed -i '/^input_enabled/d' "$CONFIG_FILE"
    echo "input_enabled = \"true\"" >> "$CONFIG_FILE"
    
    # CRITICAL: Don't block input when menu is open
    sed -i '/^input_block_timeout/d' "$CONFIG_FILE"
    echo "input_block_timeout = \"0\"" >> "$CONFIG_FILE"
    
    # CRITICAL: Ensure all player inputs are enabled
    for player in 1 2 3 4; do
        sed -i "/^input_player${player}_enable/d" "$CONFIG_FILE"
        echo "input_player${player}_enable = \"true\"" >> "$CONFIG_FILE"
    done
    
    # CRITICAL: Set input driver (x driver works with udev joypad driver)
    # Note: input_driver is for keyboard/mouse, input_joypad_driver is for controllers
    # We already set input_joypad_driver = "udev" above, which is correct
    # Don't override input_driver - let RetroArch use default (usually "x")
    
    # CRITICAL: Ensure controller works in-game (not just in menu)
    sed -i '/^input_game_focus_enable/d' "$CONFIG_FILE"
    echo "input_game_focus_enable = \"true\"" >> "$CONFIG_FILE"
    
    # CRITICAL: Configure menu toggle button - use Select+Start combo
    # RetroArch button IDs: Select=2, Start=3
    sed -i '/^input_menu_toggle_gamepad_combo/d' "$CONFIG_FILE"
    echo "input_menu_toggle_gamepad_combo = \"2+3\"" >> "$CONFIG_FILE"  # Select (2) + Start (3)
    
    # Controller menu toggle is configured via input_menu_toggle_gamepad_combo above
    
    # CRITICAL: Enable menu navigation with controller
    # By default, RetroArch uses player 1 controller for menu navigation when autodetect is enabled
    # We don't need to explicitly set menu controls - they'll use player 1 automatically
    
    # CRITICAL: Configure core updater for downloading cores
    # RetroArch needs the buildbot cores URL to fetch the core list
    # For aarch64 (ARM64), we need to specify the correct architecture URL
    # IMPORTANT: The URL format for RetroArch 1.20.0+ requires the full path
    sed -i '/^core_updater_buildbot_cores_url/d' "$CONFIG_FILE"
    echo "core_updater_buildbot_cores_url = \"https://buildbot.libretro.com/nightly/linux/aarch64/latest\"" >> "$CONFIG_FILE"
    
    # Ensure assets URL is set (needed for core info files)
    sed -i '/^core_updater_buildbot_assets_url/d' "$CONFIG_FILE"
    echo "core_updater_buildbot_assets_url = \"https://buildbot.libretro.com/assets/\"" >> "$CONFIG_FILE"
    
    # CRITICAL: Enable core info file updates (needed before core downloader works)
    sed -i '/^core_updater_auto_extract_archive/d' "$CONFIG_FILE"
    echo "core_updater_auto_extract_archive = \"true\"" >> "$CONFIG_FILE"
    
    # CRITICAL: Set core updater buildbot info URL (for core info files)
    sed -i '/^core_updater_buildbot_info_url/d' "$CONFIG_FILE"
    echo "core_updater_buildbot_info_url = \"https://buildbot.libretro.com/assets/frontend/info.zip\"" >> "$CONFIG_FILE"
    
    # CRITICAL: Enable experimental cores (some cores may be marked experimental)
    sed -i '/^core_updater_show_experimental_cores/d' "$CONFIG_FILE"
    echo "core_updater_show_experimental_cores = \"true\"" >> "$CONFIG_FILE"
    
    # CRITICAL: Ensure info directory exists (where core info files are stored)
    INFO_DIR="$CONFIG_DIR/info"
    mkdir -p "$INFO_DIR"
    chmod 755 "$INFO_DIR"  # Ensure directory is writable
    echo "$(date): Core info directory: $INFO_DIR" >> "$LOG_FILE"
    
    # CRITICAL: Set info directory path in RetroArch config (if not set)
    sed -i '/^info_directory/d' "$CONFIG_FILE"
    echo "info_directory = \"$INFO_DIR\"" >> "$CONFIG_FILE"
    
    # CRITICAL: Ensure cores directory exists
    CORES_DIR="$CONFIG_DIR/cores"
    mkdir -p "$CORES_DIR"
    chmod 755 "$CORES_DIR"  # Ensure directory is writable
    echo "$(date): Cores directory: $CORES_DIR" >> "$LOG_FILE"
    
    # CRITICAL: Set cores directory path in RetroArch config to include RetroPie and system directories
    # Build libretro_directory with all available core directories
    RETROPIE_CORES="/opt/retropie/libretrocores"
    SYSTEM_CORES="/usr/lib/aarch64-linux-gnu/libretro"
    SYSTEM_CORES_ARMHF="/usr/lib/arm-linux-gnueabihf/libretro"
    
    LIBRETRO_DIRS="$CORES_DIR"
    [ -d "$RETROPIE_CORES" ] && LIBRETRO_DIRS="$LIBRETRO_DIRS;$RETROPIE_CORES"
    [ -d "$SYSTEM_CORES" ] && LIBRETRO_DIRS="$LIBRETRO_DIRS;$SYSTEM_CORES"
    [ -d "$SYSTEM_CORES_ARMHF" ] && LIBRETRO_DIRS="$LIBRETRO_DIRS;$SYSTEM_CORES_ARMHF"
    
    sed -i '/^libretro_directory/d' "$CONFIG_FILE"
    echo "libretro_directory = \"$LIBRETRO_DIRS\"" >> "$CONFIG_FILE"
    echo "$(date): Configured libretro_directory: $LIBRETRO_DIRS" >> "$LOG_FILE"
    
    # CRITICAL: Use "gl" driver with environment variables to force hardware rendering
    # "gl" auto-selects the best backend, and with MESA_LOADER_DRIVER_OVERRIDE=vc4 it uses V3D
    # Explicitly setting gles2/gles3 can cause RetroArch to fail or use wrong backend
    sed -i '/^video_driver/d' "$CONFIG_FILE"
    echo "video_driver = \"gl\"" >> "$CONFIG_FILE"
    echo "$(date): Configured video_driver: gl (will use hardware with env vars)" >> "$LOG_FILE"
    
    # Disable threaded video - can cause performance issues on Pi
    # Single-threaded rendering is more reliable for lightweight cores like NES
    sed -i '/^video_threaded/d' "$CONFIG_FILE"
    echo "video_threaded = \"false\"" >> "$CONFIG_FILE"
    echo "$(date): Configured video_threaded: false (better for Pi)" >> "$LOG_FILE"
    
    # Performance optimizations
    sed -i '/^video_hard_sync/d' "$CONFIG_FILE"
    echo "video_hard_sync = \"false\"" >> "$CONFIG_FILE"
    
    # Disable VSync for NES/SNES - they run at 60fps, VSync can add latency
    # For N64/PS1, we might want VSync, but for 8/16-bit, disable it
    sed -i '/^video_vsync/d' "$CONFIG_FILE"
    echo "video_vsync = \"false\"" >> "$CONFIG_FILE"
    
    sed -i '/^video_frame_delay/d' "$CONFIG_FILE"
    echo "video_frame_delay = \"0\"" >> "$CONFIG_FILE"
    
    # Disable unnecessary features for performance
    sed -i '/^video_shader_enable/d' "$CONFIG_FILE"
    echo "video_shader_enable = \"false\"" >> "$CONFIG_FILE"
    
    sed -i '/^video_filter/d' "$CONFIG_FILE"
    echo "video_filter = \"\"" >> "$CONFIG_FILE"
    
    sed -i '/^video_smooth/d' "$CONFIG_FILE"
    echo "video_smooth = \"false\"" >> "$CONFIG_FILE"
    
    # Additional performance settings
    sed -i '/^video_max_swapchain_images/d' "$CONFIG_FILE"
    echo "video_max_swapchain_images = \"2\"" >> "$CONFIG_FILE"  # Reduce from default 3
    
    sed -i '/^video_gpu_record/d' "$CONFIG_FILE"
    echo "video_gpu_record = \"false\"" >> "$CONFIG_FILE"
    
    sed -i '/^video_record/d' "$CONFIG_FILE"
    echo "video_record = \"false\"" >> "$CONFIG_FILE"
    
    # Disable rewind (uses CPU/memory)
    sed -i '/^rewind_enable/d' "$CONFIG_FILE"
    echo "rewind_enable = \"false\"" >> "$CONFIG_FILE"
    
    # Disable runahead (uses CPU)
    sed -i '/^run_ahead_enabled/d' "$CONFIG_FILE"
    echo "run_ahead_enabled = \"false\"" >> "$CONFIG_FILE"
    
    # Disable netplay (not needed)
    sed -i '/^netplay_enable/d' "$CONFIG_FILE"
    echo "netplay_enable = \"false\"" >> "$CONFIG_FILE"
    
    # Audio configuration - comprehensive setup for HDMI output
    echo "$(date): Configuring audio settings..." >> "$LOG_FILE"
    
    # First, try to detect available audio devices using aplay
    echo "$(date): Detecting audio devices..." >> "$LOG_FILE"
    if command -v aplay >/dev/null 2>&1; then
        echo "$(date): Available audio devices (aplay -l):" >> "$LOG_FILE"
        aplay -l >> "$LOG_FILE" 2>&1 || true
        echo "$(date): Available audio devices (aplay -L):" >> "$LOG_FILE"
        aplay -L >> "$LOG_FILE" 2>&1 || true
    fi
    
    # For Raspberry Pi, ALSA is most reliable for HDMI audio
    AUDIO_DRIVER="alsa"
    echo "$(date): Using ALSA driver (recommended for Raspberry Pi HDMI)" >> "$LOG_FILE"
    
    sed -i '/^audio_driver/d' "$CONFIG_FILE"
    echo "audio_driver = \"$AUDIO_DRIVER\"" >> "$CONFIG_FILE"
    
    # Find audio device - check for sysdefault:CARD=ALSA first (user preference)
    AUDIO_DEVICE=""
    
    # PRIORITY 1: Check for sysdefault:CARD=ALSA from aplay -L (user's tip)
    # Also check for any sysdefault device if ALSA card exists
    if command -v aplay >/dev/null 2>&1; then
        # First, try to find sysdefault:CARD=ALSA specifically (user preference)
        SYSALSA_DEVICE=$(aplay -L 2>/dev/null | grep -iE "^sysdefault:CARD=ALSA" | head -1 | sed 's/[[:space:]]*$//' | head -1)
        if [ -n "$SYSALSA_DEVICE" ]; then
            # Use the device name as-is (RetroArch supports this format directly)
            AUDIO_DEVICE="$SYSALSA_DEVICE"
            echo "$(date): Found sysdefault:CARD=ALSA device from aplay -L: $AUDIO_DEVICE" >> "$LOG_FILE"
        else
            # If ALSA card not found, try to find sysdefault for HDMI devices (common on Pi)
            # Prefer HDMI devices over headphones - check for vc4hdmi0 first (card 1)
            SYSHDMI_DEVICE=$(aplay -L 2>/dev/null | grep -iE "^sysdefault:CARD=vc4hdmi0" | head -1 | sed 's/[[:space:]]*$//' | head -1)
            if [ -n "$SYSHDMI_DEVICE" ]; then
                AUDIO_DEVICE="$SYSHDMI_DEVICE"
                echo "$(date): Found sysdefault HDMI device (vc4hdmi0) from aplay -L: $AUDIO_DEVICE" >> "$LOG_FILE"
            else
                # Try vc4hdmi1 (card 2) as fallback
                SYSHDMI_DEVICE=$(aplay -L 2>/dev/null | grep -iE "^sysdefault:CARD=vc4hdmi1" | head -1 | sed 's/[[:space:]]*$//' | head -1)
                if [ -n "$SYSHDMI_DEVICE" ]; then
                    AUDIO_DEVICE="$SYSHDMI_DEVICE"
                    echo "$(date): Found sysdefault HDMI device (vc4hdmi1) from aplay -L: $AUDIO_DEVICE" >> "$LOG_FILE"
                fi
            fi
        fi
    fi
    
    # PRIORITY 2: Try PulseAudio if sysdefault not found
    if [ -z "$AUDIO_DEVICE" ]; then
        if command -v pactl >/dev/null 2>&1; then
            HDMI_SINK=$(pactl list short sinks 2>/dev/null | grep -i hdmi | head -1 | awk '{print $2}')
            if [ -n "$HDMI_SINK" ]; then
                AUDIO_DEVICE="$HDMI_SINK"
                echo "$(date): Found PulseAudio HDMI sink: $AUDIO_DEVICE" >> "$LOG_FILE"
            else
                # Use default PulseAudio sink
                DEFAULT_SINK=$(pactl info 2>/dev/null | grep "Default Sink:" | cut -d' ' -f3)
                if [ -n "$DEFAULT_SINK" ]; then
                    AUDIO_DEVICE="$DEFAULT_SINK"
                    echo "$(date): Using default PulseAudio sink: $AUDIO_DEVICE" >> "$LOG_FILE"
                fi
            fi
        fi
    fi
    
    # PRIORITY 3: ALSA driver - try HDMI detection using aplay -l
    if [ -z "$AUDIO_DEVICE" ]; then
        if command -v aplay >/dev/null 2>&1; then
            # Try to find HDMI device using aplay -l output
            HDMI_CARD_DEV=$(aplay -l 2>/dev/null | grep -i "hdmi\|vc4hdmi" | head -1 | sed -n 's/.*card \([0-9]*\):.*device \([0-9]*\):.*/\1,\2/p')
            if [ -n "$HDMI_CARD_DEV" ]; then
                CARD=$(echo "$HDMI_CARD_DEV" | cut -d',' -f1)
                DEV=$(echo "$HDMI_CARD_DEV" | cut -d',' -f2)
                # Get card name
                CARD_NAME=$(aplay -l 2>/dev/null | grep "^card $CARD:" | sed 's/^card [0-9]*: \([^,]*\).*/\1/' | tr -d ' ' | tr '[:upper:]' '[:lower:]')
                if [ -n "$CARD_NAME" ]; then
                    AUDIO_DEVICE="alsa/hdmi:CARD=${CARD_NAME},DEV=${DEV}"
                    echo "$(date): Found ALSA HDMI device via aplay -l: $AUDIO_DEVICE" >> "$LOG_FILE"
                fi
            fi
        fi
    fi
    
    # PRIORITY 4: Fallback - try /proc/asound detection
    if [ -z "$AUDIO_DEVICE" ] && [ -f /proc/asound/cards ]; then
        HDMI_CARD=$(grep -i "vc4hdmi\|bcm2835\|hdmi" /proc/asound/cards | head -1 | awk '{print $1}' | tr -d ':')
        if [ -n "$HDMI_CARD" ]; then
            # Try different device numbers (0, 1)
            for DEV in 0 1; do
                if [ -d "/proc/asound/card${HDMI_CARD}/pcm${DEV}p" ] || [ -d "/proc/asound/card${HDMI_CARD}/pcm${DEV}c" ]; then
                    # Get card name from /proc/asound/cards
                    CARD_NAME=$(grep "^ *${HDMI_CARD} " /proc/asound/cards | sed 's/.*\[\(.*\)\].*/\1/' | tr '[:upper:]' '[:lower:]' | tr -d ' ')
                    if [ -z "$CARD_NAME" ]; then
                        CARD_NAME="vc4hdmi${HDMI_CARD}"
                    fi
                    AUDIO_DEVICE="alsa/hdmi:CARD=${CARD_NAME},DEV=${DEV}"
                    echo "$(date): Found ALSA HDMI device via /proc/asound: $AUDIO_DEVICE" >> "$LOG_FILE"
                    break
                fi
            done
        fi
    fi
    
    # PRIORITY 5: Final fallback - use plughw format for Raspberry Pi
    # plughw:1,0 = card 1, device 0 (vc4hdmi0)
    # This format works better than the RetroArch-specific format
    if [ -z "$AUDIO_DEVICE" ]; then
        # Try card 1 first (vc4hdmi0)
        if aplay -l 2>/dev/null | grep -q "card 1.*vc4hdmi0"; then
            AUDIO_DEVICE="plughw:1,0"
            echo "$(date): Using ALSA plughw device: $AUDIO_DEVICE (card 1)" >> "$LOG_FILE"
        # Try card 2 (vc4hdmi1)
        elif aplay -l 2>/dev/null | grep -q "card 2.*vc4hdmi1"; then
            AUDIO_DEVICE="plughw:2,0"
            echo "$(date): Using ALSA plughw device: $AUDIO_DEVICE (card 2)" >> "$LOG_FILE"
        else
            AUDIO_DEVICE="plughw:1,0"
            echo "$(date): Using default ALSA plughw device: $AUDIO_DEVICE" >> "$LOG_FILE"
        fi
    fi
    
    # Set audio device for ALSA
    if [ "$AUDIO_DRIVER" = "alsa" ] && [ -n "$AUDIO_DEVICE" ]; then
        sed -i '/^audio_device/d' "$CONFIG_FILE"
        echo "audio_device = \"$AUDIO_DEVICE\"" >> "$CONFIG_FILE"
        echo "$(date): Configured ALSA audio_device: $AUDIO_DEVICE" >> "$LOG_FILE"
    else
        echo "$(date): Using default audio device for $AUDIO_DRIVER driver" >> "$LOG_FILE"
    fi
    
    # Audio enable and mute settings - CRITICAL: ensure audio is enabled and not muted
    # Use sed to replace existing or append if not found
    if grep -q "^audio_enable" "$CONFIG_FILE"; then
        sed -i 's|^audio_enable.*|audio_enable = "true"|' "$CONFIG_FILE"
    else
        echo "audio_enable = \"true\"" >> "$CONFIG_FILE"
    fi
    
    if grep -q "^audio_mute_enable" "$CONFIG_FILE"; then
        sed -i 's|^audio_mute_enable.*|audio_mute_enable = "false"|' "$CONFIG_FILE"
    else
        echo "audio_mute_enable = \"false\"" >> "$CONFIG_FILE"
    fi
    
    # Audio volume - set to 100% (1.0) to ensure sound is audible
    if grep -q "^audio_volume" "$CONFIG_FILE"; then
        sed -i 's|^audio_volume.*|audio_volume = "1.0"|' "$CONFIG_FILE"
    else
        echo "audio_volume = \"1.0\"" >> "$CONFIG_FILE"
    fi
    
    # Audio latency and resampler settings
    if grep -q "^audio_latency" "$CONFIG_FILE"; then
        sed -i 's|^audio_latency.*|audio_latency = "64"|' "$CONFIG_FILE"
    else
        echo "audio_latency = \"64\"" >> "$CONFIG_FILE"
    fi
    
    if grep -q "^audio_resampler" "$CONFIG_FILE"; then
        sed -i 's|^audio_resampler.*|audio_resampler = "sinc"|' "$CONFIG_FILE"
    else
        echo "audio_resampler = \"sinc\"" >> "$CONFIG_FILE"
    fi
    
    # Audio output rate - set to 48000 Hz (common HDMI rate)
    if grep -q "^audio_out_rate" "$CONFIG_FILE"; then
        sed -i 's|^audio_out_rate.*|audio_out_rate = "48000"|' "$CONFIG_FILE"
    else
        echo "audio_out_rate = \"48000\"" >> "$CONFIG_FILE"
    fi
    
    # Audio sync - enable for smooth playback
    if grep -q "^audio_sync" "$CONFIG_FILE"; then
        sed -i 's|^audio_sync.*|audio_sync = "true"|' "$CONFIG_FILE"
    else
        echo "audio_sync = \"true\"" >> "$CONFIG_FILE"
    fi
    
    # CRITICAL: Prevent RetroArch from saving config (keeps our settings)
    if grep -q "^config_save_on_exit" "$CONFIG_FILE"; then
        sed -i 's|^config_save_on_exit.*|config_save_on_exit = "false"|' "$CONFIG_FILE"
    else
        echo "config_save_on_exit = \"false\"" >> "$CONFIG_FILE"
    fi
    
    # Ensure HDMI audio volume is set
    amixer -c 1 set PCM 100% unmute 2>/dev/null || true
    
    echo "$(date): Audio configuration complete - driver: $AUDIO_DRIVER, device: ${AUDIO_DEVICE:-default}, enabled: true, muted: false, volume: 1.0" >> "$LOG_FILE"
    
    # CRITICAL: Disable remap files - use RetroArch's default autoconfig instead
    # This allows RetroArch to automatically detect and configure controllers
    sed -i '/^input_remap_bind_enable/d' "$CONFIG_FILE"
    echo "input_remap_bind_enable = \"false\"" >> "$CONFIG_FILE"
    echo "$(date): Disabled remap files - using RetroArch default autoconfig" >> "$LOG_FILE"
    
    # CRITICAL: Check if unzip is available (needed for extracting core info files)
    if ! command -v unzip >/dev/null 2>&1; then
        echo "$(date): WARNING: unzip not found - core info extraction may fail" >> "$LOG_FILE"
        echo "$(date): Installing unzip..." >> "$LOG_FILE"
        sudo apt-get update -qq && sudo apt-get install -y unzip 2>>"$LOG_FILE" || {
            echo "$(date): ERROR: Failed to install unzip" >> "$LOG_FILE"
        }
    else
        echo "$(date): unzip utility found: $(which unzip)" >> "$LOG_FILE"
    fi
    
    # CRITICAL: Check if curl or wget is available
    DOWNLOAD_TOOL=""
    if command -v curl >/dev/null 2>&1; then
        DOWNLOAD_TOOL="curl"
        echo "$(date): curl utility found: $(which curl)" >> "$LOG_FILE"
    elif command -v wget >/dev/null 2>&1; then
        DOWNLOAD_TOOL="wget"
        echo "$(date): wget utility found: $(which wget)" >> "$LOG_FILE"
    else
        echo "$(date): WARNING: Neither curl nor wget found - installing curl..." >> "$LOG_FILE"
        sudo apt-get update -qq && sudo apt-get install -y curl 2>>"$LOG_FILE" || {
            echo "$(date): ERROR: Failed to install curl" >> "$LOG_FILE"
        }
        if command -v curl >/dev/null 2>&1; then
            DOWNLOAD_TOOL="curl"
        fi
    fi
    
    # CRITICAL: Pre-download core info files if directory is empty
    # This ensures RetroArch has the info files it needs to show core names
    if [ -d "$INFO_DIR" ] && [ -z "$(ls -A "$INFO_DIR"/*.info 2>/dev/null)" ]; then
        echo "$(date): Info directory is empty - attempting to pre-download core info files" >> "$LOG_FILE"
        INFO_ZIP="/tmp/retroarch_info.zip"
        INFO_ZIP_URL="https://buildbot.libretro.com/assets/frontend/info.zip"
        DOWNLOAD_SUCCESS=false
        
        # Try multiple download methods for robustness
        if [ "$DOWNLOAD_TOOL" = "curl" ]; then
            # Method 1: curl with SSL verification
            if curl -L -f --connect-timeout 30 --max-time 120 -s "$INFO_ZIP_URL" -o "$INFO_ZIP" 2>>"$LOG_FILE"; then
                if [ -f "$INFO_ZIP" ] && [ -s "$INFO_ZIP" ]; then
                    FILE_SIZE=$(stat -f%z "$INFO_ZIP" 2>/dev/null || stat -c%s "$INFO_ZIP" 2>/dev/null)
                    if [ "$FILE_SIZE" -gt 1000 ]; then
                        echo "$(date): Downloaded core info zip file ($FILE_SIZE bytes)" >> "$LOG_FILE"
                        DOWNLOAD_SUCCESS=true
                    else
                        rm -f "$INFO_ZIP"
                    fi
                fi
            fi
            
            # Method 2: curl without SSL verification (if Method 1 failed)
            if [ "$DOWNLOAD_SUCCESS" = false ]; then
                echo "$(date): Retrying download without SSL verification..." >> "$LOG_FILE"
                if curl -L -f -k --connect-timeout 30 --max-time 120 -s "$INFO_ZIP_URL" -o "$INFO_ZIP" 2>>"$LOG_FILE"; then
                    if [ -f "$INFO_ZIP" ] && [ -s "$INFO_ZIP" ]; then
                        FILE_SIZE=$(stat -f%z "$INFO_ZIP" 2>/dev/null || stat -c%s "$INFO_ZIP" 2>/dev/null)
                        if [ "$FILE_SIZE" -gt 1000 ]; then
                            echo "$(date): Downloaded core info zip file without SSL verification ($FILE_SIZE bytes)" >> "$LOG_FILE"
                            DOWNLOAD_SUCCESS=true
                        else
                            rm -f "$INFO_ZIP"
                        fi
                    fi
                fi
            fi
        elif [ "$DOWNLOAD_TOOL" = "wget" ]; then
            # Method 1: wget with SSL verification
            if wget --timeout=30 --tries=3 -q "$INFO_ZIP_URL" -O "$INFO_ZIP" 2>>"$LOG_FILE"; then
                if [ -f "$INFO_ZIP" ] && [ -s "$INFO_ZIP" ]; then
                    FILE_SIZE=$(stat -f%z "$INFO_ZIP" 2>/dev/null || stat -c%s "$INFO_ZIP" 2>/dev/null)
                    if [ "$FILE_SIZE" -gt 1000 ]; then
                        echo "$(date): Downloaded core info zip file ($FILE_SIZE bytes)" >> "$LOG_FILE"
                        DOWNLOAD_SUCCESS=true
                    else
                        rm -f "$INFO_ZIP"
                    fi
                fi
            fi
            
            # Method 2: wget without SSL verification (if Method 1 failed)
            if [ "$DOWNLOAD_SUCCESS" = false ]; then
                echo "$(date): Retrying download without SSL verification..." >> "$LOG_FILE"
                if wget --no-check-certificate --timeout=30 --tries=3 -q "$INFO_ZIP_URL" -O "$INFO_ZIP" 2>>"$LOG_FILE"; then
                    if [ -f "$INFO_ZIP" ] && [ -s "$INFO_ZIP" ]; then
                        FILE_SIZE=$(stat -f%z "$INFO_ZIP" 2>/dev/null || stat -c%s "$INFO_ZIP" 2>/dev/null)
                        if [ "$FILE_SIZE" -gt 1000 ]; then
                            echo "$(date): Downloaded core info zip file without SSL verification ($FILE_SIZE bytes)" >> "$LOG_FILE"
                            DOWNLOAD_SUCCESS=true
                        else
                            rm -f "$INFO_ZIP"
                        fi
                    fi
                fi
            fi
        fi
        
        # Extract if download succeeded
        if [ "$DOWNLOAD_SUCCESS" = true ]; then
            if unzip -o "$INFO_ZIP" -d "$INFO_DIR" 2>>"$LOG_FILE"; then
                INFO_COUNT=$(ls -1 "$INFO_DIR"/*.info 2>/dev/null | wc -l)
                echo "$(date): Successfully extracted $INFO_COUNT core info files" >> "$LOG_FILE"
                rm -f "$INFO_ZIP"
            else
                echo "$(date): WARNING: Failed to extract core info zip" >> "$LOG_FILE"
                rm -f "$INFO_ZIP"
            fi
        else
            echo "$(date): WARNING: Failed to download core info zip with all methods" >> "$LOG_FILE"
            echo "$(date): You may need to run: scripts/fix_retroarch_core_info.sh" >> "$LOG_FILE"
        fi
    else
        INFO_COUNT=$(ls -1 "$INFO_DIR"/*.info 2>/dev/null | wc -l)
        echo "$(date): Core info directory already has $INFO_COUNT info files" >> "$LOG_FILE"
    fi
    
    # Ensure core updater menu items are visible
    sed -i '/^menu_show_core_updater/d' "$CONFIG_FILE"
    echo "menu_show_core_updater = \"true\"" >> "$CONFIG_FILE"
    sed -i '/^menu_show_online_updater/d' "$CONFIG_FILE"
    echo "menu_show_online_updater = \"true\"" >> "$CONFIG_FILE"
    
    # CRITICAL: Force lower resolution for better performance on Pi
    # Use custom viewport to render at reduced resolution
    # Force extremely low internal resolution for maximum performance (96x72)
    
    # CRITICAL: Use windowed fullscreen instead of true fullscreen
    # True fullscreen can cause blank screens when X server resolution doesn't match display
    # Windowed fullscreen is more reliable and still fills the screen
    sed -i '/^video_fullscreen/d' "$CONFIG_FILE"
    echo "video_fullscreen = \"false\"" >> "$CONFIG_FILE"  # Use windowed fullscreen

    # Enable windowed fullscreen - more reliable on Pi/X11
    sed -i '/^video_windowed_fullscreen/d' "$CONFIG_FILE"
    echo "video_windowed_fullscreen = \"true\"" >> "$CONFIG_FILE"
    
    # Set fullscreen resolution to CRT native (720x480)
    # This matches the Magic Dingus Box CRT native resolution
    FULLSCREEN_X="720"
    FULLSCREEN_Y="480"
    sed -i '/^video_fullscreen_x/d' "$CONFIG_FILE"
    echo "video_fullscreen_x = \"$FULLSCREEN_X\"" >> "$CONFIG_FILE"
    sed -i '/^video_fullscreen_y/d' "$CONFIG_FILE"
    echo "video_fullscreen_y = \"$FULLSCREEN_Y\"" >> "$CONFIG_FILE"
    # CRITICAL: Also set windowed size to force 720x480
    sed -i '/^video_windowed_width/d' "$CONFIG_FILE"
    echo "video_windowed_width = \"$FULLSCREEN_X\"" >> "$CONFIG_FILE"
    sed -i '/^video_windowed_height/d' "$CONFIG_FILE"
    echo "video_windowed_height = \"$FULLSCREEN_Y\"" >> "$CONFIG_FILE"
    echo "$(date): Set RetroArch fullscreen resolution to CRT native: ${FULLSCREEN_X}x${FULLSCREEN_Y}" >> "$LOG_FILE"
    
    # CRITICAL: Let games render at their NATIVE resolution (e.g., PS1 at 320x240)
    # RetroArch will then scale/stretch the native resolution to fill the 720x480 window
    # Disable custom viewport so games render at their actual native resolution
    sed -i '/^custom_viewport/d' "$CONFIG_FILE"
    sed -i '/^video_custom_viewport/d' "$CONFIG_FILE"
    echo "video_custom_viewport_enable = \"false\"" >> "$CONFIG_FILE"
    
    # CRITICAL: Disable any resolution overrides - let games use their native resolution
    sed -i '/^video_refresh_rate/d' "$CONFIG_FILE"
    sed -i '/^video_max_swapchain_images/d' "$CONFIG_FILE"
    
    # Set aspect ratio for 4:3 content (1.333 = 4/3)
    sed -i '/^video_aspect_ratio/d' "$CONFIG_FILE"
    echo "video_aspect_ratio = \"1.333\"" >> "$CONFIG_FILE"  # 4:3 aspect ratio
    
    # Force aspect ratio to maintain 4:3 when scaling
    sed -i '/^video_force_aspect/d' "$CONFIG_FILE"
    echo "video_force_aspect = \"true\"" >> "$CONFIG_FILE"  # Force 4:3 aspect ratio
    
    # CRITICAL: Enable scaling so games scale from native resolution to fill 720x480
    # Disable integer scaling to allow stretching (not pixel-perfect, but fills screen)
    sed -i '/^video_scale_integer/d' "$CONFIG_FILE"
    echo "video_scale_integer = \"false\"" >> "$CONFIG_FILE"  # Allow non-integer scaling to fill screen
    
    # Set scale to auto - RetroArch will scale from native resolution to fill 720x480
    sed -i '/^video_scale/d' "$CONFIG_FILE"
    echo "video_scale = \"1.0\"" >> "$CONFIG_FILE"  # Auto-scale to fill fullscreen
    
    # Use nearest neighbor scaling (fastest, maintains pixel art look)
    sed -i '/^video_scale_filter/d' "$CONFIG_FILE"
    echo "video_scale_filter = \"0\"" >> "$CONFIG_FILE"  # Nearest neighbor (fastest)
    
    # CRITICAL: Disable threaded video to ensure proper rendering
    sed -i '/^video_threaded/d' "$CONFIG_FILE"
    echo "video_threaded = \"false\"" >> "$CONFIG_FILE"
    
    # Disable smooth scaling - we want pixel-perfect scaling from native resolution
    sed -i '/^video_smooth/d' "$CONFIG_FILE"
    echo "video_smooth = \"false\"" >> "$CONFIG_FILE"
    
    # Disable crop overscan (saves processing)
    sed -i '/^video_crop_overscan/d' "$CONFIG_FILE"
    echo "video_crop_overscan = \"false\"" >> "$CONFIG_FILE"
    
    # Enable aspect ratio correction
    # For PS1, force 4:3 aspect ratio (most PS1 games are 4:3)
    sed -i '/^aspect_ratio_index/d' "$CONFIG_FILE"
    echo "aspect_ratio_index = \"23\"" >> "$CONFIG_FILE"  # 4:3 aspect ratio
    
    # CRITICAL: Disable frame blending and other expensive effects
    sed -i '/^video_frame_blend/d' "$CONFIG_FILE"
    echo "video_frame_blend = \"false\"" >> "$CONFIG_FILE"
    
    sed -i '/^video_gpu_screenshot/d' "$CONFIG_FILE"
    echo "video_gpu_screenshot = \"false\"" >> "$CONFIG_FILE"
    
    echo "$(date): Configured for reduced resolution (128x120) using custom viewport with 8x scaling" >> "$LOG_FILE"
    echo "$(date): Updated RetroArch config for controller support and fullscreen (autodetect, udev driver, ozone menu, 1920x1080)" >> "$LOG_FILE"
else
    # Create new config file with controller settings
    cat > "$CONFIG_FILE" <<EOF
# RetroArch config for Magic Dingus Box
# Controller settings - optimized for automatic detection
input_joypad_driver = "udev"
input_autodetect_enable = "true"
input_auto_game_focus = "true"
menu_driver = "ozone"
input_menu_toggle_gamepad_combo = "2+3"
input_enable_hotkey = "true"
input_hotkey_block_delay = "0"
# Controller autoconfiguration
input_player1_joypad_index = "0"
input_autoconfig_enable = "true"
input_joypad_driver_mapping_dir = ""
# Core updater configuration
core_updater_buildbot_cores_url = "https://buildbot.libretro.com/nightly/linux/aarch64/latest"
core_updater_buildbot_assets_url = "https://buildbot.libretro.com/assets/"
core_updater_auto_extract_archive = "true"
menu_show_core_updater = "true"
menu_show_online_updater = "true"
# Allow controller to work in menus without hotkey blocking
EOF
    echo "$(date): Created RetroArch config with controller support" >> "$LOG_FILE"
fi

# CRITICAL: Verify controller devices are accessible before launching RetroArch
echo "$(date): Verifying controller device accessibility" >> "$LOG_FILE"
CONTROLLER_FOUND=false
CONTROLLER_INDEX=0
for js_device in /dev/input/js*; do
    if [ -c "$js_device" ] && [ -r "$js_device" ]; then
        CONTROLLER_FOUND=true
        echo "$(date): Controller device accessible: $js_device (index: $CONTROLLER_INDEX)" >> "$LOG_FILE"
        # Get device info for logging
        DEVICE_INFO=$(udevadm info --query=property --name="$js_device" 2>/dev/null | grep -E "ID_SERIAL|ID_MODEL|ID_VENDOR" | head -3 || echo "unknown")
        echo "$(date): Device info: $DEVICE_INFO" >> "$LOG_FILE"
        
        # Update config to use this controller as player 1
        if [ "$CONTROLLER_INDEX" -eq 0 ]; then
            sed -i '/^input_player1_joypad_index/d' "$CONFIG_FILE"
            echo "input_player1_joypad_index = \"$CONTROLLER_INDEX\"" >> "$CONFIG_FILE"
            echo "$(date): Set input_player1_joypad_index = $CONTROLLER_INDEX" >> "$LOG_FILE"
        fi
        CONTROLLER_INDEX=$((CONTROLLER_INDEX + 1))
    fi
done

if [ "$CONTROLLER_FOUND" = "false" ]; then
    echo "$(date): WARNING: No accessible controller devices found - RetroArch may not detect controller" >> "$LOG_FILE"
else
    echo "$(date): Controller devices verified and ready for RetroArch (found $CONTROLLER_INDEX controller(s))" >> "$LOG_FILE"
fi

# CRITICAL: Ensure autoconfig directory exists and is accessible
AUTOCONFIG_DIR="$CONFIG_DIR/autoconfig"
mkdir -p "$AUTOCONFIG_DIR"
echo "$(date): Autoconfig directory: $AUTOCONFIG_DIR" >> "$LOG_FILE"

# CRITICAL: Try to download controller profiles if RetroArch supports it
# This ensures Switch controller and other controllers are automatically configured
echo "$(date): Attempting to ensure controller profiles are available" >> "$LOG_FILE"
# Note: RetroArch's autoconfig system should handle this, but we can verify
# The autoconfig directory should contain .cfg files for each controller type
# If empty, RetroArch will try to auto-detect and configure on first use
if [ -d "$AUTOCONFIG_DIR" ] && [ -z "$(ls -A "$AUTOCONFIG_DIR" 2>/dev/null)" ]; then
    echo "$(date): Autoconfig directory is empty - attempting to download controller profiles" >> "$LOG_FILE"
    # Try to download controller profiles using RetroArch's built-in updater
    # This requires RetroArch to be run with --update-controller-profiles flag
    # But we can't do this here since we're about to launch RetroArch
    # Instead, we'll create a basic autoconfig file for the detected controller
    echo "$(date): Will create basic autoconfig if controller is detected" >> "$LOG_FILE"
fi

# CRITICAL: Create a basic autoconfig file for the Switch controller if detected
# This ensures RetroArch can use the controller even without downloaded profiles
if [ "$CONTROLLER_FOUND" = "true" ]; then
    # Get controller vendor/model info
    JS_DEVICE="/dev/input/js0"
    if [ -c "$JS_DEVICE" ] && [ -r "$JS_DEVICE" ]; then
        VENDOR_ID=$(udevadm info --query=property --name="$JS_DEVICE" 2>/dev/null | grep "ID_VENDOR_ID" | cut -d= -f2 || echo "")
        MODEL_ID=$(udevadm info --query=property --name="$JS_DEVICE" 2>/dev/null | grep "ID_MODEL_ID" | cut -d= -f2 || echo "")
        VENDOR_NAME=$(udevadm info --query=property --name="$JS_DEVICE" 2>/dev/null | grep "ID_VENDOR=" | cut -d= -f2 | tr '[:upper:]' '[:lower:]' | tr ' ' '_' | tr -cd '[:alnum:]_' || echo "")
        MODEL_NAME=$(udevadm info --query=property --name="$JS_DEVICE" 2>/dev/null | grep "ID_MODEL=" | cut -d= -f2 | tr '[:upper:]' '[:lower:]' | tr ' ' '_' | tr -cd '[:alnum:]_' || echo "")
        
        if [ -n "$VENDOR_ID" ] && [ -n "$MODEL_ID" ]; then
            # Convert hex vendor/model IDs to decimal (RetroArch expects decimal)
            VENDOR_ID_DEC=$(printf "%d" "0x$VENDOR_ID" 2>/dev/null || echo "$VENDOR_ID")
            MODEL_ID_DEC=$(printf "%d" "0x$MODEL_ID" 2>/dev/null || echo "$MODEL_ID")
            
            # Create autoconfig filename based on vendor/model IDs (hex for filename, decimal for content)
            AUTOCONFIG_FILE="$AUTOCONFIG_DIR/udev/${VENDOR_ID}_${MODEL_ID}.cfg"
            mkdir -p "$(dirname "$AUTOCONFIG_FILE")"
            
            # Always recreate the autoconfig file to ensure it's correct
            echo "$(date): Creating/updating autoconfig file: $AUTOCONFIG_FILE" >> "$LOG_FILE"
            echo "$(date): Vendor ID: $VENDOR_ID (hex) = $VENDOR_ID_DEC (decimal)" >> "$LOG_FILE"
            echo "$(date): Model ID: $MODEL_ID (hex) = $MODEL_ID_DEC (decimal)" >> "$LOG_FILE"
            
            # Create a basic autoconfig file for udev driver
            # RetroArch expects vendor/product IDs in DECIMAL format
            # For Switch Pro Controller (0e6d:111d), use correct button mappings
            if [ "$VENDOR_ID" = "0e6d" ] && [ "$MODEL_ID" = "111d" ]; then
                # N64 Controller specific mappings
                cat > "$AUTOCONFIG_FILE" <<EOF
# RetroArch Autoconfig for N64 Controller (SWITCH CO.,LTD.)
# Vendor ID: $VENDOR_ID (hex) = $VENDOR_ID_DEC (decimal)
# Product ID: $MODEL_ID (hex) = $MODEL_ID_DEC (decimal)
input_device = "SWITCH CO.,LTD. Controller"
input_driver = "udev"
input_vendor_id = "$VENDOR_ID_DEC"
input_product_id = "$MODEL_ID_DEC"

# N64 Controller Layout Mapping:
# Main buttons
input_a_btn = "0"      # A button (main action, bottom)
input_b_btn = "1"      # B button (secondary action, top)
input_start_btn = "2"   # Start button

# D-pad (up, down, left, right)
input_up_btn = "h0up"
input_down_btn = "h0down"
input_left_btn = "h0left"
input_right_btn = "h0right"

# C-buttons (the four arrows: up, down, left, right)
# These will be mapped by the N64 core to appropriate inputs
input_y_btn = "3"      # C-up (mapped to Y button for N64)
input_x_btn = "4"      # C-down (mapped to X button for N64)
input_l_btn = "5"      # C-left (mapped to L button for N64)
input_r_btn = "6"      # C-right (mapped to R button for N64)

# Shoulder buttons and trigger
input_l2_btn = "7"     # L button
input_r2_btn = "8"      # R button
input_z_btn = "9"       # Z button (trigger)

# Analog stick (main joystick)
input_l_x_plus_axis = "+0"
input_l_x_minus_axis = "-0"
input_l_y_plus_axis = "+1"
input_l_y_minus_axis = "-1"

# Right stick (for C-buttons if core supports it)
input_r_x_plus_axis = "+2"
input_r_x_minus_axis = "-2"
input_r_y_plus_axis = "+3"
input_r_y_minus_axis = "-3"

# Additional buttons (if present)
input_select_btn = "10"
input_l3_btn = "11"
input_r3_btn = "12"
EOF
            else
                # Generic controller mappings
                cat > "$AUTOCONFIG_FILE" <<EOF
# Autoconfig file for Magic Dingus Box
# Controller: $VENDOR_NAME $MODEL_NAME
# Vendor ID: $VENDOR_ID (hex) = $VENDOR_ID_DEC (decimal)
# Model ID: $MODEL_ID (hex) = $MODEL_ID_DEC (decimal)
input_device = "$VENDOR_NAME $MODEL_NAME"
input_driver = "udev"
input_vendor_id = "$VENDOR_ID_DEC"
input_product_id = "$MODEL_ID_DEC"

# Basic button mappings (generic - may need adjustment)
input_a_btn = "0"
input_b_btn = "1"
input_x_btn = "2"
input_y_btn = "3"
input_l_btn = "4"
input_r_btn = "5"
input_select_btn = "6"
input_start_btn = "7"
input_l3_btn = "8"
input_r3_btn = "9"

# D-pad (hat 0)
input_up_btn = "h0up"
input_down_btn = "h0down"
input_left_btn = "h0left"
input_right_btn = "h0right"

# Left stick (axis 0 and 1)
input_l_x_plus_axis = "+0"
input_l_x_minus_axis = "-0"
input_l_y_plus_axis = "+1"
input_l_y_minus_axis = "-1"

# Right stick (axis 2 and 3)
input_r_x_plus_axis = "+2"
input_r_x_minus_axis = "-2"
input_r_y_plus_axis = "+3"
input_r_y_minus_axis = "-3"

# Triggers (axis 4 and 5, if present)
input_l2_axis = "+4"
input_r2_axis = "+5"
EOF
            fi
            echo "$(date): Created/updated autoconfig file: $AUTOCONFIG_FILE" >> "$LOG_FILE"
            echo "$(date): Autoconfig file contents:" >> "$LOG_FILE"
            cat "$AUTOCONFIG_FILE" >> "$LOG_FILE" 2>/dev/null || true
        else
            echo "$(date): Could not determine vendor/model IDs for controller" >> "$LOG_FILE"
        fi
    fi
fi

# Build RetroArch command
# CRITICAL: Always use --verbose to see controller detection issues
if [ "$IS_CORE_DOWNLOADER" = "true" ]; then
    # Core Downloader: launch with --menu flag
    # Don't use --fullscreen flag - let config file handle windowed fullscreen
    # Command-line --fullscreen forces true fullscreen which can cause blank screens
    CMD=(
        "$RETROARCH_BIN"
        "--menu"
        "--verbose"
        "--config" "$CONFIG_FILE"
    )
else
    # Game launch: use core and ROM
    # Don't use --fullscreen flag - let config file handle windowed fullscreen
    # Command-line --fullscreen forces true fullscreen which can cause blank screens
    CMD=(
        "$RETROARCH_BIN"
        "-L" "$CORE_NAME"
        "$ROM_PATH"
        "--verbose"
        "--config" "$CONFIG_FILE"
    )
fi

# Add overlay config if provided
TEMP_CONFIG=""
if [ -n "$OVERLAY_PATH" ] && [ -f "$OVERLAY_PATH" ]; then
    OVERLAY_CFG="${OVERLAY_PATH%.png}.cfg"
    if [ -f "$OVERLAY_CFG" ]; then
        # CRITICAL: Create a completely fresh temp config file
        # Remove any existing temp configs to avoid conflicts
        rm -f /tmp/retroarch_config_*.cfg 2>/dev/null || true
        TEMP_CONFIG=$(mktemp /tmp/retroarch_config_XXXXXX.cfg)
        
        # Get audio settings from main config to include in temp config
        AUDIO_DRIVER=$(grep "^audio_driver" "$CONFIG_FILE" 2>/dev/null | tail -1 | cut -d'"' -f2 || echo "alsa")
        AUDIO_DEVICE=$(grep "^audio_device" "$CONFIG_FILE" 2>/dev/null | tail -1 | cut -d'"' -f2 || echo "")
        
        # CRITICAL: Write a complete, fresh config file (not appending)
        # This ensures no old/bad settings are inherited
        cat > "$TEMP_CONFIG" <<EOF
# Temporary RetroArch config for Magic Dingus Box
# Created fresh each launch to avoid config conflicts

# Overlay settings
input_overlay_enable = "true"
input_overlay = "$OVERLAY_CFG"
input_overlay_opacity = "1.0"
input_overlay_scale = "1.0"

# Video settings - Windowed fullscreen for better compatibility at CRT native (720x480)
video_fullscreen = "false"
video_windowed_fullscreen = "true"
video_fullscreen_x = "720"
video_fullscreen_y = "480"
# CRITICAL: Force window size to match CRT native resolution
video_windowed_width = "720"
video_windowed_height = "480"
# Disable custom viewport - let games render at their NATIVE resolution (e.g., 320x240 for PS1)
# RetroArch will then scale/stretch the native resolution to fill 720x480
video_custom_viewport_enable = "false"
# Aspect ratio for 4:3 content
aspect_ratio_index = "23"
video_aspect_ratio = "1.333"
video_force_aspect = "true"
# Scaling settings - scale from native resolution to fill screen (stretch, not re-render)
video_scale_integer = "false"
video_scale = "1.0"
video_scale_filter = "0"
# Disable smooth scaling - maintain pixel art look
video_smooth = "false"
# Disable threaded video for proper rendering
video_threaded = "false"

# Controller support
input_autodetect_enable = "true"
input_joypad_driver = "udev"
input_auto_game_focus = "true"

# CRITICAL: Controller input settings
input_autodetect_enable = "true"
input_joypad_driver = "udev"
input_auto_game_focus = "true"
input_game_focus_enable = "true"
input_block_timeout = "0"

# CRITICAL: Hotkey settings for controller menu access
input_enable_hotkey = "true"
input_hotkey_block_delay = "0"
input_menu_toggle_gamepad_combo = "2+3"
# CRITICAL: Prevent RetroArch from saving config (keeps our settings)
config_save_on_exit = "false"

# Audio settings - use detected audio device
audio_driver = "$AUDIO_DRIVER"
audio_device = "$AUDIO_DEVICE"
audio_enable = "true"
audio_mute_enable = "false"
audio_volume = "1.0"
audio_out_rate = "48000"
audio_latency = "64"
audio_sync = "true"
audio_resampler = "sinc"
EOF
            
        CMD+=(--config "$TEMP_CONFIG")
        echo "$(date): Created fresh temp config with overlay, audio, and controller settings: $(basename "$OVERLAY_PATH")" >> "$LOG_FILE"
        echo "$(date): Temp config controller settings: input_joypad_driver=udev, input_menu_toggle_gamepad_combo=2+3" >> "$LOG_FILE"
    fi
fi

# CRITICAL: Ensure MPV is stopped before launching RetroArch
echo "$(date): Stopping MPV processes" >> "$LOG_FILE"
pkill -9 mpv 2>>"$LOG_FILE" || true
pkill -9 -f "mpv.*socket" 2>>"$LOG_FILE" || true
sleep 0.5

# CRITICAL: Ensure no other windows are on top
echo "$(date): Ensuring RetroArch window will be on top" >> "$LOG_FILE"
# Kill any remaining pygame/mpv windows that might be blocking
DISPLAY=:0 xdotool search --class pygame windowkill 2>/dev/null || true
DISPLAY=:0 xdotool search --class mpv windowkill 2>/dev/null || true
sleep 0.5

# Launch RetroArch and wait for it to exit
echo "$(date): Launching RetroArch: ${CMD[*]}" >> "$LOG_FILE"
echo "$(date): RetroArch command will run in foreground, blocking until exit" >> "$LOG_FILE"
echo "$(date): Current working directory: $(pwd)" >> "$LOG_FILE"
echo "$(date): DISPLAY variable: $DISPLAY" >> "$LOG_FILE"
RETROARCH_EXIT=0
# Run RetroArch in foreground - it will block until user exits
# Ensure DISPLAY is set and run RetroArch
export DISPLAY=:0

# CRITICAL: Set environment variables to force hardware rendering
# These ensure RetroArch uses GPU acceleration instead of software rendering (llvmpipe)
# Only set MESA_LOADER_DRIVER_OVERRIDE to force VC4 driver - other vars can cause issues
export LIBGL_ALWAYS_SOFTWARE=0
export MESA_LOADER_DRIVER_OVERRIDE=vc4
echo "$(date): Set hardware rendering environment variables (MESA_LOADER_DRIVER_OVERRIDE=vc4)" >> "$LOG_FILE"

# Launch RetroArch in background and capture PID
# CRITICAL: Use exec to ensure proper process tracking for wait command
# Environment variables above ensure hardware rendering
"${CMD[@]}" >> "$LOG_FILE" 2>&1 &
RETROARCH_PID=$!
echo "$(date): RetroArch launched with PID: $RETROARCH_PID" >> "$LOG_FILE"
# Verify PID was captured correctly
if [ -z "$RETROARCH_PID" ] || [ "$RETROARCH_PID" = "0" ]; then
    echo "$(date): ERROR: Failed to capture RetroArch PID" >> "$LOG_FILE"
    exit 1
fi
# Verify process actually started
sleep 0.5
if ! ps -p $RETROARCH_PID >/dev/null 2>&1; then
    echo "$(date): ERROR: RetroArch process $RETROARCH_PID not found after launch" >> "$LOG_FILE"
    exit 1
fi
echo "$(date): Verified RetroArch process $RETROARCH_PID is running" >> "$LOG_FILE"

# CRITICAL: Create lock file NOW that we have RetroArch PID
# This allows the UI service to check and exit immediately if RetroArch is active
# More efficient than background polling - UI checks once at startup
echo "$(date): Creating RetroArch lock file: $RETROARCH_LOCK_FILE" >> "$LOG_FILE"
echo "$RETROARCH_PID" > "$RETROARCH_LOCK_FILE"
echo "$(date): Lock file created with PID: $RETROARCH_PID" >> "$LOG_FILE"

# CRITICAL: Force RetroArch window to be fullscreen 720x480
# RetroArch windowed fullscreen doesn't always work correctly, so we need to manually resize
echo "$(date): Will force RetroArch window to 720x480 fullscreen after it appears" >> "$LOG_FILE"

# Also start a lightweight background monitor as backup
# This ensures the lock file is cleaned up even if RetroArch crashes
echo "$(date): Starting lightweight lock file monitor" >> "$LOG_FILE"
(
    while true; do
        sleep 2  # Check every 2 seconds (lighter than before)
        # Check if RetroArch is still running
        if ! ps -p $RETROARCH_PID >/dev/null 2>&1; then
            # RetroArch exited, remove lock file
            rm -f "$RETROARCH_LOCK_FILE" 2>/dev/null
            echo "$(date): [MONITOR] RetroArch exited, removed lock file" >> "$LOG_FILE"
            break
        fi
        # Update lock file timestamp (touch it) to show it's still active
        touch "$RETROARCH_LOCK_FILE" 2>/dev/null || true
    done
    echo "$(date): [MONITOR] Lock file monitor stopped" >> "$LOG_FILE"
) &
MONITOR_PID=$!
echo "$(date): Lock file monitor started (PID: $MONITOR_PID)" >> "$LOG_FILE"

# Using TRUE fullscreen mode - full window approach (as configured previously)
# True fullscreen eliminates window decorations and cursor automatically
echo "$(date): Using true fullscreen mode - full window approach" >> "$LOG_FILE"

# Wait for RetroArch window to appear and raise it to top
# RetroArch can take several seconds to fully initialize and create its window
echo "$(date): Waiting for RetroArch window to appear (up to 15 seconds)" >> "$LOG_FILE"
RETROARCH_WINDOW=""
for i in {1..75}; do  # 75 attempts * 0.2s = 15 seconds max wait
    sleep 0.2
    RETROARCH_WINDOW=$(DISPLAY=:0 xdotool search --class retroarch 2>/dev/null | head -1)
    if [ -n "$RETROARCH_WINDOW" ]; then
        echo "$(date): Found RetroArch window: $RETROARCH_WINDOW (after ${i} attempts, ~$((i * 2 / 10)).$((i * 2 % 10))s)" >> "$LOG_FILE"
        
        # Wait a bit more for window to be fully ready
        sleep 0.5
        
        # Verify window is still there and get its properties
        WINDOW_NAME=$(DISPLAY=:0 xdotool getwindowname "$RETROARCH_WINDOW" 2>/dev/null || echo "")
        WINDOW_GEOM=$(DISPLAY=:0 xdotool getwindowgeometry "$RETROARCH_WINDOW" 2>/dev/null || echo "")
        echo "$(date): Window name: $WINDOW_NAME" >> "$LOG_FILE"
        echo "$(date): Window geometry: $WINDOW_GEOM" >> "$LOG_FILE"
        
        # CRITICAL: Ensure RetroArch window is visible and on top
        # We need to raise it and give it focus so it's actually visible
        echo "$(date): RetroArch window found - ensuring it's visible and on top" >> "$LOG_FILE"
        
        # Ensure window is mapped (visible)
        DISPLAY=:0 xdotool windowmap "$RETROARCH_WINDOW" 2>>"$LOG_FILE" || true
        sleep 0.2
        
        # CRITICAL: Force window to be 720x480 and positioned at 0,0
        # RetroArch windowed fullscreen doesn't always work, so manually resize
        echo "$(date): Forcing RetroArch window to 720x480 at position 0,0" >> "$LOG_FILE"
        # Remove any window decorations that might offset position
        DISPLAY=:0 xprop -id "$RETROARCH_WINDOW" -f _MOTIF_WM_HINTS 32c -set _MOTIF_WM_HINTS "0x2, 0x0, 0x0, 0x0, 0x0" 2>>"$LOG_FILE" || true
        # Move and resize multiple times to ensure it sticks
        for resize_attempt in {1..3}; do
            DISPLAY=:0 xdotool windowmove "$RETROARCH_WINDOW" 0 0 2>>"$LOG_FILE" || true
            sleep 0.1
            DISPLAY=:0 xdotool windowsize "$RETROARCH_WINDOW" 720 480 2>>"$LOG_FILE" || true
            sleep 0.1
        done
        sleep 0.2
        # Verify the resize worked
        NEW_GEOM=$(DISPLAY=:0 xdotool getwindowgeometry "$RETROARCH_WINDOW" 2>/dev/null | grep Geometry | awk '{print $2}' || echo "")
        NEW_POS=$(DISPLAY=:0 xdotool getwindowgeometry "$RETROARCH_WINDOW" 2>/dev/null | grep Position | awk '{print $2}' || echo "")
        echo "$(date): Window after resize - geometry: $NEW_GEOM, position: $NEW_POS" >> "$LOG_FILE"
        
        # CRITICAL: Remove window decorations and make it borderless
        # This ensures the window fills the entire screen without any borders
        DISPLAY=:0 xprop -id "$RETROARCH_WINDOW" -f _MOTIF_WM_HINTS 32c -set _MOTIF_WM_HINTS "0x2, 0x0, 0x0, 0x0, 0x0" 2>>"$LOG_FILE" || true
        # Also try removing decorations via _NET_WM_WINDOW_TYPE
        DISPLAY=:0 xprop -id "$RETROARCH_WINDOW" -f _NET_WM_WINDOW_TYPE 32a -set _NET_WM_WINDOW_TYPE "_NET_WM_WINDOW_TYPE_NORMAL" 2>>"$LOG_FILE" || true
        
        # Raise window to top of stacking order
        DISPLAY=:0 xdotool windowraise "$RETROARCH_WINDOW" 2>>"$LOG_FILE" || true
        sleep 0.2
        
        # Activate/focus the window so it receives input and is on top
        DISPLAY=:0 xdotool windowactivate "$RETROARCH_WINDOW" 2>>"$LOG_FILE" || true
        sleep 0.2
        
        # CRITICAL: Ensure window is actually visible and not hidden
        DISPLAY=:0 xdotool windowmap "$RETROARCH_WINDOW" 2>>"$LOG_FILE" || true
        # Remove any hidden/minimized states
        DISPLAY=:0 xprop -id "$RETROARCH_WINDOW" -f _NET_WM_STATE 32a -set _NET_WM_STATE "" 2>>"$LOG_FILE" || true
        sleep 0.2
        
        # Also try using wmctrl if available (more reliable for some window managers)
        if command -v wmctrl >/dev/null 2>&1; then
            DISPLAY=:0 wmctrl -i -a "$RETROARCH_WINDOW" 2>>"$LOG_FILE" || true
            sleep 0.2
        fi
        
        # Give RetroArch time to initialize display after focusing
        sleep 0.5
        
        # Clean up any interfering windows that might be on top
        echo "$(date): Cleaning up interfering windows and ensuring RetroArch is on top" >> "$LOG_FILE"
        # Kill MPV windows (already done earlier, but double-check)
        for mpv_win in $(DISPLAY=:0 xdotool search --class mpv 2>/dev/null); do
            DISPLAY=:0 xdotool windowkill "$mpv_win" 2>>"$LOG_FILE" || true
        done
        # Kill pygame windows (UI) - but be careful not to kill RetroArch window
        for pygame_win in $(DISPLAY=:0 xdotool search --class pygame 2>/dev/null); do
            if [ "$pygame_win" != "$RETROARCH_WINDOW" ]; then
                DISPLAY=:0 xdotool windowkill "$pygame_win" 2>>"$LOG_FILE" || true
            fi
        done
        
        # Kill any windows with "Magic Dingus Box" in the name (UI windows)
        for window in $(DISPLAY=:0 xdotool search --name "Magic Dingus Box" 2>/dev/null); do
            if [ "$window" != "$RETROARCH_WINDOW" ]; then
                DISPLAY=:0 xdotool windowkill "$window" 2>>"$LOG_FILE" || true
            fi
        done
        
        # CRITICAL: Lower or kill ALL other windows that might be covering RetroArch
        # Check all windows and lower/kill any that overlap with RetroArch's position
        echo "$(date): Checking for overlapping windows..." >> "$LOG_FILE"
        for other_win in $(DISPLAY=:0 xdotool search --all --name '.*' 2>/dev/null); do
            if [ "$other_win" != "$RETROARCH_WINDOW" ] && [ -n "$other_win" ]; then
                # Get window geometry
                OTHER_GEOM=$(DISPLAY=:0 xdotool getwindowgeometry "$other_win" 2>/dev/null | grep Geometry | awk '{print $2}' || echo "")
                if [ -n "$OTHER_GEOM" ]; then
                    # If window is 720x480 or similar size, it might be covering RetroArch
                    if echo "$OTHER_GEOM" | grep -qE "720x480|720x405|720x"; then
                        echo "$(date): Found potentially overlapping window $other_win (geometry: $OTHER_GEOM), lowering it" >> "$LOG_FILE"
                        # Lower the window instead of killing (safer)
                        DISPLAY=:0 xdotool windowlower "$other_win" 2>>"$LOG_FILE" || true
                        # If it's a small window (1x1), it's probably a decoration - kill it
                        if echo "$OTHER_GEOM" | grep -qE "1x1"; then
                            DISPLAY=:0 xdotool windowkill "$other_win" 2>>"$LOG_FILE" || true
                        fi
                    fi
                fi
            fi
        done
        
        sleep 0.5  # Give windows time to close/lower
        
        # CRITICAL: Raise and focus RetroArch window again after cleanup
        # This ensures it's definitely on top after killing other windows
        echo "$(date): Raising RetroArch window to top after cleanup" >> "$LOG_FILE"
        
        # Force window to be above all others using xprop
        DISPLAY=:0 xprop -id "$RETROARCH_WINDOW" -f _NET_WM_STATE 32a -set _NET_WM_STATE "_NET_WM_STATE_ABOVE _NET_WM_STATE_FULLSCREEN" 2>>"$LOG_FILE" || true
        
        # Use xdotool to raise and activate
        DISPLAY=:0 xdotool windowraise "$RETROARCH_WINDOW" 2>>"$LOG_FILE" || true
        DISPLAY=:0 xdotool windowactivate "$RETROARCH_WINDOW" 2>>"$LOG_FILE" || true
        
        # Also try setting it to always on top
        DISPLAY=:0 xdotool set_window --overrideredirect 1 "$RETROARCH_WINDOW" 2>>"$LOG_FILE" || true
        sleep 0.1
        DISPLAY=:0 xdotool set_window --overrideredirect 0 "$RETROARCH_WINDOW" 2>>"$LOG_FILE" || true
        
        if command -v wmctrl >/dev/null 2>&1; then
            DISPLAY=:0 wmctrl -i -a "$RETROARCH_WINDOW" 2>>"$LOG_FILE" || true
            DISPLAY=:0 wmctrl -i -r "$RETROARCH_WINDOW" -b add,above 2>>"$LOG_FILE" || true
        fi
        
        # Send a key event to ensure window gets focus (Alt+Tab to cycle focus)
        DISPLAY=:0 xdotool key alt+Tab 2>>"$LOG_FILE" || true
        sleep 0.2
        DISPLAY=:0 xdotool windowactivate "$RETROARCH_WINDOW" 2>>"$LOG_FILE" || true
        
        # Verify RetroArch window is still there and visible
        if DISPLAY=:0 xdotool search --class retroarch 2>/dev/null | grep -q "$RETROARCH_WINDOW"; then
            # Check if window is actually visible
            WINDOW_STATE=$(DISPLAY=:0 xdotool getwindowgeometry "$RETROARCH_WINDOW" 2>/dev/null || echo "")
            MAP_STATE=$(DISPLAY=:0 xdotool getwindowmapstate "$RETROARCH_WINDOW" 2>/dev/null || echo "unknown")
            echo "$(date): RetroArch window verified and raised - window state: $WINDOW_STATE, map state: $MAP_STATE" >> "$LOG_FILE"
            
            # CRITICAL: Start a background process to continuously keep window on top and fullscreen
            # This ensures the window stays visible and at correct size even if something tries to resize it
            WINDOW_MONITOR_PID=""
            (
                while ps -p $RETROARCH_PID >/dev/null 2>&1; do
                    # Every 2 seconds, ensure window is still on top and correct size
                    if DISPLAY=:0 xdotool search --class retroarch 2>/dev/null | grep -q "$RETROARCH_WINDOW"; then
                        # Check current window size and position
                        CURRENT_GEOM=$(DISPLAY=:0 xdotool getwindowgeometry "$RETROARCH_WINDOW" 2>/dev/null | grep Geometry | awk '{print $2}' || echo "")
                        CURRENT_POS=$(DISPLAY=:0 xdotool getwindowgeometry "$RETROARCH_WINDOW" 2>/dev/null | grep Position | awk '{print $2}' || echo "")
                        
                        # Force window to be 720x480 at 0,0 if it's not already
                        # Check if position is close to 0,0 (within 5 pixels) and size is correct
                        POS_X=$(echo "$CURRENT_POS" | cut -d',' -f1)
                        POS_Y=$(echo "$CURRENT_POS" | cut -d',' -f2)
                        if [ "$CURRENT_GEOM" != "720x480" ] || [ "${POS_X:-999}" -gt 5 ] || [ "${POS_Y:-999}" -gt 5 ]; then
                            DISPLAY=:0 xdotool windowmove "$RETROARCH_WINDOW" 0 0 2>/dev/null || true
                            DISPLAY=:0 xdotool windowsize "$RETROARCH_WINDOW" 720 480 2>/dev/null || true
                        fi
                        
                        # Aggressively keep window on top
                        DISPLAY=:0 xdotool windowraise "$RETROARCH_WINDOW" 2>/dev/null || true
                        DISPLAY=:0 xdotool windowactivate "$RETROARCH_WINDOW" 2>/dev/null || true
                        
                        # Lower any other windows that might have appeared and are covering RetroArch
                        for other_win in $(DISPLAY=:0 xdotool search --all --name '.*' 2>/dev/null); do
                            if [ "$other_win" != "$RETROARCH_WINDOW" ] && [ -n "$other_win" ]; then
                                OTHER_GEOM=$(DISPLAY=:0 xdotool getwindowgeometry "$other_win" 2>/dev/null | grep Geometry | awk '{print $2}' || echo "")
                                if [ -n "$OTHER_GEOM" ] && echo "$OTHER_GEOM" | grep -qE "720x480|720x405"; then
                                    DISPLAY=:0 xdotool windowlower "$other_win" 2>/dev/null || true
                                fi
                            fi
                        done
                    fi
                    sleep 2
                done
                echo "$(date): [WINDOW_MONITOR] Stopped monitoring window (RetroArch exited)" >> "$LOG_FILE"
            ) &
            WINDOW_MONITOR_PID=$!
            echo "$(date): Started background window monitor (PID: $WINDOW_MONITOR_PID) to keep RetroArch on top and fullscreen" >> "$LOG_FILE"
        else
            echo "$(date): WARNING: RetroArch window disappeared" >> "$LOG_FILE"
        fi
        break
    fi
    # Log progress every 5 seconds
    if [ $((i % 25)) -eq 0 ]; then
        echo "$(date): Still waiting for RetroArch window... (attempt $i/75, ~$((i * 2 / 10)).$((i * 2 % 10))s elapsed)" >> "$LOG_FILE"
    fi
done

if [ -z "$RETROARCH_WINDOW" ]; then
    echo "$(date): WARNING: RetroArch window not found after 15 seconds of waiting" >> "$LOG_FILE"
    echo "$(date): RetroArch process status:" >> "$LOG_FILE"
    ps -p $RETROARCH_PID >> "$LOG_FILE" 2>&1 || echo "  Process not found" >> "$LOG_FILE"
    echo "$(date): All windows on screen:" >> "$LOG_FILE"
    DISPLAY=:0 xdotool search --all --name '.*' 2>>"$LOG_FILE" | head -10 >> "$LOG_FILE" || true
fi

# Wait for RetroArch process to exit
echo "$(date): Waiting for RetroArch process (PID: $RETROARCH_PID) to exit..." >> "$LOG_FILE"
RETROARCH_EXIT=0

# CRITICAL: Use a polling loop instead of wait, because wait may fail if process
# is not a direct child (can happen with background processes)
# Poll every 0.5 seconds until RetroArch exits
# The background monitor will kill any UI processes that try to start during this wait
while ps -p $RETROARCH_PID >/dev/null 2>&1; do
    sleep 0.5
    # Log progress every 10 seconds
    if [ $(($(date +%s) % 10)) -eq 0 ]; then
        echo "$(date): Still waiting for RetroArch (PID: $RETROARCH_PID) to exit..." >> "$LOG_FILE"
        # Also check if monitor is still running
        if [ -n "$MONITOR_PID" ] && ! ps -p $MONITOR_PID >/dev/null 2>&1; then
            echo "$(date): WARNING: Background monitor (PID: $MONITOR_PID) stopped unexpectedly" >> "$LOG_FILE"
        fi
        # Also check if window monitor is still running
        if [ -n "$WINDOW_MONITOR_PID" ] && ! ps -p $WINDOW_MONITOR_PID >/dev/null 2>&1; then
            echo "$(date): WARNING: Window monitor (PID: $WINDOW_MONITOR_PID) stopped unexpectedly" >> "$LOG_FILE"
        fi
    fi
done

# Stop window monitor if it's still running
if [ -n "${WINDOW_MONITOR_PID:-}" ] && [ "$WINDOW_MONITOR_PID" != "" ]; then
    kill "$WINDOW_MONITOR_PID" 2>>"$LOG_FILE" || true
    echo "$(date): Stopped window monitor" >> "$LOG_FILE"
fi

# Stop the background monitor (it should have stopped already, but ensure it's gone)
if [ -n "$MONITOR_PID" ]; then
    kill $MONITOR_PID 2>>"$LOG_FILE" || true
    echo "$(date): Stopped lock file monitor" >> "$LOG_FILE"
fi

# CRITICAL: Remove lock file when RetroArch exits
# This signals that it's safe for the UI to start again
if [ -f "$RETROARCH_LOCK_FILE" ]; then
    rm -f "$RETROARCH_LOCK_FILE"
    echo "$(date): Removed RetroArch lock file - UI can now start" >> "$LOG_FILE"
fi

# Process has exited - get exit code if possible
if wait $RETROARCH_PID 2>/dev/null; then
    RETROARCH_EXIT=$?
    echo "$(date): RetroArch process exited with code: $RETROARCH_EXIT (via wait)" >> "$LOG_FILE"
else
    # Wait failed (process not a child), but we know it exited from ps check
    RETROARCH_EXIT=0
    echo "$(date): RetroArch process exited (detected via ps check, wait failed - not a child process)" >> "$LOG_FILE"
fi
echo "$(date): RetroArch command completed with exit code: $RETROARCH_EXIT" >> "$LOG_FILE"

# Clean up temp config
if [ -n "$TEMP_CONFIG" ] && [ -f "$TEMP_CONFIG" ]; then
    rm -f "$TEMP_CONFIG"
    echo "$(date): Cleaned up temp config" >> "$LOG_FILE"
fi

echo "$(date): RetroArch exited with code: $RETROARCH_EXIT" >> "$LOG_FILE"

# Release any controller grabs that RetroArch might have held
echo "$(date): Releasing controller devices" >> "$LOG_FILE"
sleep 1  # Give RetroArch time to fully release devices

# Touch the log file so UI can detect RetroArch just exited
touch "$LOG_FILE"

# Try to reset controller devices if they exist
# RetroArch may have grabbed devices exclusively, so we need to ensure they're released
for js_device in /dev/input/js*; do
    if [ -c "$js_device" ]; then
        # Use udevadm to trigger a re-scan of the device
        udevadm trigger --action=change --subsystem-match=input "$js_device" 2>>"$LOG_FILE" || true
        echo "$(date): Triggered udev re-scan for $js_device" >> "$LOG_FILE"
    fi
done

# Also reset event devices (evdev uses these)
for event_device in /dev/input/event*; do
    if [ -c "$event_device" ]; then
        # Check if it's a joystick device
        if udevadm info --query=property --name="$event_device" 2>/dev/null | grep -q "ID_INPUT_JOYSTICK=1"; then
            udevadm trigger --action=change --subsystem-match=input "$event_device" 2>>"$LOG_FILE" || true
            echo "$(date): Triggered udev re-scan for joystick event device $event_device" >> "$LOG_FILE"
        fi
    fi
done

# Additional delay to ensure controllers are fully released and ready
sleep 1

# Restart Magic Dingus Box UI service
# CRITICAL: Unmask BOTH user and system services before starting
echo "$(date): Unmasking services to allow restart" >> "$LOG_FILE"
systemctl --user unmask "$SERVICE_NAME" 2>>"$LOG_FILE" || true
sudo systemctl unmask "$SERVICE_NAME" 2>>"$LOG_FILE" || true
sudo systemctl unmask magic-mpv.service 2>>"$LOG_FILE" || true

# Start mpv service first (UI service Requires it)
echo "$(date): Starting mpv service" >> "$LOG_FILE"
sudo systemctl start magic-mpv.service 2>>"$LOG_FILE" || {
    echo "$(date): WARNING: Failed to start mpv service, but continuing" >> "$LOG_FILE"
}
sleep 1  # Give mpv time to start

# Try to start UI service (prefer system service since that's what was running)
echo "$(date): Restarting $SERVICE_NAME" >> "$LOG_FILE"
if sudo systemctl start "$SERVICE_NAME" 2>>"$LOG_FILE"; then
    echo "$(date): UI service restarted successfully (system service)" >> "$LOG_FILE"
elif systemctl --user start "$SERVICE_NAME" 2>>"$LOG_FILE"; then
    echo "$(date): UI service restarted successfully (user service)" >> "$LOG_FILE"
else
    echo "ERROR: Failed to restart $SERVICE_NAME" >> "$LOG_FILE"
    echo "ERROR: Failed to restart $SERVICE_NAME. Check logs: $LOG_FILE" >&2
    echo "Attempting to start service manually..." >> "$LOG_FILE"
    # Last resort: try to start the Python process directly
    cd /opt/magic_dingus_box && nohup /opt/magic_dingus_box/venv/bin/python -m magic_dingus_box.main > /tmp/magic-ui-manual.log 2>&1 &
    exit 1
fi

exit $RETROARCH_EXIT

