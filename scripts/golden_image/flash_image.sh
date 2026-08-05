#!/usr/bin/env bash
set -euo pipefail

#
# Magic Dingus Box - Flash Golden Image
#
# Flashes a compressed golden image (.img.gz) onto an SD card from macOS.
# Handles device selection, confirmation, unmounting, flashing, and ejection.
#
# Usage: ./scripts/golden_image/flash_image.sh <image.img.gz> [/dev/diskN]
#
# Must be run on macOS (Darwin). If run on Linux, prints the equivalent
# dd command for the user to run manually.
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
# Banner
# ---------------------------------------------------------------------------
echo ""
echo -e "${BOLD}${CYAN}============================================${NC}"
echo -e "${BOLD}${CYAN}  Magic Dingus Box - Flash Golden Image${NC}"
echo -e "${BOLD}${CYAN}============================================${NC}"
echo ""

# ---------------------------------------------------------------------------
# Validate image file argument
# ---------------------------------------------------------------------------
if [[ $# -lt 1 ]]; then
    echo -e "${RED}Error: Missing image file argument.${NC}"
    echo ""
    echo -e "${BOLD}Usage:${NC} $0 <image.img.gz> [/dev/diskN]"
    echo ""
    echo -e "  ${DIM}image.img.gz${NC}   Path to the compressed golden image"
    echo -e "  ${DIM}/dev/diskN${NC}     Target SD card device (optional, will prompt if omitted)"
    exit 1
fi

IMAGE_FILE="$1"

if [[ ! -f "$IMAGE_FILE" ]]; then
    echo -e "${RED}Error: Image file not found: ${IMAGE_FILE}${NC}"
    exit 1
fi

if [[ "$IMAGE_FILE" != *.img.gz ]]; then
    echo -e "${RED}Error: Image file must be a .img.gz file: ${IMAGE_FILE}${NC}"
    exit 1
fi

# ---------------------------------------------------------------------------
# Platform check
# ---------------------------------------------------------------------------
OS="$(uname)"
if [[ "$OS" != "Darwin" ]]; then
    echo -e "${YELLOW}This script is designed for macOS.${NC}"
    echo ""
    if [[ "$OS" == "Linux" ]]; then
        echo -e "On Linux, use this command instead:"
        echo ""
        echo -e "  ${CYAN}gzip -dc \"${IMAGE_FILE}\" | sudo dd of=/dev/sdX bs=4M status=progress${NC}"
        echo ""
        echo -e "  Replace ${BOLD}/dev/sdX${NC} with your SD card device."
        echo -e "  ${RED}Double-check the device -- dd will overwrite without confirmation.${NC}"
    else
        echo -e "${RED}Unsupported platform: ${OS}${NC}"
    fi
    exit 1
fi

# ---------------------------------------------------------------------------
# Device selection
# ---------------------------------------------------------------------------
DISK_DEVICE="${2:-}"

if [[ -z "$DISK_DEVICE" ]]; then
    echo -e "${BOLD}Available external disks:${NC}"
    echo ""
    diskutil list external physical
    echo ""
    read -r -p "$(echo -e "${BOLD}Enter target device (e.g. /dev/disk4): ${NC}")" DISK_DEVICE

    if [[ -z "$DISK_DEVICE" ]]; then
        echo -e "${RED}Error: No device specified. Aborting.${NC}"
        exit 1
    fi
fi

# ---------------------------------------------------------------------------
# Validate device
# ---------------------------------------------------------------------------
if [[ ! -e "$DISK_DEVICE" ]]; then
    echo -e "${RED}Error: Device does not exist: ${DISK_DEVICE}${NC}"
    exit 1
fi

# Ensure it looks like a disk device path
if [[ "$DISK_DEVICE" != /dev/disk* ]]; then
    echo -e "${RED}Error: Device must be a /dev/diskN path: ${DISK_DEVICE}${NC}"
    exit 1
fi

# ---------------------------------------------------------------------------
# Show disk info for confirmation
# ---------------------------------------------------------------------------
echo ""
echo -e "${BOLD}Target disk information:${NC}"
echo ""
diskutil info "$DISK_DEVICE" | grep -E '(Device Identifier|Device Node|Media Name|Disk Size|Protocol|Removable Media|Device / Media Name)' || true
echo ""

# ---------------------------------------------------------------------------
# Confirmation
# ---------------------------------------------------------------------------
IMAGE_BASENAME="$(basename "$IMAGE_FILE")"
IMAGE_SIZE="$(du -h "$IMAGE_FILE" | cut -f1 | tr -d ' ')"

echo -e "${BOLD}Summary:${NC}"
echo -e "  Image:   ${CYAN}${IMAGE_BASENAME}${NC} (${IMAGE_SIZE})"
echo -e "  Target:  ${CYAN}${DISK_DEVICE}${NC}"
echo ""
echo -e "  ${RED}${BOLD}WARNING: ALL DATA ON ${DISK_DEVICE} WILL BE ERASED!${NC}"
echo ""
read -r -p "$(echo -e "${BOLD}Type \"yes\" to continue: ${NC}")" confirm

if [[ "$confirm" != "yes" ]]; then
    echo -e "${RED}Aborted.${NC}"
    exit 1
fi

echo ""

# ---------------------------------------------------------------------------
# Derive raw device for faster I/O
# ---------------------------------------------------------------------------
# /dev/disk4 -> /dev/rdisk4 (must match the pattern used in create_image.sh)
RAW_DEVICE="${DISK_DEVICE/\/dev\/disk//dev/rdisk}"

# ---------------------------------------------------------------------------
# Step 1: Unmount
# ---------------------------------------------------------------------------
echo -e "${CYAN}[1/3] Unmounting ${DISK_DEVICE}...${NC}"

diskutil unmountDisk "$DISK_DEVICE"
echo -e "  ${GREEN}Done${NC}"
echo ""

# ---------------------------------------------------------------------------
# Step 2: Flash
# ---------------------------------------------------------------------------
echo -e "${CYAN}[2/3] Flashing image to ${RAW_DEVICE}...${NC}"
echo -e "  ${DIM}This may take several minutes depending on image size and card speed.${NC}"
echo -e "  ${DIM}You may be prompted for your password (sudo).${NC}"
echo ""

# macOS BSD dd does not support status=progress; use Ctrl+T for progress
echo -e "  ${DIM}Tip: Press Ctrl+T at any time to see progress${NC}"
gzip -dc "$IMAGE_FILE" | sudo dd of="$RAW_DEVICE" bs=4m

echo ""
echo -e "  ${GREEN}Flash complete${NC}"
echo ""

# ---------------------------------------------------------------------------
# Step 3: Sync and eject
# ---------------------------------------------------------------------------
echo -e "${CYAN}[3/3] Syncing and ejecting...${NC}"

sync
echo -e "  ${GREEN}Sync complete${NC}"

if diskutil eject "$DISK_DEVICE"; then
    echo -e "  ${GREEN}Ejected ${DISK_DEVICE}${NC}"
else
    echo -e "  ${YELLOW}Warning: Eject failed. Safely remove the SD card manually.${NC}"
fi

# ---------------------------------------------------------------------------
# Success and next steps
# ---------------------------------------------------------------------------
echo ""
echo -e "${GREEN}${BOLD}SD card flashed successfully!${NC}"
echo ""
echo -e "${BOLD}Next steps:${NC}"
echo -e "  1. Remove the SD card from your Mac"
echo -e "  2. Insert it into a Raspberry Pi 4B"
echo -e "  3. Connect HDMI and power"
echo -e "  4. First-boot setup runs automatically (SSH keys, filesystem expansion)"
echo -e "  5. Kiosk starts in ~30 seconds"
echo -e "  6. Upload videos via the web manager at ${CYAN}http://magicpi-XXXX.local:5000${NC}"
echo -e "     (first boot names each unit magicpi-XXXX — the kiosk's Settings →"
echo -e "     Info screen shows the exact address and a QR code)"
echo ""
