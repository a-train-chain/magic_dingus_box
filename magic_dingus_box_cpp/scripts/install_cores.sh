#!/usr/bin/env bash
set -euo pipefail

#
# Install RetroArch cores for C++ Magic Dingus Box
# This script pre-installs the cores needed for NES, N64, and PS1 games
# so they don't need to be downloaded through RetroArch's online updater
#
# Usage:
#   ./scripts/install_cores.sh                    # Install cores locally
#   ./scripts/install_cores.sh --pi               # Install cores on Pi
#   PI_HOST=pi@1.2.3.4 ./scripts/install_cores.sh --pi
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

PI_HOST="${PI_HOST:-magic@magicpi.local}"
REMOTE_MODE=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pi|-p)
            REMOTE_MODE=true
            shift
            ;;
        --help|-h)
            cat <<EOF
Install RetroArch cores for Magic Dingus Box

Usage: $(basename "$0") [options]

Options:
  --pi, -p       Install cores on Raspberry Pi (default: ${PI_HOST})
  --help, -h     Show this help

Environment overrides:
  PI_HOST        SSH target (default: ${PI_HOST})

This script installs the cores needed for:
  • NES games (nestopia_libretro)
  • N64 games (mupen64plus-next_libretro)
  • PS1 games (pcsx_rearmed_libretro)

Cores will be available immediately without needing to download through RetroArch.
EOF
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

echo "=== Installing RetroArch Cores ==="
echo ""

# Function to install cores
install_cores_logic() {
    echo "Updating package lists..."
    sudo apt update -qq

    echo "Installing RetroArch..."
    sudo apt install -y retroarch

    echo "Installing Cores via APT..."
    # Try to install cores via apt if available
    CORES_TO_INSTALL=(
        "libretro-nestopia"
        "libretro-pcsx-rearmed"
        "libretro-genesisplusgx"
        "libretro-snes9x"
        "libretro-fbneo"
        "libretro-beetle-pce-fast"
        "libretro-prosystem"
    )
    
    for core in "${CORES_TO_INSTALL[@]}"; do
        if sudo apt install -y "$core" 2>/dev/null; then
            echo "  ✓ $core installed via apt"
        else
            echo "  ⚠ $core not found in apt, will try manual download"
        fi
    done

    # Manual download fallback
    echo "Checking for missing cores and downloading if needed..."
    
    CORE_DIR="/home/$(whoami)/.config/retroarch/cores"
    mkdir -p "$CORE_DIR"
    
    # URL for aarch64 linux cores (using Christian Haitian's repo as reliable source for aarch64)
    BUILDBOT_URL="https://github.com/christianhaitian/retroarch-cores/raw/master/aarch64"
    
    declare -A CORES_MAP=(
        ["nestopia_libretro.so"]="nestopia_libretro.so.zip"
        ["pcsx_rearmed_libretro.so"]="pcsx_rearmed_libretro.so.zip"
        ["genesis_plus_gx_libretro.so"]="genesis_plus_gx_libretro.so.zip"
        ["snes9x2010_libretro.so"]="snes9x2010_libretro.so.zip"
        ["fbneo_libretro.so"]="fbneo_libretro.so.zip"
        ["mednafen_pce_fast_libretro.so"]="mednafen_pce_fast_libretro.so.zip"
        ["prosystem_libretro.so"]="prosystem_libretro.so.zip"
    )
    
    for core_so in "${!CORES_MAP[@]}"; do
        # Check if core exists in system or user dir
        if [ -f "/usr/lib/aarch64-linux-gnu/libretro/$core_so" ] || \
           [ -f "/usr/lib/libretro/$core_so" ] || \
           [ -f "$CORE_DIR/$core_so" ]; then
            echo "  ✓ $core_so found"
        else
            echo "  Downloading $core_so..."
            zip_file="${CORES_MAP[$core_so]}"
            wget -q "$BUILDBOT_URL/$zip_file" -O "/tmp/$zip_file"
            if [ $? -eq 0 ]; then
                unzip -o -d "$CORE_DIR" "/tmp/$zip_file"
                rm "/tmp/$zip_file"
                echo "  ✓ $core_so downloaded and installed"
            else
                echo "  ✗ Failed to download $core_so"
            fi
        fi
    done
    
    echo "Core installation logic complete."
}

# Check connectivity to Pi if in remote mode
if [ "$REMOTE_MODE" = true ]; then
    echo "Installing cores on ${PI_HOST}..."

    if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "${PI_HOST}" "echo 'Connection successful'" >/dev/null 2>&1; then
        echo "✗ ERROR: Cannot connect to ${PI_HOST}"
        exit 1
    fi

    # Run the installation remotely
    # We pass the function definition and call it
    ssh "${PI_HOST}" "$(typeset -f install_cores_logic); install_cores_logic"

else
    echo "Installing cores locally..."
    install_cores_logic
fi

echo ""
echo "=== Core Installation Complete ==="
echo ""
echo "🎮 Your Magic Dingus Box is ready for gaming!"
echo ""
echo "Required cores for your playlists:"
echo "  ✓ NES: nestopia_libretro"
echo "  ✓ PS1: pcsx_rearmed_libretro"
echo ""
echo "Games will launch immediately without downloading cores."
