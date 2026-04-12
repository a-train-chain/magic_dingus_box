#!/usr/bin/env bash
set -euo pipefail

#
# Magic Dingus Box - Create Golden Image
#
# Creates a compressed SD card image from a Pi's SD card inserted into a Mac.
# Run this AFTER prepare_golden_image.sh has been executed on the Pi and the
# Pi has been shut down.
#
# Usage: ./scripts/golden_image/create_image.sh [/dev/diskN] [--shrink] [--help]
#
# Must be run on macOS.
#

# ---------------------------------------------------------------------------
# Colors
# ---------------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
VERSION_FILE="${PROJECT_ROOT}/VERSION"

# ---------------------------------------------------------------------------
# Usage
# ---------------------------------------------------------------------------
usage() {
    echo ""
    echo -e "${BOLD}Usage:${NC} $(basename "$0") [/dev/diskN] [--shrink] [--help]"
    echo ""
    echo -e "${BOLD}Options:${NC}"
    echo "  /dev/diskN    SD card device path (auto-detected if omitted)"
    echo "  --shrink      Shrink image with PiShrink via Docker before compressing"
    echo "  --help        Show this help message"
    echo ""
    echo -e "${BOLD}Description:${NC}"
    echo "  Reads a Raspberry Pi SD card and creates a compressed golden image"
    echo "  (.img.gz) that can be flashed onto new SD cards."
    echo ""
    echo -e "${BOLD}Prerequisites:${NC}"
    echo "  1. Run prepare_golden_image.sh on the Pi"
    echo "  2. Shut down the Pi (sudo shutdown -h now)"
    echo "  3. Remove the SD card and insert it into your Mac"
    echo ""
    echo -e "${BOLD}Examples:${NC}"
    echo "  $(basename "$0")                   Auto-detect SD card"
    echo "  $(basename "$0") /dev/disk4        Use specific device"
    echo "  $(basename "$0") --shrink          Auto-detect + shrink with PiShrink"
    echo ""
}

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
DEVICE=""
SHRINK=false

for arg in "$@"; do
    case "$arg" in
        --help|-h)
            usage
            exit 0
            ;;
        --shrink)
            SHRINK=true
            ;;
        /dev/disk*)
            DEVICE="$arg"
            ;;
        *)
            echo -e "${RED}Error: Unknown argument: ${arg}${NC}"
            usage
            exit 1
            ;;
    esac
done

# ---------------------------------------------------------------------------
# Preflight: macOS check
# ---------------------------------------------------------------------------
if [[ "$(uname)" != "Darwin" ]]; then
    echo -e "${RED}Error: This script must be run on macOS.${NC}"
    echo -e "${DIM}Detected OS: $(uname)${NC}"
    exit 1
fi

# ---------------------------------------------------------------------------
# Read version
# ---------------------------------------------------------------------------
if [[ ! -f "$VERSION_FILE" ]]; then
    echo -e "${RED}Error: VERSION file not found at ${VERSION_FILE}${NC}"
    exit 1
fi
VERSION=$(tr -d '[:space:]' < "$VERSION_FILE")

DATE=$(date +%Y%m%d)
IMAGE_NAME="magic_dingus_box_golden_v${VERSION}_${DATE}.img"
OUTPUT_FILE="${PROJECT_ROOT}/${IMAGE_NAME}"
COMPRESSED_FILE="${OUTPUT_FILE}.gz"

# ---------------------------------------------------------------------------
# Header
# ---------------------------------------------------------------------------
echo ""
echo -e "${BOLD}${CYAN}============================================${NC}"
echo -e "${BOLD}${CYAN}  Magic Dingus Box - Create Golden Image${NC}"
echo -e "${BOLD}${CYAN}============================================${NC}"
echo ""
echo -e "  ${BOLD}Version:${NC}  ${VERSION}"
echo -e "  ${BOLD}Date:${NC}     ${DATE}"
echo ""

# ---------------------------------------------------------------------------
# Auto-detect or validate device
# ---------------------------------------------------------------------------
if [[ -z "$DEVICE" ]]; then
    echo -e "${CYAN}Detecting external disks...${NC}"
    echo ""

    diskutil list external physical 2>/dev/null || true

    echo ""
    echo -e "${YELLOW}Enter the SD card device path (e.g., /dev/disk4):${NC}"
    read -r -p "> " DEVICE

    if [[ -z "$DEVICE" ]]; then
        echo -e "${RED}Error: No device specified. Aborting.${NC}"
        exit 1
    fi
fi

# Validate device exists
if ! diskutil info "$DEVICE" &>/dev/null; then
    echo -e "${RED}Error: Device ${DEVICE} not found.${NC}"
    echo -e "${DIM}Run 'diskutil list external physical' to see available devices.${NC}"
    exit 1
fi

# ---------------------------------------------------------------------------
# Show disk info for confirmation
# ---------------------------------------------------------------------------
echo ""
echo -e "${BOLD}Selected device: ${CYAN}${DEVICE}${NC}"
echo ""
diskutil info "$DEVICE" | grep -E '(Device Identifier|Device Node|Media Name|Total Size|Disk Size|Protocol|Media Type)' | while IFS= read -r line; do
    echo -e "  ${DIM}${line}${NC}"
done
echo ""

# Compute raw device for faster I/O (/dev/diskN -> /dev/rdiskN)
RAW_DEVICE="${DEVICE/\/dev\/disk//dev/rdisk}"

# Output info
echo -e "  ${BOLD}Output:${NC}   ${IMAGE_NAME}.gz"
echo -e "  ${BOLD}Raw I/O:${NC}  ${RAW_DEVICE}"
if [[ "$SHRINK" == true ]]; then
    echo -e "  ${BOLD}Shrink:${NC}   Yes (PiShrink via Docker)"
fi
echo ""

# ---------------------------------------------------------------------------
# Confirmation
# ---------------------------------------------------------------------------
echo -e "${YELLOW}${BOLD}WARNING: This will read the entire SD card. Ensure no other operations${NC}"
echo -e "${YELLOW}${BOLD}are accessing the card.${NC}"
echo ""
read -r -p "$(echo -e "${BOLD}Proceed with imaging ${DEVICE}? [y/N] ${NC}")" confirm
if [[ "${confirm}" != "y" && "${confirm}" != "Y" ]]; then
    echo -e "${RED}Aborted.${NC}"
    exit 1
fi

echo ""

# ---------------------------------------------------------------------------
# Step 1/3: Unmount partitions
# ---------------------------------------------------------------------------
echo -e "${CYAN}[1/3] Unmounting partitions on ${DEVICE}...${NC}"

if diskutil unmountDisk "$DEVICE"; then
    echo -e "  ${GREEN}Done${NC} - all partitions unmounted"
else
    echo -e "${RED}Error: Failed to unmount ${DEVICE}.${NC}"
    echo -e "${DIM}Ensure no applications are using files on the SD card.${NC}"
    exit 1
fi

echo ""

# ---------------------------------------------------------------------------
# Step 2/3: Read SD card
# ---------------------------------------------------------------------------
echo -e "${CYAN}[2/3] Reading SD card (this will take a while)...${NC}"
echo -e "  ${DIM}Source: ${RAW_DEVICE}${NC}"
echo -e "  ${DIM}Dest:   ${OUTPUT_FILE}${NC}"
echo ""

# macOS BSD dd does not support status=progress; use Ctrl+T for progress
echo -e "  ${DIM}Tip: Press Ctrl+T at any time to see progress${NC}"
sudo dd if="$RAW_DEVICE" of="$OUTPUT_FILE" bs=4m

echo ""
echo -e "  ${GREEN}Done${NC} - raw image created"

# Show raw image size
RAW_SIZE=$(ls -lh "$OUTPUT_FILE" | awk '{print $5}')
echo -e "  ${DIM}Raw image size: ${RAW_SIZE}${NC}"
echo ""

# ---------------------------------------------------------------------------
# Step 2.5/3: PiShrink (optional)
# ---------------------------------------------------------------------------
if [[ "$SHRINK" == true ]]; then
    echo -e "${CYAN}[2.5/3] Shrinking image with PiShrink...${NC}"

    if command -v docker &>/dev/null; then
        OUTPUT_DIR=$(dirname "$OUTPUT_FILE")
        docker run --rm --privileged \
            -v "${OUTPUT_DIR}":/workdir \
            mgomesborges/pishrink \
            pishrink.sh "/workdir/${IMAGE_NAME}"

        echo -e "  ${GREEN}Done${NC} - image shrunk"

        # Show shrunk size
        SHRUNK_SIZE=$(ls -lh "$OUTPUT_FILE" | awk '{print $5}')
        echo -e "  ${DIM}Shrunk image size: ${SHRUNK_SIZE}${NC}"
    else
        echo -e "  ${YELLOW}Warning: Docker not found, skipping PiShrink.${NC}"
        echo -e "  ${DIM}Install Docker Desktop for macOS to enable image shrinking.${NC}"
    fi

    echo ""
fi

# ---------------------------------------------------------------------------
# Step 3/3: Compress
# ---------------------------------------------------------------------------
echo -e "${CYAN}[3/3] Compressing image with gzip...${NC}"
echo -e "  ${DIM}This may take several minutes depending on image size.${NC}"
echo ""

gzip -v "$OUTPUT_FILE"

echo ""
echo -e "  ${GREEN}Done${NC} - image compressed"
echo ""

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
FINAL_SIZE=$(ls -lh "$COMPRESSED_FILE" | awk '{print $5}')

echo -e "${BOLD}${GREEN}============================================${NC}"
echo -e "${BOLD}${GREEN}  Golden Image Created Successfully${NC}"
echo -e "${BOLD}${GREEN}============================================${NC}"
echo ""
echo -e "  ${BOLD}File:${NC}     ${COMPRESSED_FILE}"
echo -e "  ${BOLD}Size:${NC}     ${FINAL_SIZE}"
echo -e "  ${BOLD}Version:${NC}  ${VERSION}"
echo ""
echo -e "${BOLD}To flash this image onto a new SD card:${NC}"
echo ""
echo -e "  ${CYAN}./scripts/golden_image/flash_image.sh ${IMAGE_NAME}.gz${NC}"
echo ""
echo -e "${DIM}Alternatively, use Raspberry Pi Imager:${NC}"
echo -e "${DIM}  1. Open Raspberry Pi Imager${NC}"
echo -e "${DIM}  2. Choose OS -> Use custom -> select ${IMAGE_NAME}.gz${NC}"
echo -e "${DIM}  3. Choose Storage -> select target SD card${NC}"
echo -e "${DIM}  4. Click Write${NC}"
echo ""
