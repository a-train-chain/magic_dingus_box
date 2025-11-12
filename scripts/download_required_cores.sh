#!/bin/bash
# Download required RetroArch cores based on playlists
# Downloads: fceumm (NES), parallel_n64 (N64), pcsx_rearmed (PS1)

set -e

ARCH="aarch64"  # Raspberry Pi 4B is ARM64
CORES_DIR="$HOME/.config/retroarch/cores"
SYSTEM_CORES_DIR="/usr/lib/aarch64-linux-gnu/libretro"

mkdir -p "$CORES_DIR"

# Cores to download
CORES=(
    "fceumm_libretro"
    "parallel_n64_libretro"
    "pcsx_rearmed_libretro"
)

echo "Downloading required RetroArch cores for $ARCH..."
echo "Target directory: $CORES_DIR"
echo ""

# Try multiple download methods
download_core() {
    local CORE_NAME="$1"
    local DOWNLOADED=false
    
    # Method 1: Try direct download from buildbot (nightly)
    echo "Attempting to download $CORE_NAME..."
    
    # Try nightly buildbot
    NIGHTLY_URL="https://buildbot.libretro.com/nightly/linux/${ARCH}/latest/${CORE_NAME}.so"
    if curl -L -f -o "/tmp/${CORE_NAME}.so" "$NIGHTLY_URL" 2>/dev/null; then
        echo "  ✓ Downloaded from nightly buildbot"
        cp "/tmp/${CORE_NAME}.so" "$CORES_DIR/"
        chmod +x "$CORES_DIR/${CORE_NAME}.so"
        rm -f "/tmp/${CORE_NAME}.so"
        DOWNLOADED=true
    else
        # Method 2: Try stable release
        STABLE_URL="https://buildbot.libretro.com/stable/1.20.0/linux/${ARCH}/${CORE_NAME}.so"
        if curl -L -f -o "/tmp/${CORE_NAME}.so" "$STABLE_URL" 2>/dev/null; then
            echo "  ✓ Downloaded from stable buildbot"
            cp "/tmp/${CORE_NAME}.so" "$CORES_DIR/"
            chmod +x "$CORES_DIR/${CORE_NAME}.so"
            rm -f "/tmp/${CORE_NAME}.so"
            DOWNLOADED=true
        fi
    fi
    
    if [ "$DOWNLOADED" = true ]; then
        if [ -f "$CORES_DIR/${CORE_NAME}.so" ]; then
            echo "  ✓ Installed: $CORES_DIR/${CORE_NAME}.so"
            return 0
        fi
    fi
    
    echo "  ✗ Failed to download $CORE_NAME"
    return 1
}

# Download each core
SUCCESS_COUNT=0
FAILED_CORES=()

for CORE in "${CORES[@]}"; do
    # Check if already installed
    if [ -f "$CORES_DIR/${CORE}.so" ] || [ -f "$SYSTEM_CORES_DIR/${CORE}.so" ]; then
        echo "✓ $CORE already installed"
        ((SUCCESS_COUNT++))
        continue
    fi
    
    if download_core "$CORE"; then
        ((SUCCESS_COUNT++))
    else
        FAILED_CORES+=("$CORE")
    fi
    echo ""
done

echo "=========================================="
echo "Download Summary:"
echo "  Successfully installed: $SUCCESS_COUNT/${#CORES[@]}"
if [ ${#FAILED_CORES[@]} -gt 0 ]; then
    echo "  Failed cores: ${FAILED_CORES[*]}"
    echo ""
    echo "For failed cores, install manually via RetroArch UI:"
    echo "  1. Run: DISPLAY=:0 retroarch"
    echo "  2. Go to: Online Updater -> Core Downloader"
    echo "  3. Download the missing cores"
fi

# Verify installations
echo ""
echo "Installed cores:"
for CORE in "${CORES[@]}"; do
    if [ -f "$CORES_DIR/${CORE}.so" ]; then
        echo "  ✓ $CORE (user directory)"
    elif [ -f "$SYSTEM_CORES_DIR/${CORE}.so" ]; then
        echo "  ✓ $CORE (system directory)"
    else
        echo "  ✗ $CORE (not found)"
    fi
done

