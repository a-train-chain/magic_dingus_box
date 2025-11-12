#!/bin/bash
# Download Parallel N64 core for RetroArch
# This downloads the core directly from RetroArch's buildbot

set -e

CORES_DIR="$HOME/.config/retroarch/cores"
mkdir -p "$CORES_DIR"

ARCH="aarch64"  # Raspberry Pi 4B is ARM64
CORE_NAME="parallel_n64_libretro.so"

echo "Downloading Parallel N64 core for $ARCH..."

# RetroArch buildbot URL pattern
# Try different possible URLs
URLS=(
    "https://buildbot.libretro.com/nightly/linux/${ARCH}/latest/${CORE_NAME}.zip"
    "https://buildbot.libretro.com/stable/1.20.0/linux/${ARCH}/${CORE_NAME}.zip"
)

DOWNLOADED=false
for URL in "${URLS[@]}"; do
    echo "Trying: $URL"
    if curl -L -f -o "/tmp/${CORE_NAME}.zip" "$URL" 2>/dev/null; then
        echo "Downloaded core successfully"
        unzip -o "/tmp/${CORE_NAME}.zip" -d "$CORES_DIR" 2>/dev/null || {
            # Try unzipping manually
            cd /tmp
            unzip -o "${CORE_NAME}.zip" 2>/dev/null || true
            cp -f "${CORE_NAME}" "$CORES_DIR/" 2>/dev/null || true
        }
        rm -f "/tmp/${CORE_NAME}.zip"
        DOWNLOADED=true
        break
    fi
done

if [ "$DOWNLOADED" = true ]; then
    if [ -f "$CORES_DIR/$CORE_NAME" ]; then
        chmod +x "$CORES_DIR/$CORE_NAME"
        echo "✓ N64 core installed at: $CORES_DIR/$CORE_NAME"
    else
        echo "⚠ Core downloaded but not found in expected location"
        echo "Check: $CORES_DIR"
    fi
else
    echo "⚠ Could not download core automatically"
    echo ""
    echo "Please download manually:"
    echo "1. Visit: https://buildbot.libretro.com/nightly/linux/aarch64/latest/"
    echo "2. Download: parallel_n64_libretro.so.zip"
    echo "3. Extract and copy parallel_n64_libretro.so to: $CORES_DIR"
    echo ""
    echo "Or use RetroArch UI:"
    echo "1. Run: retroarch"
    echo "2. Go to: Online Updater -> Core Downloader"
    echo "3. Find: Nintendo - Nintendo 64 (Parallel N64)"
    echo "4. Download it"
fi

