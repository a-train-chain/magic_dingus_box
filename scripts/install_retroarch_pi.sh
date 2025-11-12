#!/bin/bash
# Install RetroArch and libretro cores on Raspberry Pi 4B
# Optimized for 2GB RAM systems

set -e

echo "Installing RetroArch and libretro cores for Raspberry Pi 4B..."

# Update package lists
echo "[1/5] Updating package lists..."
sudo apt update

# Install RetroArch
echo "[2/5] Installing RetroArch..."
sudo apt install -y retroarch

# Install essential libretro cores (prioritize performance on 2GB RAM)
echo "[3/5] Installing essential libretro cores..."
sudo apt install -y \
    libretro-fceumm \
    libretro-snes9x \
    libretro-mupen64plus-next \
    libretro-pcsx-rearmed

# Install optional lightweight cores (if space allows)
echo "[4/5] Installing optional lightweight cores..."
sudo apt install -y \
    libretro-gambatte \
    libretro-genesis-plus-gx || echo "Optional cores installation failed, continuing..."

# Verify installation
echo "[5/5] Verifying installation..."
if [ -f "/usr/bin/retroarch" ]; then
    echo "✓ RetroArch executable found at /usr/bin/retroarch"
    retroarch --version || echo "Warning: Could not get RetroArch version"
else
    echo "ERROR: RetroArch executable not found!"
    exit 1
fi

# Check for installed cores
CORE_DIR="/usr/lib/libretro"
if [ -d "$CORE_DIR" ]; then
    echo "✓ Core directory found at $CORE_DIR"
    CORE_COUNT=$(find "$CORE_DIR" -name "*.so" | wc -l)
    echo "  Found $CORE_COUNT core(s)"
    echo "  Installed cores:"
    find "$CORE_DIR" -name "*.so" -exec basename {} \; | sed 's/^/    - /'
else
    echo "WARNING: Core directory not found at $CORE_DIR"
fi

# Create default RetroArch config directory
CONFIG_DIR="$HOME/.config/retroarch"
mkdir -p "$CONFIG_DIR"
echo "✓ Created config directory: $CONFIG_DIR"

echo ""
echo "RetroArch installation complete!"
echo "Next step: Run scripts/configure_retroarch_pi.sh to optimize settings for Pi 4B 2GB RAM"

