#!/bin/bash
# Install RetroArch cores using RetroArch's online updater
# This script launches RetroArch headless and uses its online updater

set -e

CORES_DIR="$HOME/.config/retroarch/cores"
mkdir -p "$CORES_DIR"

# Cores to install
CORES=(
    "Nintendo - Nintendo Entertainment System (FCEUmm)"
    "Nintendo - Nintendo 64 (Parallel N64)"
    "Sony - PlayStation (PCSX ReARMed)"
)

echo "Installing RetroArch cores via online updater..."
echo "This will launch RetroArch in the background to download cores"
echo ""

# Check if RetroArch is installed
if ! command -v retroarch >/dev/null 2>&1; then
    echo "ERROR: RetroArch not found"
    exit 1
fi

# Method: Use RetroArch's online updater via command line
# RetroArch can download cores if we use the right approach

echo "Attempting to download cores..."
echo ""
echo "NOTE: RetroArch cores must be downloaded via the RetroArch UI."
echo "Please run the following command on the Pi to download cores:"
echo ""
echo "  DISPLAY=:0 retroarch"
echo ""
echo "Then in RetroArch:"
echo "  1. Go to: Online Updater -> Core Downloader"
echo "  2. Download these cores:"
for CORE in "${CORES[@]}"; do
    echo "     - $CORE"
done
echo ""
echo "Alternatively, cores can be downloaded manually from:"
echo "  https://buildbot.libretro.com/nightly/linux/aarch64/latest/"
echo ""
echo "Download these files and place them in: $CORES_DIR"
echo "  - fceumm_libretro.so"
echo "  - parallel_n64_libretro.so"
echo "  - pcsx_rearmed_libretro.so"

# Try to download using wget with different URL patterns
echo ""
echo "Attempting automatic download..."

download_with_wget() {
    local CORE_NAME="$1"
    local URL="https://buildbot.libretro.com/nightly/linux/aarch64/latest/${CORE_NAME}.so"
    
    if wget -q --spider "$URL" 2>/dev/null; then
        wget -q "$URL" -O "$CORES_DIR/${CORE_NAME}.so" && {
            chmod +x "$CORES_DIR/${CORE_NAME}.so"
            echo "  ✓ Downloaded $CORE_NAME"
            return 0
        }
    fi
    return 1
}

# Try downloading cores
SUCCESS=0
for CORE_BASE in "fceumm_libretro" "parallel_n64_libretro" "pcsx_rearmed_libretro"; do
    if [ -f "$CORES_DIR/${CORE_BASE}.so" ]; then
        echo "  ✓ $CORE_BASE already installed"
        ((SUCCESS++))
    elif download_with_wget "$CORE_BASE"; then
        ((SUCCESS++))
    else
        echo "  ✗ Failed to download $CORE_BASE"
    fi
done

echo ""
if [ $SUCCESS -eq 3 ]; then
    echo "✓ All cores installed successfully!"
else
    echo "⚠ Some cores failed to download automatically"
    echo "Please install them manually via RetroArch UI"
fi

