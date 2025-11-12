#!/bin/bash
# Download RetroArch cores for Raspberry Pi (using armhf architecture)
# Some cores may only be available as armhf but work on aarch64

set -e

ARCH="armhf"  # Try armhf as fallback
CORES_DIR="$HOME/.config/retroarch/cores"
mkdir -p "$CORES_DIR"

echo "Downloading RetroArch cores (armhf architecture)..."
echo "Note: These may work on aarch64 through compatibility"
echo ""

CORES=(
    "fceumm_libretro"
    "parallel_n64_libretro"
    "pcsx_rearmed_libretro"
)

download_core() {
    local CORE_BASE="$1"
    local CORE_FILE="${CORE_BASE}.so"
    
    echo "Downloading $CORE_FILE..."
    
    # Try armhf URLs
    URLS=(
        "https://buildbot.libretro.com/nightly/linux/${ARCH}/latest/${CORE_FILE}"
        "https://buildbot.libretro.com/stable/1.20.0/linux/${ARCH}/${CORE_FILE}"
    )
    
    for URL in "${URLS[@]}"; do
        echo "  Trying: $URL"
        if curl -L -f --connect-timeout 30 --max-time 120 \
            "$URL" -o "/tmp/${CORE_FILE}" 2>/dev/null; then
            if [ -f "/tmp/${CORE_FILE}" ] && [ -s "/tmp/${CORE_FILE}" ]; then
                FILE_SIZE=$(stat -f%z "/tmp/${CORE_FILE}" 2>/dev/null || stat -c%s "/tmp/${CORE_FILE}" 2>/dev/null)
                if [ "$FILE_SIZE" -gt 10000 ]; then
                    mv "/tmp/${CORE_FILE}" "$CORES_DIR/"
                    chmod +x "$CORES_DIR/$CORE_FILE"
                    echo "  ✓ Downloaded $CORE_FILE ($FILE_SIZE bytes)"
                    return 0
                else
                    rm -f "/tmp/${CORE_FILE}"
                fi
            fi
        fi
    done
    
    echo "  ✗ Failed to download $CORE_FILE"
    return 1
}

SUCCESS=0
for CORE in "${CORES[@]}"; do
    if [ -f "$CORES_DIR/${CORE}.so" ]; then
        echo "✓ ${CORE}.so already installed"
        ((SUCCESS++))
        continue
    fi
    
    if download_core "$CORE"; then
        ((SUCCESS++))
    fi
    echo ""
done

echo "Downloaded $SUCCESS/${#CORES[@]} cores"
if [ $SUCCESS -gt 0 ]; then
    echo ""
    echo "Cores installed in: $CORES_DIR"
    echo "Test them in RetroArch to see if they work on your system"
fi

