#!/bin/bash
# Build mupen64plus-next libretro core from source
# This core supports OpenGLES and should work on Raspberry Pi 4

set -e

CORES_DIR="${1:-$HOME/.config/retroarch/cores}"
BUILD_DIR="/tmp/mupen64plus-next-build"

echo "=== Building Mupen64Plus-Next Core ==="
echo "Target directory: $CORES_DIR"
echo "Build directory: $BUILD_DIR"
echo ""

# Check if running as root
if [ "$EUID" -eq 0 ]; then
    echo "Error: Don't run as root. Run as regular user."
    exit 1
fi

# Install build dependencies
echo "1. Installing build dependencies..."
sudo apt-get update -qq
sudo apt-get install -y \
    build-essential \
    git \
    cmake \
    libgl1-mesa-dev \
    libgles2-mesa-dev \
    libegl1-mesa-dev \
    libdrm-dev \
    libgbm-dev \
    libx11-dev \
    libxext-dev \
    libxrandr-dev \
    libsdl2-dev \
    zlib1g-dev \
    libpng-dev \
    || {
        echo "Failed to install dependencies"
        exit 1
    }

# Clean up old build directory
if [ -d "$BUILD_DIR" ]; then
    echo "2. Cleaning old build directory..."
    rm -rf "$BUILD_DIR"
fi

# Clone repository using libretro-super (recommended method)
echo "3. Cloning libretro-super repository..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Try libretro-super method first (most reliable)
if git clone --depth 1 https://github.com/libretro/libretro-super.git 2>/dev/null; then
    echo "   Using libretro-super build system..."
    cd libretro-super
    
    # Fetch mupen64plus-next
    ./libretro-fetch.sh mupen64plus_next || {
        echo "Failed to fetch mupen64plus-next"
        exit 1
    }
    
    # Build for aarch64 with OpenGLES
    echo "4. Building mupen64plus-next core..."
    echo "   This may take 15-45 minutes..."
    
    # Build with aarch64-gles platform
    ./libretro-build.sh mupen64plus_next platform=aarch64-gles || {
        echo "Build with aarch64-gles failed, trying aarch64..."
        ./libretro-build.sh mupen64plus_next platform=aarch64 || {
            echo "Build failed"
            exit 1
        }
    }
    
    # Find the built core
    CORE_FILE=$(find . -name "mupen64plus-next_libretro.so" -type f | head -1)
    
else
    # Fallback: Direct clone and build
    echo "   libretro-super failed, trying direct build..."
    git clone --depth 1 https://github.com/libretro/mupen64plus-libretro-nx.git
    cd mupen64plus-libretro-nx
    
    echo "4. Building mupen64plus-next core..."
    echo "   This may take 10-30 minutes..."
    
    # Build with platform=aarch64 and OpenGLES support
    make -j$(nproc) platform=aarch64-gles \
        || {
            echo "Build failed. Trying alternative build method..."
            # Try without platform flag, let it auto-detect
            make -j$(nproc) \
                || {
                    echo "Build failed with both methods"
                    exit 1
                }
        }
    
    CORE_FILE="mupen64plus-next_libretro.so"
fi

# Check if build succeeded
if [ -n "$CORE_FILE" ] && [ -f "$CORE_FILE" ]; then
    echo "5. Build successful! Installing core..."
    mkdir -p "$CORES_DIR"
    cp "$CORE_FILE" "$CORES_DIR/mupen64plus-next_libretro.so"
    chmod +x "$CORES_DIR/mupen64plus-next_libretro.so"
    
    # Verify it's 64-bit
    if file "$CORES_DIR/mupen64plus-next_libretro.so" | grep -q "ELF 64-bit.*aarch64"; then
        echo "   ✅ Core is 64-bit (aarch64)"
    else
        echo "   ⚠ Warning: Core may not be 64-bit"
        file "$CORES_DIR/mupen64plus-next_libretro.so"
    fi
    
    echo ""
    echo "=== Build Complete ==="
    echo "Core installed to: $CORES_DIR/mupen64plus-next_libretro.so"
    echo ""
    echo "Test with:"
    echo "  retroarch -L $CORES_DIR/mupen64plus-next_libretro.so /path/to/n64/rom.n64 --fullscreen"
else
    echo "ERROR: Build failed - mupen64plus-next_libretro.so not found"
    exit 1
fi

# Cleanup
echo ""
echo "Cleaning up build directory..."
cd /
rm -rf "$BUILD_DIR"

echo "Done!"

