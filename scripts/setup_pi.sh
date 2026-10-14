#!/bin/bash
#
# Magic Dingus Box - Raspberry Pi Setup Script
# Run this after cloning the repo to a new Pi
#
# Usage: sudo ./scripts/setup_pi.sh
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== Magic Dingus Box Pi Setup ===${NC}"

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Please run as root (sudo ./scripts/setup_pi.sh)${NC}"
    exit 1
fi

# Get the directory where the script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo -e "${YELLOW}Project directory: ${PROJECT_DIR}${NC}"

# === 1. Install systemd service files ===
echo -e "\n${GREEN}[1/6] Installing systemd service files...${NC}"

# Through converge_system_units.sh --only, the one writer of unit files: it
# installs the tree's copy of each (the kiosk unit from
# magic_dingus_box_cpp/systemd/, never a stale root-level copy — one used to
# be installed here: Type=simple, NO Restart=, no WatchdogSec, no
# EnvironmentFile) and daemon-reloads when it wrote.
bash "$PROJECT_DIR/magic_dingus_box_cpp/scripts/converge_system_units.sh" \
    --only magic-dingus-box-cpp.service --only magic-dingus-web.service

echo "Service files installed"

# === 2. Optimize boot time - mask slow services ===
echo -e "\n${GREEN}[2/6] Optimizing boot time...${NC}"

# Mask the network wait service (causes 2+ minute delay)
systemctl mask systemd-networkd-wait-online.service 2>/dev/null || true
echo "Masked systemd-networkd-wait-online.service"

# Disable cloud-init (not needed for kiosk)
systemctl disable cloud-init.service 2>/dev/null || true
systemctl disable cloud-init-main.service 2>/dev/null || true
systemctl disable cloud-init-network.service 2>/dev/null || true
systemctl disable cloud-init-local.service 2>/dev/null || true
echo "Disabled cloud-init services"

# === 3. Reload systemd ===
echo -e "\n${GREEN}[3/6] Reloading systemd...${NC}"
systemctl daemon-reload
echo "Systemd reloaded"

# === 4. Enable services ===
echo -e "\n${GREEN}[4/6] Enabling Magic Dingus Box services...${NC}"
systemctl enable magic-dingus-box-cpp.service
systemctl enable magic-dingus-web.service 2>/dev/null || echo "Web service not found, skipping"
echo "Services enabled"

# === 5. Create required directories ===
echo -e "\n${GREEN}[5/6] Creating required directories...${NC}"

# Config directory
mkdir -p /opt/magic_dingus_box/config
chown -R magic:magic /opt/magic_dingus_box/config 2>/dev/null || true

# Data directories (if not exist)
mkdir -p /opt/magic_dingus_box/magic_dingus_box_cpp/data/media
mkdir -p /opt/magic_dingus_box/magic_dingus_box_cpp/data/roms
mkdir -p /opt/magic_dingus_box/magic_dingus_box_cpp/data/playlists
mkdir -p /opt/magic_dingus_box/magic_dingus_box_cpp/data/intro
chown -R magic:magic /opt/magic_dingus_box/magic_dingus_box_cpp/data 2>/dev/null || true

echo "Directories created"

# === 6. Set permissions ===
echo -e "\n${GREEN}[6/6] Setting permissions...${NC}"

# Ensure magic user owns the project
chown -R magic:magic /opt/magic_dingus_box 2>/dev/null || true

# Make sure magic user is in required groups
usermod -aG video,audio,input,render magic 2>/dev/null || true

echo "Permissions set"

# === Done ===
echo -e "\n${GREEN}=== Setup Complete! ===${NC}"
echo ""
echo "Next steps:"
echo "  1. Build the C++ app:  cd /opt/magic_dingus_box/magic_dingus_box_cpp/build && cmake .. && make -j2"
echo "     (-j2 on a Pi: -j4 OOMs a 1.5 GB Pi 4B and swaps a 2 GB Pi 5)"
echo "  2. Reboot:             sudo reboot"
echo ""
echo "Boot time should be ~10-20 seconds (not 2+ minutes)"
