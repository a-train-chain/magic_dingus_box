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
    
    # Resolve the REAL target user. This script runs under sudo, so $(whoami)
    # is "root" and the old "/home/$(whoami)" expanded to /home/root — a path
    # that does not exist on Debian (root's home is /root). Cores therefore
    # landed somewhere NOTHING reads, while the script still printed "✓
    # installed". Observed live: mupen64plus_next, parallel_n64 and flycast
    # all downloaded successfully into /home/root/.config/retroarch/cores and
    # were invisible to both the kiosk and verify_box.sh.
    TARGET_USER="${SUDO_USER:-$(id -un)}"
    TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
    [ -n "$TARGET_HOME" ] || TARGET_HOME="$HOME"

    # Cores must land in BOTH places, because two different consumers look in
    # two different directories:
    #   - the kiosk at RUNTIME resolves config::retroarch::get_cores_dir(),
    #     i.e. $HOME/.config/retroarch/cores for the user running the binary
    #   - verify_box.sh dlopen-checks <app>/libretro_cores/*.so
    # The seven original cores are present in both; anything installed to only
    # one of them either fails to launch or fails the acceptance test.
    CORE_DIR="${TARGET_HOME}/.config/retroarch/cores"
    APP_CORE_DIR="${SCRIPT_DIR}/../libretro_cores"
    mkdir -p "$CORE_DIR" "$APP_CORE_DIR"
    
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
        # N64 + Dreamcast. The kiosk matches core names by SUBSTRING
        # (is_n64_core() looks for "mupen64plus" / "parallel_n64",
        # renderer_for_core() for "flycast"), so these exact filenames are
        # what controller_mapping.cpp and launch_contract.cpp expect.
        # mupen64plus_next is the primary N64 core; parallel_n64 is the
        # backup and gets the identical core-option contract.
        # NOTE: only christianhaitian's aarch64 repo carries all three —
        # the libretro buildbot 404s on mupen64plus_next for aarch64.
        ["mupen64plus_next_libretro.so"]="mupen64plus_next_libretro.so.zip"
        ["parallel_n64_libretro.so"]="parallel_n64_libretro.so.zip"
        ["flycast_libretro.so"]="flycast_libretro.so.zip"
    )
    
    # Mirror a core into the app dir and hand both copies to the target user.
    # Running under sudo means every file we create is root-owned by default,
    # which would leave the kiosk unable to read its own cores.
    place_core() {
        local so="$1"
        [ -f "$CORE_DIR/$so" ] && cp -f "$CORE_DIR/$so" "$APP_CORE_DIR/$so"
        chown "$TARGET_USER" "$CORE_DIR/$so" "$APP_CORE_DIR/$so" 2>/dev/null || true
        chmod 755 "$CORE_DIR/$so" "$APP_CORE_DIR/$so" 2>/dev/null || true
    }

    for core_so in "${!CORES_MAP[@]}"; do
        # A core only counts as present if BOTH consumers can see it — the
        # runtime path and the app dir. Checking just one let a half-installed
        # core report "✓ found" forever.
        if { [ -f "/usr/lib/aarch64-linux-gnu/libretro/$core_so" ] || \
             [ -f "/usr/lib/libretro/$core_so" ] || \
             [ -f "$CORE_DIR/$core_so" ]; } && [ -f "$APP_CORE_DIR/$core_so" ]; then
            echo "  ✓ $core_so found"
        elif [ -f "$CORE_DIR/$core_so" ]; then
            # Downloaded previously but never mirrored — repair in place.
            place_core "$core_so"
            echo "  ✓ $core_so mirrored to app dir"
        else
            echo "  Downloading $core_so..."
            zip_file="${CORES_MAP[$core_so]}"
            wget -q "$BUILDBOT_URL/$zip_file" -O "/tmp/$zip_file"
            if [ $? -eq 0 ]; then
                unzip -o -d "$CORE_DIR" "/tmp/$zip_file"
                place_core "$core_so"
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
