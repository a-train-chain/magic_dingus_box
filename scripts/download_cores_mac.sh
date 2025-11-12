#!/bin/bash
# Download RetroArch cores on Mac for transfer to Raspberry Pi
# This script tries multiple architectures to find available cores

set -e

DOWNLOAD_DIR="$HOME/Downloads/retroarch_cores"
mkdir -p "$DOWNLOAD_DIR"

echo "Downloading RetroArch cores for Raspberry Pi..."
echo "Download directory: $DOWNLOAD_DIR"
echo ""

CORES=(
    "fceumm_libretro"
    "parallel_n64_libretro"
    "pcsx_rearmed_libretro"
)

# Try different architectures - cores are in .zip files
ARCHITECTURES=(
    "armv7-neon-hf"
    "armhf"
)

download_core() {
    local CORE_BASE="$1"
    local CORE_FILE="${CORE_BASE}.so"
    local CORE_ZIP="${CORE_FILE}.zip"
    
    echo "Downloading $CORE_FILE..."
    
    for ARCH in "${ARCHITECTURES[@]}"; do
        # Try .zip files first (they're what's actually available)
        URLS=(
            "https://buildbot.libretro.com/nightly/linux/${ARCH}/latest/${CORE_ZIP}"
            "https://buildbot.libretro.com/stable/1.20.0/linux/${ARCH}/${CORE_ZIP}"
            # Also try direct .so files as fallback
            "https://buildbot.libretro.com/nightly/linux/${ARCH}/latest/${CORE_FILE}"
            "https://buildbot.libretro.com/stable/1.20.0/linux/${ARCH}/${CORE_FILE}"
        )
        
        for URL in "${URLS[@]}"; do
            echo "  Trying: $URL"
            if curl -L -f --connect-timeout 30 --max-time 120 \
                "$URL" -o "$DOWNLOAD_DIR/${CORE_ZIP}" 2>/dev/null; then
                if [ -f "$DOWNLOAD_DIR/${CORE_ZIP}" ] && [ -s "$DOWNLOAD_DIR/${CORE_ZIP}" ]; then
                    FILE_SIZE=$(stat -f%z "$DOWNLOAD_DIR/${CORE_ZIP}" 2>/dev/null || stat -c%s "$DOWNLOAD_DIR/${CORE_ZIP}" 2>/dev/null)
                    if [ "$FILE_SIZE" -gt 1000 ]; then
                        echo "  ✓ Downloaded $CORE_ZIP ($FILE_SIZE bytes) from $ARCH"
                        # Extract the .so file from the zip
                        if unzip -o "$DOWNLOAD_DIR/${CORE_ZIP}" -d "$DOWNLOAD_DIR" 2>/dev/null; then
                            if [ -f "$DOWNLOAD_DIR/${CORE_FILE}" ]; then
                                rm -f "$DOWNLOAD_DIR/${CORE_ZIP}"
                                echo "  ✓ Extracted $CORE_FILE"
                                return 0
                            fi
                        fi
                        rm -f "$DOWNLOAD_DIR/${CORE_ZIP}"
                    else
                        rm -f "$DOWNLOAD_DIR/${CORE_ZIP}"
                    fi
                fi
            fi
        done
    done
    
    echo "  ✗ Failed to download $CORE_FILE from all sources"
    return 1
}

SUCCESS=0
for CORE in "${CORES[@]}"; do
    if download_core "$CORE"; then
        ((SUCCESS++))
    fi
    echo ""
done

echo "=========================================="
echo "Download Summary: $SUCCESS/${#CORES[@]} cores downloaded"
echo "=========================================="
echo ""
echo "Cores downloaded to: $DOWNLOAD_DIR"
echo ""
if [ $SUCCESS -gt 0 ]; then
    echo "To transfer to Raspberry Pi, run:"
    echo "  scp $DOWNLOAD_DIR/*.so alexanderchaney@magicpi.local:/tmp/"
    echo ""
    echo "Then on the Pi:"
    echo "  mkdir -p ~/.config/retroarch/cores"
    echo "  mv /tmp/*.so ~/.config/retroarch/cores/"
    echo "  chmod +x ~/.config/retroarch/cores/*.so"
fi

