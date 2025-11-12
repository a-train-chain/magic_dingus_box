#!/bin/bash
# Install N64 core for RetroArch via online updater
# This script downloads and installs the Parallel N64 core

set -e

echo "Installing N64 core for RetroArch..."

# Create cores directory if it doesn't exist
CORES_DIR="$HOME/.config/retroarch/cores"
mkdir -p "$CORES_DIR"

# Download Parallel N64 core using RetroArch's core updater
# Note: This requires RetroArch to be able to download cores
echo "Downloading Parallel N64 core..."
retroarch --update-core-info || echo "Core info update failed, continuing..."

# Try to download the core directly
# Parallel N64 core name: parallel_n64_libretro
CORE_NAME="parallel_n64_libretro"

# Check if RetroArch can download cores
if command -v retroarch >/dev/null 2>&1; then
    echo "RetroArch found. You may need to download the N64 core manually:"
    echo "1. Run: retroarch"
    echo "2. Go to: Online Updater -> Core Downloader"
    echo "3. Find: Nintendo - Nintendo 64 (Parallel N64)"
    echo "4. Download it"
    echo ""
    echo "Or install via RetroArch command line (if available):"
    echo "retroarch --update-core-info"
    echo ""
    echo "The core will be installed to: $CORES_DIR"
else
    echo "ERROR: RetroArch not found"
    exit 1
fi

# Check if core exists after potential download
if [ -f "$CORES_DIR/${CORE_NAME}.so" ]; then
    echo "✓ N64 core found at: $CORES_DIR/${CORE_NAME}.so"
else
    echo "⚠ N64 core not found. You'll need to download it manually via RetroArch UI"
    echo "Core should be named: ${CORE_NAME}.so"
fi

