# Golden Image Provisioning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create a complete "golden image" workflow that lets the user clone a working Raspberry Pi 4B (2GB) onto new SD cards — shipping all 148 curated game ROMs but with clean video playlists — and have each new Pi boot to a working kiosk with a unique identity.

**Architecture:** Three Pi-side scripts (prepare, first-boot, and a systemd oneshot) plus two Mac-side helpers (create image, flash image). All scripts live in `scripts/golden_image/` to keep them separate from the existing deployment infrastructure. The existing `deploy_cpp.sh`, `setup_pi.sh`, etc. remain untouched for the script-based provisioning path.

**Tech Stack:** Bash scripts, systemd, dd, gzip, PiShrink (optional via Docker), Raspberry Pi OS Bookworm (64-bit Lite)

**Hardware Target:** Raspberry Pi 4B with 2GB RAM

---

## File Map

| Action | Path | Purpose |
|--------|------|---------|
| Create | `scripts/golden_image/prepare_golden_image.sh` | Runs on working Pi to clean it for imaging |
| Create | `scripts/golden_image/first_boot.sh` | Runs once on each new Pi clone at first boot |
| Create | `systemd/magic-first-boot.service` | Systemd oneshot that triggers first_boot.sh |
| Create | `scripts/golden_image/create_image.sh` | Runs on Mac to dd the SD card into a .img file |
| Create | `scripts/golden_image/flash_image.sh` | Runs on Mac to flash a .img onto a new SD card |

**Existing files — NOT modified:**
- `scripts/setup_pi.sh` — still works for script-based provisioning
- `magic_dingus_box_cpp/scripts/deploy_cpp.sh` — still works for dev iteration
- `magic_dingus_box_cpp/scripts/provision_user.sh` — still works standalone
- `magic_dingus_box_cpp/scripts/install_deps.sh` — still works standalone
- All systemd service files in `systemd/` and `magic_dingus_box_cpp/systemd/`

---

### Task 1: Create `prepare_golden_image.sh`

This script runs on the **working Pi** (via SSH) to clean it for cloning. It preserves game ROMs, game playlists, emulator cores, BIOS files, and the compiled binary. It removes video playlists, user media, device identity, logs, SSH host keys, and bash history.

**Files:**
- Create: `scripts/golden_image/prepare_golden_image.sh`

- [ ] **Step 1: Create the script file with header and safety checks**

```bash
#!/usr/bin/env bash
set -euo pipefail

#
# prepare_golden_image.sh
#
# Prepares the current working Pi for SD card cloning.
# Keeps: game ROMs, game playlists, cores, BIOS, compiled binary, assets, thumbnails
# Removes: video playlists, user media, device identity, saves, logs, SSH host keys
#
# Usage: sudo ./scripts/golden_image/prepare_golden_image.sh
#
# After running:
#   1. sudo shutdown -h now
#   2. Remove SD card
#   3. On Mac: ./scripts/golden_image/create_image.sh
#

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

INSTALL_DIR="/opt/magic_dingus_box"
DATA_DIR="$INSTALL_DIR/magic_dingus_box_cpp/data"
CONFIG_DIR="$INSTALL_DIR/config"
MAGIC_USER="magic"

# Must run as root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Please run as root: sudo $0${NC}"
    exit 1
fi

# Safety: confirm this is the right Pi
echo -e "${CYAN}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║       Magic Dingus Box - Golden Image Preparation       ║${NC}"
echo -e "${CYAN}╚══════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${YELLOW}This will prepare this Pi for SD card cloning.${NC}"
echo ""
echo "What will be KEPT:"
echo "  - All game ROMs (data/roms/)"
echo "  - All game playlists (arcade, nes, snes, genesis, etc.)"
echo "  - Game thumbnails (data/thumbnails/)"
echo "  - Intro video (data/intro/)"
echo "  - Compiled binary (build/magic_dingus_box_cpp)"
echo "  - RetroArch cores and BIOS files"
echo "  - All installed packages and dependencies"
echo "  - System configuration (boot, audio, USB gadget)"
echo ""
echo "What will be REMOVED:"
echo "  - Video/music playlists (danny_gatton, wes_montgomery, paul_franklin)"
echo "  - User media files (data/media/*, dev_data/)"
echo "  - Device identity (device_info.json)"
echo "  - Game saves and save states"
echo "  - Settings (will regenerate with defaults on first boot)"
echo "  - SSH host keys (regenerated on first boot)"
echo "  - Logs and bash history"
echo ""
read -p "Continue? (y/N): " confirm
if [[ ! "$confirm" =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 0
fi

echo ""
echo -e "${GREEN}[1/9] Stopping services...${NC}"
systemctl stop magic-dingus-box-cpp.service 2>/dev/null || true
systemctl stop magic-dingus-web.service 2>/dev/null || true
echo "  Services stopped"

echo -e "${GREEN}[2/9] Removing video/music playlists...${NC}"
# Remove only video-type playlists. Game playlists stay.
# Video playlists are identified by having source_type: local items
PLAYLIST_DIR="$DATA_DIR/playlists"
VIDEO_PLAYLISTS=(
    "danny_gatton.yaml"
    "wes_montgomery.yaml"
    "paul_franklin.yaml"
)
for pl in "${VIDEO_PLAYLISTS[@]}"; do
    if [ -f "$PLAYLIST_DIR/$pl" ]; then
        rm "$PLAYLIST_DIR/$pl"
        echo "  Removed $pl"
    fi
done
echo "  Game playlists preserved"

echo -e "${GREEN}[3/9] Clearing user media...${NC}"
# Clear media directory (video files uploaded by user)
if [ -d "$DATA_DIR/media" ]; then
    rm -rf "$DATA_DIR/media"/*
    echo "  Cleared data/media/"
fi
# Clear dev_data if it exists on Pi
if [ -d "$INSTALL_DIR/magic_dingus_box_cpp/dev_data" ]; then
    rm -rf "$INSTALL_DIR/magic_dingus_box_cpp/dev_data"
    echo "  Removed dev_data/"
fi
echo "  Media cleared"

echo -e "${GREEN}[4/9] Clearing saves and states...${NC}"
if [ -d "$DATA_DIR/saves" ]; then
    rm -rf "$DATA_DIR/saves"/*
    echo "  Cleared saves/"
fi
if [ -d "$DATA_DIR/states" ]; then
    rm -rf "$DATA_DIR/states"/*
    echo "  Cleared states/"
fi
echo "  Saves cleared"

echo -e "${GREEN}[5/9] Removing device identity and settings...${NC}"
# Remove device_info.json (regenerated on first boot with new UUID)
find "$INSTALL_DIR" -name "device_info.json" -delete 2>/dev/null || true
# Remove settings (will regenerate with factory defaults)
rm -f "$CONFIG_DIR/settings.json"
echo "  Device identity and settings cleared"

echo -e "${GREEN}[6/9] Clearing logs...${NC}"
rm -f "$CONFIG_DIR/magic_dingus_box.log"
rm -f "$INSTALL_DIR/magic_dingus_box_cpp/build/"*.log 2>/dev/null || true
rm -f /home/$MAGIC_USER/retroarch_launcher.log 2>/dev/null || true
journalctl --rotate 2>/dev/null || true
journalctl --vacuum-time=1s 2>/dev/null || true
echo "  Logs cleared"

echo -e "${GREEN}[7/9] Installing first-boot service...${NC}"
# Copy first_boot.sh to the install directory
FIRST_BOOT_SRC="$INSTALL_DIR/scripts/golden_image/first_boot.sh"
FIRST_BOOT_DST="/opt/magic_dingus_box/scripts/golden_image/first_boot.sh"
if [ -f "$FIRST_BOOT_SRC" ]; then
    chmod +x "$FIRST_BOOT_SRC"
else
    echo -e "${YELLOW}  Warning: first_boot.sh not found at $FIRST_BOOT_SRC${NC}"
    echo -e "${YELLOW}  First-boot setup will not run on cloned Pis${NC}"
fi
# Install the systemd service
SERVICE_SRC="$INSTALL_DIR/systemd/magic-first-boot.service"
if [ -f "$SERVICE_SRC" ]; then
    cp "$SERVICE_SRC" /etc/systemd/system/magic-first-boot.service
    systemctl daemon-reload
    systemctl enable magic-first-boot.service
    echo "  First-boot service installed and enabled"
else
    echo -e "${YELLOW}  Warning: magic-first-boot.service not found${NC}"
fi

echo -e "${GREEN}[8/9] Clearing SSH host keys and user history...${NC}"
# SSH host keys will be regenerated on first boot
rm -f /etc/ssh/ssh_host_*
# Clear bash history
rm -f /home/$MAGIC_USER/.bash_history
rm -f /root/.bash_history
history -c 2>/dev/null || true
echo "  SSH keys and history cleared"

echo -e "${GREEN}[9/9] Verification...${NC}"
echo ""
echo "  Checking critical files:"

# Verify binary exists
if [ -f "$INSTALL_DIR/magic_dingus_box_cpp/build/magic_dingus_box_cpp" ]; then
    echo -e "  ${GREEN}+${NC} Compiled binary present"
else
    echo -e "  ${RED}!${NC} WARNING: Compiled binary missing!"
fi

# Count ROMs
ROM_COUNT=$(find "$DATA_DIR/roms" -type f \( -name "*.zip" -o -name "*.7z" -o -name "*.chd" -o -name "*.m3u" \) 2>/dev/null | wc -l)
echo -e "  ${GREEN}+${NC} $ROM_COUNT ROM files in data/roms/"

# Count game playlists
PLAYLIST_COUNT=$(ls "$PLAYLIST_DIR"/*.yaml 2>/dev/null | wc -l)
echo -e "  ${GREEN}+${NC} $PLAYLIST_COUNT game playlists"

# Check cores
CORE_COUNT=$(find /home/$MAGIC_USER/.config/retroarch/cores -name "*.so" 2>/dev/null | wc -l)
echo -e "  ${GREEN}+${NC} $CORE_COUNT RetroArch cores installed"

# Check BIOS
if [ -f "/home/$MAGIC_USER/.config/retroarch/system/scph5501.bin" ]; then
    echo -e "  ${GREEN}+${NC} PS1 BIOS present"
else
    echo -e "  ${YELLOW}-${NC} PS1 BIOS not found (PS1 games won't work without it)"
fi

# Check no video playlists remain
VIDEO_REMAINING=0
for pl in "${VIDEO_PLAYLISTS[@]}"; do
    if [ -f "$PLAYLIST_DIR/$pl" ]; then
        VIDEO_REMAINING=$((VIDEO_REMAINING + 1))
    fi
done
if [ $VIDEO_REMAINING -eq 0 ]; then
    echo -e "  ${GREEN}+${NC} Video playlists removed"
else
    echo -e "  ${YELLOW}!${NC} $VIDEO_REMAINING video playlists still present"
fi

# Check media is empty
MEDIA_COUNT=$(find "$DATA_DIR/media" -type f 2>/dev/null | wc -l)
if [ "$MEDIA_COUNT" -eq 0 ]; then
    echo -e "  ${GREEN}+${NC} Media directory clean"
else
    echo -e "  ${YELLOW}!${NC} $MEDIA_COUNT files still in media/"
fi

# Check first-boot is enabled
if systemctl is-enabled magic-first-boot.service 2>/dev/null | grep -q enabled; then
    echo -e "  ${GREEN}+${NC} First-boot service enabled"
else
    echo -e "  ${YELLOW}!${NC} First-boot service not enabled"
fi

echo ""
echo -e "${GREEN}=== Golden Image Preparation Complete ===${NC}"
echo ""
echo "Next steps:"
echo "  1. Review the output above for any warnings"
echo "  2. Shut down:  sudo shutdown -h now"
echo "  3. Remove the SD card"
echo "  4. On your Mac, run:  ./scripts/golden_image/create_image.sh"
echo ""
```

- [ ] **Step 2: Make the script executable**

```bash
chmod +x scripts/golden_image/prepare_golden_image.sh
```

- [ ] **Step 3: Commit**

```bash
git add scripts/golden_image/prepare_golden_image.sh
git commit -m "feat: add prepare_golden_image.sh for SD card cloning workflow"
```

---

### Task 2: Create `first_boot.sh` and systemd service

This script runs **once** on each newly cloned Pi at first boot. It generates a unique device identity, expands the filesystem to fill the SD card, regenerates SSH host keys, creates required directories, and disables itself.

**Files:**
- Create: `scripts/golden_image/first_boot.sh`
- Create: `systemd/magic-first-boot.service`

- [ ] **Step 1: Create first_boot.sh**

```bash
#!/usr/bin/env bash
set -euo pipefail

#
# first_boot.sh
#
# Runs once on each newly cloned Pi to set up unique identity
# and expand the filesystem. Called by magic-first-boot.service.
#
# This script:
#   1. Regenerates SSH host keys
#   2. Expands root partition to fill the SD card
#   3. Generates unique device_info.json
#   4. Creates saves/states directories
#   5. Resets file permissions
#   6. Disables itself (runs only once)
#

LOG_TAG="magic-first-boot"
INSTALL_DIR="/opt/magic_dingus_box"
DATA_DIR="$INSTALL_DIR/magic_dingus_box_cpp/data"
CONFIG_DIR="$INSTALL_DIR/config"
MAGIC_USER="magic"

log() {
    echo "$1"
    logger -t "$LOG_TAG" "$1"
}

log "=== Magic Dingus Box First Boot Setup ==="

# --- 1. Regenerate SSH host keys ---
log "[1/6] Regenerating SSH host keys..."
if [ ! -f /etc/ssh/ssh_host_ed25519_key ]; then
    ssh-keygen -A
    systemctl restart sshd 2>/dev/null || systemctl restart ssh 2>/dev/null || true
    log "  SSH host keys regenerated"
else
    log "  SSH host keys already exist, skipping"
fi

# --- 2. Expand root filesystem ---
log "[2/6] Expanding root filesystem..."
ROOT_PART=$(findmnt -n -o SOURCE /)
if [ -n "$ROOT_PART" ]; then
    ROOT_DISK="/dev/$(lsblk -no pkname "$ROOT_PART")"
    PART_NUM=$(echo "$ROOT_PART" | grep -o '[0-9]*$')

    # Get current and total sizes
    CURRENT_SIZE=$(lsblk -bno SIZE "$ROOT_PART" 2>/dev/null || echo "0")
    DISK_SIZE=$(lsblk -bno SIZE "$ROOT_DISK" 2>/dev/null || echo "0")

    if [ "$CURRENT_SIZE" -lt "$DISK_SIZE" ]; then
        log "  Expanding partition $PART_NUM on $ROOT_DISK..."
        parted -s "$ROOT_DISK" resizepart "$PART_NUM" 100%
        resize2fs "$ROOT_PART"
        log "  Filesystem expanded"
    else
        log "  Filesystem already fills the SD card, skipping"
    fi
else
    log "  WARNING: Could not determine root partition"
fi

# --- 3. Generate unique device identity ---
log "[3/6] Generating device identity..."
DEVICE_ID=$(cat /proc/sys/kernel/random/uuid)
DEVICE_INFO="$DATA_DIR/device_info.json"
mkdir -p "$(dirname "$DEVICE_INFO")"
cat > "$DEVICE_INFO" <<EOF
{
  "device_id": "$DEVICE_ID",
  "device_name": "Magic Dingus Box",
  "created_at": $(date +%s)
}
EOF
chown "$MAGIC_USER:$MAGIC_USER" "$DEVICE_INFO"
log "  Device ID: $DEVICE_ID"

# --- 4. Create required directories ---
log "[4/6] Creating directories..."
mkdir -p "$DATA_DIR/saves"
mkdir -p "$DATA_DIR/states"
mkdir -p "$DATA_DIR/media"
mkdir -p "$CONFIG_DIR"
log "  Directories created"

# --- 5. Fix permissions ---
log "[5/6] Setting permissions..."
chown -R "$MAGIC_USER:$MAGIC_USER" "$INSTALL_DIR"
chown -R "$MAGIC_USER:$MAGIC_USER" "/home/$MAGIC_USER/.config" 2>/dev/null || true
log "  Permissions set"

# --- 6. Disable this service (run only once) ---
log "[6/6] Disabling first-boot service..."
systemctl disable magic-first-boot.service
log "  First-boot service disabled"

log "=== First Boot Setup Complete ==="
log "Device is ready. ID: $DEVICE_ID"
```

- [ ] **Step 2: Make first_boot.sh executable**

```bash
chmod +x scripts/golden_image/first_boot.sh
```

- [ ] **Step 3: Create the systemd service file**

Create `systemd/magic-first-boot.service`:

```ini
[Unit]
Description=Magic Dingus Box First Boot Setup
# Run early, before our main services
Before=magic-dingus-box-cpp.service magic-dingus-web.service
After=local-fs.target
# Only run if first_boot.sh exists
ConditionPathExists=/opt/magic_dingus_box/scripts/golden_image/first_boot.sh

[Service]
Type=oneshot
ExecStart=/bin/bash /opt/magic_dingus_box/scripts/golden_image/first_boot.sh
RemainAfterExit=no
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

- [ ] **Step 4: Commit**

```bash
git add scripts/golden_image/first_boot.sh systemd/magic-first-boot.service
git commit -m "feat: add first-boot service for per-unit identity and filesystem expansion"
```

---

### Task 3: Create `create_image.sh` (Mac-side)

Runs on the user's Mac to create a compressed SD card image. Detects the SD card device, reads it with `dd`, and optionally shrinks it with PiShrink via Docker.

**Files:**
- Create: `scripts/golden_image/create_image.sh`

- [ ] **Step 1: Create the script**

```bash
#!/usr/bin/env bash
set -euo pipefail

#
# create_image.sh
#
# Creates a compressed SD card image from a Pi's SD card.
# Run this on your Mac after preparing the golden image on the Pi.
#
# Usage:
#   ./scripts/golden_image/create_image.sh                    # Auto-detect SD card
#   ./scripts/golden_image/create_image.sh /dev/disk4         # Specify device
#   ./scripts/golden_image/create_image.sh /dev/disk4 --shrink  # Shrink with PiShrink (requires Docker)
#
# Output: magic_dingus_box_golden_<version>_<date>.img.gz
#

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
VERSION=$(cat "$PROJECT_ROOT/VERSION" 2>/dev/null || echo "unknown")
DATE=$(date +%Y%m%d)
OUTPUT_DIR="$PROJECT_ROOT"
IMAGE_NAME="magic_dingus_box_golden_v${VERSION}_${DATE}.img"
SHRINK=false
DISK_DEVICE=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --shrink|-s)
            SHRINK=true
            shift
            ;;
        --help|-h)
            cat <<EOF
Create a golden SD card image from a Raspberry Pi's SD card.

Usage: $(basename "$0") [/dev/diskN] [--shrink]

Options:
  /dev/diskN     SD card device (auto-detected if omitted)
  --shrink, -s   Shrink image with PiShrink (requires Docker Desktop)
  --help, -h     Show this help

Output: ${IMAGE_NAME}.gz in project root

Prerequisites:
  1. Run prepare_golden_image.sh on the Pi
  2. Shut down the Pi (sudo shutdown -h now)
  3. Remove the SD card and insert it into your Mac
EOF
            exit 0
            ;;
        /dev/*)
            DISK_DEVICE="$1"
            shift
            ;;
        *)
            echo -e "${RED}Unknown argument: $1${NC}"
            exit 1
            ;;
    esac
done

echo -e "${CYAN}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║       Magic Dingus Box - Create Golden Image            ║${NC}"
echo -e "${CYAN}╚══════════════════════════════════════════════════════════╝${NC}"
echo ""

# Must be macOS
if [[ "$(uname)" != "Darwin" ]]; then
    echo -e "${RED}This script is designed for macOS.${NC}"
    echo "On Linux, use: sudo dd if=/dev/sdX of=${IMAGE_NAME} bs=4M status=progress"
    exit 1
fi

# Auto-detect SD card if not specified
if [ -z "$DISK_DEVICE" ]; then
    echo -e "${YELLOW}Detecting SD card...${NC}"
    echo ""
    # List external disks (likely SD cards)
    echo "Available disks:"
    echo "────────────────"
    diskutil list external physical 2>/dev/null || diskutil list
    echo ""
    echo -e "${YELLOW}Enter the SD card device (e.g., /dev/disk4):${NC}"
    read -p "> " DISK_DEVICE
fi

# Validate device exists
if [ ! -e "$DISK_DEVICE" ]; then
    echo -e "${RED}Device $DISK_DEVICE does not exist${NC}"
    exit 1
fi

# Get disk info for confirmation
DISK_INFO=$(diskutil info "$DISK_DEVICE" 2>/dev/null | grep -E "Disk Size|Media Name|Device Node" || echo "Unknown disk")
DISK_SIZE=$(diskutil info "$DISK_DEVICE" 2>/dev/null | grep "Disk Size" | awk -F'(' '{print $1}' | sed 's/.*: *//')

echo ""
echo -e "${YELLOW}Selected disk:${NC}"
echo "$DISK_INFO"
echo ""
echo -e "${YELLOW}Output: ${OUTPUT_DIR}/${IMAGE_NAME}.gz${NC}"
echo ""
echo -e "${RED}WARNING: Make sure this is the correct SD card!${NC}"
read -p "Continue? (y/N): " confirm
if [[ ! "$confirm" =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 0
fi

# Use raw device for faster I/O
RAW_DEVICE="${DISK_DEVICE/disk/rdisk}"

# Unmount all partitions (but don't eject)
echo ""
echo -e "${GREEN}[1/3] Unmounting partitions...${NC}"
diskutil unmountDisk "$DISK_DEVICE"

# Read SD card
echo -e "${GREEN}[2/3] Reading SD card (this may take 10-30 minutes)...${NC}"
echo "  Source: $RAW_DEVICE"
echo "  Destination: $OUTPUT_DIR/$IMAGE_NAME"
echo ""

sudo dd if="$RAW_DEVICE" of="$OUTPUT_DIR/$IMAGE_NAME" bs=4m status=progress

echo ""
echo -e "${GREEN}  Raw image created: $(du -h "$OUTPUT_DIR/$IMAGE_NAME" | awk '{print $1}')${NC}"

# Optional: Shrink with PiShrink
if [ "$SHRINK" = true ]; then
    echo -e "${GREEN}[2.5/3] Shrinking image with PiShrink (via Docker)...${NC}"
    if command -v docker &>/dev/null; then
        docker run --rm --privileged \
            -v "$OUTPUT_DIR":/workdir \
            mgomesborges/pishrink \
            pishrink.sh "/workdir/$IMAGE_NAME"
        echo -e "${GREEN}  Image shrunk: $(du -h "$OUTPUT_DIR/$IMAGE_NAME" | awk '{print $1}')${NC}"
    else
        echo -e "${YELLOW}  Docker not found. Skipping shrink.${NC}"
        echo "  Install Docker Desktop to enable PiShrink support."
    fi
fi

# Compress
echo -e "${GREEN}[3/3] Compressing image...${NC}"
gzip -v "$OUTPUT_DIR/$IMAGE_NAME"

FINAL_SIZE=$(du -h "$OUTPUT_DIR/${IMAGE_NAME}.gz" | awk '{print $1}')
echo ""
echo -e "${GREEN}=== Image Created Successfully ===${NC}"
echo ""
echo "  File: ${IMAGE_NAME}.gz"
echo "  Size: ${FINAL_SIZE}"
echo "  Version: v${VERSION}"
echo ""
echo "To flash onto a new SD card:"
echo "  ./scripts/golden_image/flash_image.sh ${IMAGE_NAME}.gz"
echo ""
echo "Or use Raspberry Pi Imager:"
echo "  1. Open Raspberry Pi Imager"
echo "  2. Choose OS > Use custom"
echo "  3. Select ${IMAGE_NAME}.gz"
echo "  4. Choose your SD card"
echo "  5. Write"
```

- [ ] **Step 2: Make executable**

```bash
chmod +x scripts/golden_image/create_image.sh
```

- [ ] **Step 3: Commit**

```bash
git add scripts/golden_image/create_image.sh
git commit -m "feat: add create_image.sh for Mac-side SD card imaging"
```

---

### Task 4: Create `flash_image.sh` (Mac-side)

Runs on the user's Mac to flash a golden image onto a new SD card.

**Files:**
- Create: `scripts/golden_image/flash_image.sh`

- [ ] **Step 1: Create the script**

```bash
#!/usr/bin/env bash
set -euo pipefail

#
# flash_image.sh
#
# Flashes a golden image onto a new SD card.
# Run this on your Mac.
#
# Usage:
#   ./scripts/golden_image/flash_image.sh magic_dingus_box_golden_v1.3.0_20260412.img.gz
#   ./scripts/golden_image/flash_image.sh magic_dingus_box_golden_v1.3.0_20260412.img.gz /dev/disk4
#

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

IMAGE_FILE="${1:-}"
DISK_DEVICE="${2:-}"

if [ -z "$IMAGE_FILE" ]; then
    echo "Usage: $(basename "$0") <image.img.gz> [/dev/diskN]"
    echo ""
    echo "Arguments:"
    echo "  image.img.gz   The golden image file (gzipped)"
    echo "  /dev/diskN     Target SD card (auto-detected if omitted)"
    exit 1
fi

if [ ! -f "$IMAGE_FILE" ]; then
    echo -e "${RED}Image file not found: $IMAGE_FILE${NC}"
    exit 1
fi

echo -e "${CYAN}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║       Magic Dingus Box - Flash Golden Image             ║${NC}"
echo -e "${CYAN}╚══════════════════════════════════════════════════════════╝${NC}"
echo ""

# Must be macOS
if [[ "$(uname)" != "Darwin" ]]; then
    echo -e "${RED}This script is designed for macOS.${NC}"
    echo "On Linux, use: gzip -dc $IMAGE_FILE | sudo dd of=/dev/sdX bs=4M status=progress"
    exit 1
fi

# Auto-detect SD card if not specified
if [ -z "$DISK_DEVICE" ]; then
    echo -e "${YELLOW}Detecting SD card...${NC}"
    echo ""
    echo "Available disks:"
    echo "────────────────"
    diskutil list external physical 2>/dev/null || diskutil list
    echo ""
    echo -e "${YELLOW}Enter the target SD card device (e.g., /dev/disk4):${NC}"
    read -p "> " DISK_DEVICE
fi

# Validate device
if [ ! -e "$DISK_DEVICE" ]; then
    echo -e "${RED}Device $DISK_DEVICE does not exist${NC}"
    exit 1
fi

# Show disk info
DISK_INFO=$(diskutil info "$DISK_DEVICE" 2>/dev/null | grep -E "Disk Size|Media Name|Device Node" || echo "Unknown disk")
echo ""
echo -e "${YELLOW}Target disk:${NC}"
echo "$DISK_INFO"
echo ""
echo -e "${YELLOW}Image: $IMAGE_FILE${NC}"
echo ""
echo -e "${RED}WARNING: ALL DATA ON $DISK_DEVICE WILL BE ERASED!${NC}"
read -p "Type 'yes' to continue: " confirm
if [ "$confirm" != "yes" ]; then
    echo "Aborted."
    exit 0
fi

RAW_DEVICE="${DISK_DEVICE/disk/rdisk}"

echo ""
echo -e "${GREEN}[1/3] Unmounting partitions...${NC}"
diskutil unmountDisk "$DISK_DEVICE"

echo -e "${GREEN}[2/3] Flashing image (this may take 5-15 minutes)...${NC}"
echo "  Source: $IMAGE_FILE"
echo "  Target: $RAW_DEVICE"
echo ""

gzip -dc "$IMAGE_FILE" | sudo dd of="$RAW_DEVICE" bs=4m status=progress

echo ""
echo -e "${GREEN}[3/3] Ejecting SD card...${NC}"
sync
diskutil eject "$DISK_DEVICE"

echo ""
echo -e "${GREEN}=== Flash Complete ===${NC}"
echo ""
echo "Next steps:"
echo "  1. Insert the SD card into the new Raspberry Pi"
echo "  2. Connect HDMI and power"
echo "  3. The Pi will boot and run first-time setup automatically"
echo "  4. After ~30 seconds, the kiosk will start with all games ready"
echo "  5. Upload video content via the web manager at http://magicpi.local:5000"
echo ""
```

- [ ] **Step 2: Make executable**

```bash
chmod +x scripts/golden_image/flash_image.sh
```

- [ ] **Step 3: Commit**

```bash
git add scripts/golden_image/flash_image.sh
git commit -m "feat: add flash_image.sh for writing golden images to new SD cards"
```

---

### Task 5: Verify integration with existing scripts

Confirm the new scripts don't interfere with the existing deployment infrastructure and that the first-boot service plays nicely with the existing systemd services.

**Files:**
- Read: `systemd/magic-dingus-box-cpp.service` (verify ordering)
- Read: `systemd/magic-dingus-web.service` (verify ordering)
- Read: `scripts/setup_pi.sh` (verify no conflicts)

- [ ] **Step 1: Verify service ordering is correct**

The `magic-first-boot.service` has `Before=magic-dingus-box-cpp.service magic-dingus-web.service`, which means:
- On first boot: first-boot runs, then the main app starts
- On subsequent boots: first-boot is disabled, main app starts directly

Verify by reading the existing services and confirming no circular dependencies:

```bash
# On Pi (or review files locally):
# magic-dingus-box-cpp.service has After=network-online.target
# magic-first-boot.service has After=local-fs.target, Before=magic-dingus-box-cpp.service
# No circular dependency.
```

- [ ] **Step 2: Verify prepare script doesn't break deploy_cpp.sh**

The prepare script only removes:
- Video playlists (3 specific files)
- Media content (data/media/*)
- Device identity (device_info.json)
- Saves/states content
- Settings
- SSH host keys
- Logs

None of these are required by `deploy_cpp.sh`. The deploy script syncs code, not user data. Verified compatible.

- [ ] **Step 3: Verify the golden image still supports the script-based path**

A Pi flashed from the golden image can still receive updates via:
- `deploy_cpp.sh --build` (code updates)
- `update.sh` (OTA updates)
- `setup_pi.sh` (re-run is idempotent)

These are all additive operations that don't conflict with the golden image workflow.

- [ ] **Step 4: Commit all scripts together with a summary**

If any adjustments were needed during verification, commit them:

```bash
git add -A scripts/golden_image/ systemd/magic-first-boot.service
git commit -m "feat: golden image provisioning workflow for cloning Pi units

Adds scripts to prepare a working Pi for SD card imaging, handle
per-unit first-boot setup (unique device ID, filesystem expansion,
SSH key regeneration), and create/flash images from macOS.

Workflow: prepare_golden_image.sh (Pi) -> create_image.sh (Mac) -> flash_image.sh (Mac)

Existing deployment scripts (deploy_cpp.sh, setup_pi.sh) remain
fully functional for the script-based provisioning path."
```

---

## Full Workflow Summary

```
WORKING PI                           MAC                              NEW PI
───────────                          ───                              ──────
1. SSH in                                                             
2. sudo prepare_golden_image.sh                                       
3. sudo shutdown -h now                                               
4. Remove SD card ─────────────> 5. Insert SD card                    
                                 6. ./create_image.sh                 
                                 7. Remove SD card                    
                                 8. Insert blank SD card              
                                 9. ./flash_image.sh image.img.gz     
                                10. Remove SD card ──────────────> 11. Insert + power on
                                                                  12. first_boot.sh (auto)
                                                                      - SSH keys regen
                                                                      - Filesystem expand
                                                                      - Device ID created
                                                                  13. Kiosk starts with games
                                                                  14. Upload videos via web UI
```

Repeat steps 8-14 for each additional Pi.
