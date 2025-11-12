#!/bin/bash
# Download RetroArch cores using RetroArch's online updater
# This script uses RetroArch's menu system to download cores programmatically

set -e

CORES_DIR="$HOME/.config/retroarch/cores"
mkdir -p "$CORES_DIR"

echo "Downloading RetroArch cores..."
echo ""

# Cores to download (as they appear in RetroArch's Core Downloader)
CORES=(
    "Nintendo - Nintendo Entertainment System (FCEUmm)"
    "Nintendo - Nintendo 64 (ParaLLEl N64)"
    "Sony - PlayStation (PCSX ReARMed)"
)

# Check if RetroArch is installed
if ! command -v retroarch >/dev/null 2>&1; then
    echo "ERROR: RetroArch not found"
    exit 1
fi

# Method: Use RetroArch's online updater via command line
# RetroArch supports downloading cores via its menu system
# We can use RetroArch's command-line interface if available

echo "Attempting to download cores via RetroArch online updater..."
echo ""

# Try using RetroArch's online updater command
# RetroArch might support downloading cores via command line
# Check RetroArch help for online updater options

# Actually, RetroArch doesn't have a direct command-line way to download cores
# We need to use the UI or download manually

# Let me try downloading from alternative sources
echo "Trying alternative download methods..."

# Try GitHub releases or other sources
download_core_alternative() {
    local CORE_BASE="$1"
    local CORE_FILE="${CORE_BASE}_libretro.so"
    
    # Try different sources
    # Source 1: Try RetroArch's buildbot with different path
    URLS=(
        "https://buildbot.libretro.com/nightly/linux/arm64/latest/${CORE_FILE}"
        "https://buildbot.libretro.com/nightly/linux/armv7-neon-hf/latest/${CORE_FILE}"
        "https://github.com/libretro/${CORE_BASE}/releases/latest/download/${CORE_FILE}"
    )
    
    for URL in "${URLS[@]}"; do
        echo "  Trying: $URL"
        if curl -L -f -o "$CORES_DIR/$CORE_FILE" "$URL" 2>/dev/null; then
            if [ -s "$CORES_DIR/$CORE_FILE" ]; then
                chmod +x "$CORES_DIR/$CORE_FILE"
                echo "  ✓ Downloaded $CORE_FILE"
                return 0
            fi
            rm -f "$CORES_DIR/$CORE_FILE"
        fi
    done
    
    return 1
}

# Download cores
SUCCESS=0
for CORE_BASE in "fceumm" "parallel_n64" "pcsx_rearmed"; do
    CORE_FILE="${CORE_BASE}_libretro.so"
    
    if [ -f "$CORES_DIR/$CORE_FILE" ]; then
        echo "✓ $CORE_FILE already installed"
        ((SUCCESS++))
        continue
    fi
    
    echo "Downloading $CORE_FILE..."
    if download_core_alternative "$CORE_BASE"; then
        ((SUCCESS++))
    else
        echo "  ✗ Failed to download $CORE_FILE"
    fi
    echo ""
done

if [ $SUCCESS -lt 3 ]; then
    echo "=========================================="
    echo "Some cores could not be downloaded automatically."
    echo ""
    echo "Please install them manually via RetroArch UI:"
    echo ""
    echo "1. SSH to the Pi and run:"
    echo "   DISPLAY=:0 retroarch"
    echo ""
    echo "2. In RetroArch menu:"
    echo "   - Go to: Online Updater"
    echo "   - Select: Core Downloader"
    echo "   - Download these cores:"
    for CORE in "${CORES[@]}"; do
        echo "     * $CORE"
    done
    echo ""
    echo "3. Cores will be installed to: $CORES_DIR"
    echo ""
    echo "Or download manually from:"
    echo "   https://buildbot.libretro.com/nightly/linux/aarch64/latest/"
    echo "   (Look for .so files and download them)"
fi

echo ""
echo "Installed cores:"
ls -lh "$CORES_DIR"/*.so 2>/dev/null | awk '{print "  " $9 " (" $5 ")"}' || echo "  No cores found"

