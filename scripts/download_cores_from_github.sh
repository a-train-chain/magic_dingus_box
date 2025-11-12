#!/bin/bash
# Download 64-bit RetroArch cores from GitHub releases
# Alternative to RetroPie repository when it's unavailable

set -e

CORES_DIR="${1:-$HOME/.config/retroarch/cores}"
ARCH="aarch64"

echo "=== Downloading 64-bit Cores from GitHub ==="
echo "Target directory: $CORES_DIR"
echo "Architecture: $ARCH"
echo ""

mkdir -p "$CORES_DIR"

# GitHub repositories and release patterns
# Format: REPO_URL|RELEASE_PATTERN|OUTPUT_NAME
declare -a CORE_SOURCES=(
    # Format: "repo_url|release_pattern|output_name"
    # Note: Most cores don't have pre-built releases, so this is a template
)

DOWNLOAD_TOOL=""
if command -v curl >/dev/null 2>&1; then
    DOWNLOAD_TOOL="curl"
elif command -v wget >/dev/null 2>&1; then
    DOWNLOAD_TOOL="wget"
else
    echo "Error: Neither curl nor wget found. Install one: sudo apt install curl"
    exit 1
fi

echo "Note: Most libretro cores don't have pre-built GitHub releases."
echo "You'll need to either:"
echo "1. Build from source (see MANUAL_CORE_DOWNLOAD_GUIDE.md)"
echo "2. Find pre-built cores from community sources"
echo "3. Wait for RetroPie repository to become available"
echo ""
echo "Alternative sources to check:"
echo "- SBC64 Gaming: https://www.sbc64gaming.com/"
echo "- RetroPie forums: https://retropie.org.uk/forum/"
echo "- Raspberry Pi forums: Community builds"
echo ""
echo "To manually transfer a core file:"
echo "1. Download .so file to your Mac"
echo "2. Transfer: scp core_libretro.so alexanderchaney@magicpi.local:~/.config/retroarch/cores/"
echo "3. Set permissions: ssh alexanderchaney@magicpi.local 'chmod +x ~/.config/retroarch/cores/core_libretro.so'"
echo "4. Verify: ssh alexanderchaney@magicpi.local 'file ~/.config/retroarch/cores/core_libretro.so'"
echo "   (Should show: ELF 64-bit LSB shared object, ARM aarch64)"

