#!/usr/bin/env bash
set -euo pipefail

#
# Deploy C++ Kiosk Engine to Raspberry Pi
# Usage:
#   ./scripts/deploy_cpp.sh              # sync code to Pi
#   ./scripts/deploy_cpp.sh --build      # sync + build on Pi
#   ./scripts/deploy_cpp.sh --test       # sync + build + test
#   ./scripts/deploy_cpp.sh --cores      # sync + build + install cores
#   PI_HOST=pi@1.2.3.4 ./scripts/deploy_cpp.sh
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

PI_HOST="${PI_HOST:-magic@magicpi.local}"
PI_DIR="${PI_DIR:-/opt/magic_dingus_box}"
BUILD=false
TEST=false
INSTALL_CORES=false
SETUP_USB_GADGET=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build|-b)
            BUILD=true
            shift
            ;;
        --test|-t)
            BUILD=true
            TEST=true
            shift
            ;;
        --cores|-c)
            BUILD=true
            INSTALL_CORES=true
            shift
            ;;
        --usb-gadget|-u)
            SETUP_USB_GADGET=true
            shift
            ;;
        --help|-h)
            cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --build, -b     Sync code and build on Pi
  --test, -t      Sync, build, and test on Pi
  --cores, -c     Sync, build, install RetroArch cores
  --usb-gadget, -u  Setup USB Ethernet Gadget mode for fast uploads
  --help, -h      Show this help

Environment overrides:
  PI_HOST         SSH target (default: ${PI_HOST})
  PI_DIR          Remote base path (default: ${PI_DIR})
EOF
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

echo "=== Deploying C++ Kiosk Engine to ${PI_HOST} ==="
echo ""

# Check connectivity before proceeding
echo "Checking connectivity to ${PI_HOST}..."
if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "${PI_HOST}" "echo 'Connection successful'" >/dev/null 2>&1; then
    echo ""
    echo "✗ ERROR: Cannot connect to ${PI_HOST}"
    echo ""
    echo "Possible solutions:"
    echo "  1. Use IP address instead: PI_HOST=magic@192.168.1.XXX ./scripts/deploy_cpp.sh --build"
    echo "  2. Ensure Pi is powered on and on the same network"
    echo "  3. Check if mDNS/Bonjour is working: ping magicpi.local"
    echo "  4. Find Pi IP: ssh magic@<known-ip> 'hostname -I'"
    echo ""
    exit 1
fi
echo "  ✓ Connection successful"
echo ""

# Step 1: Sync code
echo "Step 1: Syncing code to ${PI_HOST}:${PI_DIR}/magic_dingus_box_cpp"
rsync -avz \
    --delete \
    --exclude '.git' \
    --exclude 'build' \
    --exclude '.DS_Store' \
    --exclude '*.o' \
    --exclude '*.a' \
    --exclude 'CMakeCache.txt' \
    --exclude 'CMakeFiles' \
    --exclude 'data/playlists/*' \
    --exclude 'data/roms/*' \
    --exclude 'data/media/*' \
    --exclude 'data/device_info.json' \
    --exclude 'dev_data/roms/*' \
    --exclude 'dev_data/media/*' \
    "${CPP_DIR}/" \
    "${PI_HOST}:${PI_DIR}/magic_dingus_box_cpp/"

echo "  ✓ Code synced"

# Step 1.5: Sync Web UI
echo "Step 1.5: Syncing Web UI to ${PI_HOST}:${PI_DIR}/magic_dingus_box/web"
rsync -avz \
    --delete \
    --exclude '__pycache__' \
    "${CPP_DIR}/../magic_dingus_box/web/" \
    "${PI_HOST}:${PI_DIR}/magic_dingus_box/web/"
    
echo "  ✓ Web UI synced"

# Step 1.55: Sync VERSION file and update script
echo "Step 1.55: Syncing VERSION file and update script..."
rsync -avz \
    "${CPP_DIR}/../VERSION" \
    "${PI_HOST}:${PI_DIR}/"

# Make update.sh executable on Pi
ssh "${PI_HOST}" "chmod +x ${PI_DIR}/magic_dingus_box_cpp/scripts/update.sh" 2>/dev/null || true
echo "  ✓ VERSION file and update script synced"

# Step 1.6: Install Web UI Service
echo "Step 1.6: Installing Web UI Service..."
rsync -avz \
    "${CPP_DIR}/../systemd/magic-dingus-web.service" \
    "${PI_HOST}:${PI_DIR}/systemd/"

ssh "${PI_HOST}" bash <<'EOF'
sudo cp /opt/magic_dingus_box/systemd/magic-dingus-web.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable magic-dingus-web.service
sudo systemctl restart magic-dingus-web.service
EOF
echo "  ✓ Web UI Service installed and started"
echo ""

# Step 1.7: Install C++ App Service
echo "Step 1.7: Installing C++ App Service..."
rsync -avz \
    "${CPP_DIR}/systemd/magic-dingus-box-cpp.service" \
    "${PI_HOST}:${PI_DIR}/systemd/"

ssh "${PI_HOST}" bash <<'EOF'
sudo cp /opt/magic_dingus_box/systemd/magic-dingus-box-cpp.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable magic-dingus-box-cpp.service
# Restart if it's already running (or start if not)
sudo systemctl restart magic-dingus-box-cpp.service
EOF
echo "  ✓ C++ App Service installed and started"
echo ""

# Step 1.8: Install Local Cores
echo "Step 1.8: Installing Local Cores..."
# Ensure core directory exists
ssh "${PI_HOST}" "mkdir -p /home/magic/.config/retroarch/cores"
rsync -avz \
    "${CPP_DIR}/libretro_cores/" \
    "${PI_HOST}:/home/magic/.config/retroarch/cores/"
echo "  ✓ Local cores installed"
echo ""

# Step 2: Download stb_truetype.h (always update to ensure it's the real file)
echo "Step 2: Ensuring stb_truetype.h is present..."
ssh "${PI_HOST}" bash <<'EOF'
cd /opt/magic_dingus_box/magic_dingus_box_cpp/src/ui

# Check if file is just a placeholder (contains "Placeholder")
if [ -f stb_truetype.h ] && grep -q "Placeholder" stb_truetype.h 2>/dev/null; then
    echo "  Placeholder file detected, downloading real stb_truetype.h..."
    rm -f stb_truetype.h
fi

# Download if missing or if download failed before
if [ ! -f stb_truetype.h ] || [ ! -s stb_truetype.h ]; then
    echo "  Downloading stb_truetype.h..."
    wget -q https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h -O stb_truetype.h
    if [ $? -eq 0 ] && [ -s stb_truetype.h ]; then
        echo "  ✓ stb_truetype.h downloaded"
    else
        echo "  ✗ Failed to download stb_truetype.h"
        exit 1
    fi
else
    echo "  ✓ stb_truetype.h already exists"
fi
EOF
echo ""

# Step 3: Build (if requested)
if [ "$BUILD" = true ]; then
    echo "Step 3: Building on Pi..."
    ssh "${PI_HOST}" bash <<'EOF'
set -e
cd /opt/magic_dingus_box/magic_dingus_box_cpp

# Check and install dependencies
echo "  Checking dependencies..."
MISSING_DEPS=()

# Map pkg-config names to apt package names
declare -A PKG_MAP=(
    ["libdrm"]="libdrm-dev"
    ["gbm"]="libgbm-dev"
    ["egl"]="libegl1-mesa-dev"
    ["glesv2"]="libgles2-mesa-dev"
    ["libevdev"]="libevdev-dev"
    ["libgpiod"]="libgpiod-dev"
    ["yaml-cpp"]="libyaml-cpp-dev"
    ["jsoncpp"]="libjsoncpp-dev"
)

# Check each dependency
for pkg in libdrm gbm egl glesv2 libevdev libgpiod yaml-cpp jsoncpp; do
    if ! pkg-config --exists "$pkg" 2>/dev/null; then
        MISSING_DEPS+=("${PKG_MAP[$pkg]}")
    fi
done

if [ ${#MISSING_DEPS[@]} -gt 0 ]; then
    echo "  Installing missing dependencies: ${MISSING_DEPS[*]}"
    sudo apt update -qq
    sudo apt install -y "${MISSING_DEPS[@]}"
    echo "  ✓ Dependencies installed"
else
    echo "  ✓ All dependencies present"
fi

# Build
echo "  Running cmake..."
mkdir -p build
cd build

# Always run cmake to ensure it's up to date
if ! cmake ..; then
    echo "  ✗ CMake configuration failed!"
    exit 1
fi

echo "  Compiling..."
if ! make -j4; then
    echo "  ✗ Build failed!"
    exit 1
fi

echo "  ✓ Build complete"
EOF
    echo ""
fi

# Step 3.5: Install RetroArch cores (if requested)
if [ "$INSTALL_CORES" = true ]; then
    echo "Step 3.5: Installing RetroArch cores on Pi..."

    # Run the core installation using the script inside the C++ project
    ssh "${PI_HOST}" bash <<'EOF'
set -e
cd /opt/magic_dingus_box/magic_dingus_box_cpp

echo "  Installing RetroArch and cores..."
# Install RetroArch first
sudo apt update -qq
sudo apt install -y retroarch

# Run the consolidated core installer
echo "  Running core installer..."
chmod +x scripts/install_cores.sh
if sudo scripts/install_cores.sh; then
    echo "  ✓ Cores installed successfully"
else
    echo "  ✗ Core installation failed"
    exit 1
fi

echo "  ✓ Core installation complete"
EOF
    echo ""
fi

# Step 4: Test (if requested)
if [ "$TEST" = true ]; then
    echo "Step 4: Testing on Pi..."
    echo "  Note: This will start the app. Press Ctrl+C in the SSH session to stop."
    echo ""
    ssh -t "${PI_HOST}" bash <<'EOF'
cd /opt/magic_dingus_box/magic_dingus_box_cpp/build
if [ ! -f magic_dingus_box_cpp ]; then
    echo "ERROR: Executable not found. Build may have failed."
    exit 1
fi

echo "Starting app (run as root for DRM access)..."
sudo ./magic_dingus_box_cpp
EOF
    echo ""
fi

# Step 5: Setup USB Gadget (if requested)
if [ "$SETUP_USB_GADGET" = true ]; then
    echo "Step 5: Setting up USB Ethernet Gadget mode..."
    echo ""
    echo "  This enables direct laptop-to-Pi USB connections for faster uploads."
    echo "  The Pi will need to reboot after setup."
    echo ""
    
    # Run the setup script on the Pi
    ssh "${PI_HOST}" bash <<'EOF'
cd /opt/magic_dingus_box/magic_dingus_box_cpp/scripts
chmod +x setup_usb_gadget.sh

# Run non-interactively (don't prompt for reboot)
sudo bash -c '
set -e

echo "=== Setting up USB Ethernet Gadget Mode ==="

# Detect config.txt location
if [ -f /boot/firmware/config.txt ]; then
    CONFIG_TXT="/boot/firmware/config.txt"
elif [ -f /boot/config.txt ]; then
    CONFIG_TXT="/boot/config.txt"
else
    echo "ERROR: Cannot find config.txt"
    exit 1
fi

# Enable dwc2 overlay
if ! grep -q "^dtoverlay=dwc2" "$CONFIG_TXT"; then
    if grep -q "^\[all\]" "$CONFIG_TXT"; then
        sed -i "/^\[all\]/a dtoverlay=dwc2" "$CONFIG_TXT"
    else
        echo "dtoverlay=dwc2" >> "$CONFIG_TXT"
    fi
    echo "  ✓ Enabled dwc2 overlay"
else
    echo "  ✓ dwc2 overlay already enabled"
fi

# Add kernel modules
if ! grep -q "^dwc2" /etc/modules; then
    echo "dwc2" >> /etc/modules
    echo "  ✓ Added dwc2 module"
fi

if ! grep -q "^g_ether" /etc/modules; then
    echo "g_ether" >> /etc/modules
    echo "  ✓ Added g_ether module"
fi

# Configure static IP for usb0
mkdir -p /etc/systemd/network
cat > /etc/systemd/network/10-usb-gadget.network << NETEOF
[Match]
Name=usb0

[Network]
Address=192.168.7.1/24
DHCPServer=yes

[DHCPServer]
PoolOffset=100
PoolSize=20
EmitDNS=no
NETEOF

echo "  ✓ Configured static IP 192.168.7.1"

# Enable systemd-networkd
systemctl enable systemd-networkd 2>/dev/null || true

echo ""
echo "=== USB Gadget Setup Complete ==="
'
EOF
    
    echo ""
    echo "  ✓ USB Ethernet Gadget mode configured"
    echo ""
    echo "  ⚠️  REBOOT REQUIRED: Run 'ssh ${PI_HOST} sudo reboot' to activate USB mode"
    echo ""
fi

echo "=== Deployment Complete ==="
echo ""

if [ "$SETUP_USB_GADGET" = true ]; then
    echo "🔌 USB Ethernet Gadget mode configured!"
    echo ""
    echo "   After rebooting the Pi:"
    echo "   1. Connect USB-C cable from laptop to Pi"
    echo "   2. Wait 5-10 seconds"
    echo "   3. Open browser to: http://192.168.7.1:5000"
    echo ""
    echo "   WiFi access continues to work at: http://magicpi.local:5000"
    echo ""
fi

if [ "$INSTALL_CORES" = true ]; then
    echo "🎮 RetroArch cores pre-installed! Your games will launch immediately."
    echo ""
    echo "Game readiness status:"
    echo "  ✓ NES: nestopia_libretro.so"
    echo "  ✓ N64: mupen64plus-next_libretro.so"
    echo "  ✓ PS1: pcsx_rearmed_libretro.so"
    echo ""
fi

if [ "$BUILD" = false ]; then
    echo "Next steps:"
    echo "  1. SSH to Pi: ssh ${PI_HOST}"
    echo "  2. Build: cd ${PI_DIR}/magic_dingus_box_cpp && mkdir -p build && cd build && cmake .. && make -j4"
    echo "  3. Test: sudo ./magic_dingus_box_cpp"
fi
if [ "$TEST" = false ] && [ "$BUILD" = true ] && [ "$INSTALL_CORES" = false ]; then
    echo "Next steps:"
    echo "  1. SSH to Pi: ssh ${PI_HOST}"
    echo "  2. Test: cd ${PI_DIR}/magic_dingus_box_cpp/build && sudo ./magic_dingus_box_cpp"
fi

