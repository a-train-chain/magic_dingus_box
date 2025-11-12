#!/bin/bash
# Manually download RetroArch/libretro cores from buildbot
# This works when RetroPie repository is unavailable

set -e

CORES_DIR="${1:-$HOME/.config/retroarch/cores}"
ARCH="${2:-armv7-neon-hf}"  # Default to ARM 32-bit (most compatible)

echo "=== Manual Core Downloader ==="
echo "Target directory: $CORES_DIR"
echo "Architecture: $ARCH"
echo ""

mkdir -p "$CORES_DIR"

# Core URLs from libretro buildbot
# Format: https://buildbot.libretro.com/nightly/linux/ARCH/latest/CORE.zip
BUILDBOT_BASE="https://buildbot.libretro.com/nightly/linux/$ARCH/latest"

# Cores we want to download
CORES=(
    "fceumm_libretro"
    "mupen64plus_next_libretro"
    "pcsx_rearmed_libretro"
)

DOWNLOAD_TOOL=""
if command -v curl >/dev/null 2>&1; then
    DOWNLOAD_TOOL="curl"
elif command -v wget >/dev/null 2>&1; then
    DOWNLOAD_TOOL="wget"
else
    echo "Error: Neither curl nor wget found. Install one: sudo apt install curl"
    exit 1
fi

download_core() {
    local core_name="$1"
    # Buildbot uses .so.zip format
    local core_zip="${core_name}.so.zip"
    local core_so="${core_name}.so"
    local url="${BUILDBOT_BASE}/${core_zip}"
    local temp_zip="/tmp/${core_zip}"
    
    echo "Downloading $core_name..."
    
    if [ "$DOWNLOAD_TOOL" = "curl" ]; then
        if curl -L -f --connect-timeout 30 --max-time 120 "$url" -o "$temp_zip" 2>/dev/null; then
            if [ -f "$temp_zip" ] && [ -s "$temp_zip" ]; then
                echo "  ✓ Downloaded $core_zip"
                if unzip -o "$temp_zip" -d "$CORES_DIR" 2>/dev/null; then
                    if [ -f "$CORES_DIR/$core_so" ]; then
                        chmod +x "$CORES_DIR/$core_so"
                        echo "  ✓ Extracted and installed $core_so"
                        rm -f "$temp_zip"
                        return 0
                    fi
                fi
                rm -f "$temp_zip"
            fi
        fi
    elif [ "$DOWNLOAD_TOOL" = "wget" ]; then
        if wget --timeout=30 --tries=3 -q "$url" -O "$temp_zip" 2>/dev/null; then
            if [ -f "$temp_zip" ] && [ -s "$temp_zip" ]; then
                echo "  ✓ Downloaded $core_zip"
                if unzip -o "$temp_zip" -d "$CORES_DIR" 2>/dev/null; then
                    if [ -f "$CORES_DIR/$core_so" ]; then
                        chmod +x "$CORES_DIR/$core_so"
                        echo "  ✓ Extracted and installed $core_so"
                        rm -f "$temp_zip"
                        return 0
                    fi
                fi
                rm -f "$temp_zip"
            fi
        fi
    fi
    
    echo "  ✗ Failed to download $core_name"
    return 1
}

# Try multiple architectures
ARCHITECTURES=("armv7-neon-hf" "armhf" "aarch64")

SUCCESS_COUNT=0
for arch in "${ARCHITECTURES[@]}"; do
    echo ""
    echo "Trying architecture: $arch"
    BUILDBOT_BASE="https://buildbot.libretro.com/nightly/linux/$arch/latest"
    
    for core in "${CORES[@]}"; do
        # Skip if already installed
        core_so="${core}.so"
        if [ -f "$CORES_DIR/$core_so" ]; then
            echo "  $core_so already exists, skipping..."
            continue
        fi
        
        if download_core "$core"; then
            SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
        fi
    done
    
    # If we got all cores, stop trying other architectures
    if [ $SUCCESS_COUNT -eq ${#CORES[@]} ]; then
        break
    fi
done

echo ""
echo "=== Download Complete ==="
echo "Successfully downloaded: $SUCCESS_COUNT cores"
echo "Cores location: $CORES_DIR"
ls -lh "$CORES_DIR"/*.so 2>/dev/null | tail -5 || echo "No cores found"
