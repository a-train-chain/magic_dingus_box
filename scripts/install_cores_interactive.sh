#!/bin/bash
# Interactive script to install RetroArch cores
# This will guide the user through installing cores via RetroArch UI

set -e

CORES_DIR="$HOME/.config/retroarch/cores"
mkdir -p "$CORES_DIR"

echo "=========================================="
echo "RetroArch Core Installation Guide"
echo "=========================================="
echo ""
echo "Required cores based on your playlists:"
echo "  1. FCEUmm (NES)"
echo "  2. ParaLLEl N64 (N64)"
echo "  3. PCSX ReARMed (PS1)"
echo ""
echo "To install these cores, you have two options:"
echo ""
echo "OPTION 1: Via RetroArch UI (Recommended)"
echo "----------------------------------------"
echo "1. On your Pi, run:"
echo "   DISPLAY=:0 retroarch"
echo ""
echo "2. In RetroArch menu:"
echo "   - Navigate to: Online Updater"
echo "   - Select: Core Downloader"
echo "   - Find and download:"
echo "     * Nintendo - Nintendo Entertainment System (FCEUmm)"
echo "     * Nintendo - Nintendo 64 (ParaLLEl N64)"
echo "     * Sony - PlayStation (PCSX ReARMed)"
echo ""
echo "3. Cores will be installed to: $CORES_DIR"
echo ""
echo "OPTION 2: Manual Download"
echo "-------------------------"
echo "Visit: https://buildbot.libretro.com/nightly/linux/aarch64/latest/"
echo "Download these .so files:"
echo "  - fceumm_libretro.so"
echo "  - parallel_n64_libretro.so"
echo "  - pcsx_rearmed_libretro.so"
echo ""
echo "Place them in: $CORES_DIR"
echo "Make them executable: chmod +x $CORES_DIR/*.so"
echo ""
echo "=========================================="
echo ""
echo "Checking current core status..."
echo ""

# Check which cores are installed
CORES=("fceumm_libretro" "parallel_n64_libretro" "pcsx_rearmed_libretro")
INSTALLED=0

for CORE in "${CORES[@]}"; do
    if [ -f "$CORES_DIR/${CORE}.so" ] || [ -f "/usr/lib/aarch64-linux-gnu/libretro/${CORE}.so" ]; then
        echo "✓ $CORE - INSTALLED"
        ((INSTALLED++))
    else
        echo "✗ $CORE - NOT INSTALLED"
    fi
done

echo ""
if [ $INSTALLED -eq 3 ]; then
    echo "✓ All required cores are installed!"
else
    echo "⚠ $INSTALLED/3 cores installed. Please install missing cores using Option 1 or 2 above."
fi

