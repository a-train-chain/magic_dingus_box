#!/bin/bash
# Verify RetroPie setup is correct for Magic Dingus Box
# Run this after installation or reboot to ensure everything is configured

set -e

echo "=== Verifying RetroPie Setup for Magic Dingus Box ==="
echo ""

# Check RetroArch executable
echo "1. Checking RetroArch executable..."
RETROARCH_FOUND=false
for path in "/opt/retropie/emulators/retroarch/bin/retroarch" "/usr/bin/retroarch"; do
    if [ -f "$path" ]; then
        echo "   ✓ Found RetroArch at: $path"
        RETROARCH_FOUND=true
        break
    fi
done

if [ "$RETROARCH_FOUND" = false ]; then
    echo "   ✗ RetroArch not found!"
    echo "   Install with: sudo scripts/install_retropie_cores.sh"
    exit 1
fi

# Check RetroPie cores
echo ""
echo "2. Checking RetroPie cores..."
CORE_DIRS=(
    "$HOME/.config/retroarch/cores"
    "/opt/retropie/libretrocores"
    "/usr/lib/aarch64-linux-gnu/libretro"
    "/usr/lib/arm-linux-gnueabihf/libretro"
)

CORE_FOUND=false
for dir in "${CORE_DIRS[@]}"; do
    if [ -d "$dir" ] && [ "$(ls -A "$dir"/*.so 2>/dev/null)" ]; then
        echo "   ✓ Found cores in: $dir"
        CORE_COUNT=$(ls -1 "$dir"/*.so 2>/dev/null | wc -l)
        echo "     ($CORE_COUNT cores found)"
        CORE_FOUND=true
        
        # Check for specific cores we need (updated for 64-bit compatibility)
        for core in "nestopia_libretro.so" "snes9x_libretro.so" "parallel_n64_libretro.so" "pcsx_rearmed_libretro.so"; do
            if [ -f "$dir/$core" ]; then
                # Verify it's 64-bit
                if file "$dir/$core" 2>/dev/null | grep -q "ELF 64-bit.*aarch64"; then
                    echo "     ✓ $core (64-bit)"
                elif file "$dir/$core" 2>/dev/null | grep -q "ELF 32-bit"; then
                    echo "     ⚠ $core (32-bit - won't work on 64-bit RetroArch)"
                else
                    echo "     ✓ $core"
                fi
            else
                echo "     ✗ $core (missing)"
            fi
        done
    fi
done

if [ "$CORE_FOUND" = false ]; then
    echo "   ✗ No cores found!"
    echo "   Install with: sudo scripts/install_retropie_cores.sh"
    exit 1
fi

# Check RetroArch config
echo ""
echo "3. Checking RetroArch configuration..."
USER_HOME="${HOME:-/home/alexanderchaney}"
CONFIG_FILE="$USER_HOME/.config/retroarch/retroarch.cfg"

if [ -f "$CONFIG_FILE" ]; then
    echo "   ✓ Config file exists: $CONFIG_FILE"
    
    if grep -q "libretro_directory" "$CONFIG_FILE"; then
        echo "   ✓ libretro_directory configured"
        grep "libretro_directory" "$CONFIG_FILE" | head -1
    else
        echo "   ✗ libretro_directory not configured"
        echo "   Run: sudo scripts/install_retropie_cores.sh"
    fi
else
    echo "   ✗ Config file not found: $CONFIG_FILE"
    echo "   Run: sudo scripts/install_retropie_cores.sh"
fi

# Check wrapper script
echo ""
echo "4. Checking wrapper script..."
WRAPPER_SCRIPT="/opt/magic_dingus_box/scripts/launch_retroarch.sh"
if [ -f "$WRAPPER_SCRIPT" ]; then
    if [ -x "$WRAPPER_SCRIPT" ]; then
        echo "   ✓ Wrapper script exists and is executable"
    else
        echo "   ✗ Wrapper script exists but not executable"
        echo "   Fix with: chmod +x $WRAPPER_SCRIPT"
    fi
else
    echo "   ✗ Wrapper script not found: $WRAPPER_SCRIPT"
fi

# Check lock file (should not exist on boot)
echo ""
echo "5. Checking for stale lock files..."
LOCK_FILE="/tmp/magic_retroarch_active.lock"
if [ -f "$LOCK_FILE" ]; then
    LOCK_CONTENT=$(cat "$LOCK_FILE" 2>/dev/null || echo "")
    if [ "$LOCK_CONTENT" = "starting" ]; then
        echo "   ⚠ Stale lock file found (placeholder 'starting')"
        echo "   Removing stale lock file..."
        rm -f "$LOCK_FILE"
        echo "   ✓ Cleaned up stale lock file"
    elif [ -n "$LOCK_CONTENT" ]; then
        # Check if process is still running
        if ps -p "$LOCK_CONTENT" >/dev/null 2>&1; then
            PROC_NAME=$(ps -p "$LOCK_CONTENT" -o comm= 2>/dev/null || echo "")
            if echo "$PROC_NAME" | grep -qi retroarch; then
                echo "   ✓ Lock file exists and RetroArch is running (PID: $LOCK_CONTENT)"
            else
                echo "   ⚠ Stale lock file found (process $LOCK_CONTENT is not RetroArch)"
                rm -f "$LOCK_FILE"
                echo "   ✓ Cleaned up stale lock file"
            fi
        else
            echo "   ⚠ Stale lock file found (process $LOCK_CONTENT not running)"
            rm -f "$LOCK_FILE"
            echo "   ✓ Cleaned up stale lock file"
        fi
    else
        echo "   ⚠ Empty lock file found"
        rm -f "$LOCK_FILE"
        echo "   ✓ Cleaned up empty lock file"
    fi
else
    echo "   ✓ No lock file (expected on boot)"
fi

echo ""
echo "=== Verification Complete ==="
echo ""
echo "If all checks passed, RetroPie is ready to use!"
echo "Launch a game from Magic Dingus Box to test."

