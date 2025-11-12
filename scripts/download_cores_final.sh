#!/bin/bash
# Final attempt to download RetroArch cores
# Uses RetroArch's online updater programmatically

set -e

CORES_DIR="$HOME/.config/retroarch/cores"
mkdir -p "$CORES_DIR"

echo "Downloading RetroArch cores..."
echo ""

# Cores needed
CORES=("fceumm_libretro" "parallel_n64_libretro" "pcsx_rearmed_libretro")

# Try using RetroArch's online updater via its internal API
# RetroArch stores core download URLs in its core info files

download_via_retroarch() {
    local CORE_BASE="$1"
    local CORE_FILE="${CORE_BASE}.so"
    
    echo "Downloading $CORE_FILE..."
    
    # Method 1: Try RetroArch's buildbot with correct architecture
    # For aarch64, cores might be in different locations
    ARCH_VARIANTS=("aarch64" "arm64" "armv7-neon-hf" "armv8")
    
    for ARCH in "${ARCH_VARIANTS[@]}"; do
        URLS=(
            "https://buildbot.libretro.com/nightly/linux/${ARCH}/latest/${CORE_FILE}"
            "https://buildbot.libretro.com/stable/1.20.0/linux/${ARCH}/${CORE_FILE}"
        )
        
        for URL in "${URLS[@]}"; do
            if curl -L -f -s -o "$CORES_DIR/$CORE_FILE" "$URL" 2>/dev/null; then
                if [ -s "$CORES_DIR/$CORE_FILE" ] && [ $(stat -f%z "$CORES_DIR/$CORE_FILE" 2>/dev/null || stat -c%s "$CORES_DIR/$CORE_FILE" 2>/dev/null) -gt 10000 ]; then
                    chmod +x "$CORES_DIR/$CORE_FILE"
                    echo "  ✓ Downloaded from $URL"
                    return 0
                fi
                rm -f "$CORES_DIR/$CORE_FILE"
            fi
        done
    done
    
    return 1
}

# Download cores
SUCCESS=0
for CORE_BASE in "${CORES[@]}"; do
    if [ -f "$CORES_DIR/${CORE_BASE}.so" ]; then
        echo "✓ ${CORE_BASE}.so already installed"
        ((SUCCESS++))
        continue
    fi
    
    if download_via_retroarch "$CORE_BASE"; then
        ((SUCCESS++))
    else
        echo "  ✗ Failed to download ${CORE_BASE}.so"
    fi
    echo ""
done

if [ $SUCCESS -eq 3 ]; then
    echo "✓ All cores downloaded successfully!"
    echo ""
    echo "Installed cores:"
    ls -lh "$CORES_DIR"/*.so 2>/dev/null | awk '{print "  " $9 " (" $5 ")"}'
else
    echo "⚠ Only $SUCCESS/3 cores downloaded"
    echo ""
    echo "Please install remaining cores via RetroArch UI:"
    echo "  DISPLAY=:0 retroarch"
    echo "  Then: Online Updater -> Core Downloader"
fi

