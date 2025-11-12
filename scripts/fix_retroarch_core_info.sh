#!/bin/bash
# Fix RetroArch core info files (info.zip) error on Raspberry Pi OS Lite
# This script diagnoses and fixes common issues preventing core info updates

set -e

LOG_FILE="/tmp/retroarch_core_info_fix.log"
echo "$(date): Starting RetroArch core info fix script" | tee -a "$LOG_FILE"

# Configuration
CONFIG_DIR="$HOME/.config/retroarch"
CONFIG_FILE="$CONFIG_DIR/retroarch.cfg"
INFO_DIR="$CONFIG_DIR/info"
CORES_DIR="$CONFIG_DIR/cores"
INFO_ZIP_URL="https://buildbot.libretro.com/assets/frontend/info.zip"
INFO_ZIP_TEMP="/tmp/retroarch_info.zip"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo_info() {
    echo -e "${GREEN}[INFO]${NC} $1" | tee -a "$LOG_FILE"
}

echo_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1" | tee -a "$LOG_FILE"
}

echo_error() {
    echo -e "${RED}[ERROR]${NC} $1" | tee -a "$LOG_FILE"
}

# Step 1: Check network connectivity
echo_info "Step 1: Checking network connectivity..."
if ! ping -c 1 -W 5 buildbot.libretro.com >/dev/null 2>&1; then
    echo_error "Cannot reach buildbot.libretro.com - check your internet connection"
    exit 1
fi
echo_info "Network connectivity OK"

# Step 2: Check and install required dependencies
echo_info "Step 2: Checking required dependencies..."

MISSING_DEPS=()

if ! command -v curl >/dev/null 2>&1; then
    MISSING_DEPS+=("curl")
fi

if ! command -v unzip >/dev/null 2>&1; then
    MISSING_DEPS+=("unzip")
fi

if ! command -v wget >/dev/null 2>&1; then
    MISSING_DEPS+=("wget")
fi

if [ ${#MISSING_DEPS[@]} -gt 0 ]; then
    echo_warn "Missing dependencies: ${MISSING_DEPS[*]}"
    echo_info "Installing missing dependencies..."
    sudo apt-get update -qq
    sudo apt-get install -y "${MISSING_DEPS[@]}" || {
        echo_error "Failed to install dependencies"
        exit 1
    }
    echo_info "Dependencies installed successfully"
else
    echo_info "All required dependencies are installed"
fi

# Step 3: Create necessary directories
echo_info "Step 3: Creating necessary directories..."
mkdir -p "$CONFIG_DIR"
mkdir -p "$INFO_DIR"
mkdir -p "$CORES_DIR"

# Ensure directories are writable
chmod 755 "$CONFIG_DIR"
chmod 755 "$INFO_DIR"
chmod 755 "$CORES_DIR"

# Check if directories are writable
if [ ! -w "$INFO_DIR" ]; then
    echo_error "Info directory is not writable: $INFO_DIR"
    echo_info "Attempting to fix permissions..."
    chmod 755 "$INFO_DIR" || {
        echo_error "Failed to fix permissions. You may need to run: sudo chown -R $USER:$USER $CONFIG_DIR"
        exit 1
    }
fi
echo_info "Directories created and permissions verified"

# Step 4: Backup existing info files (if any)
if [ -d "$INFO_DIR" ] && [ "$(ls -A "$INFO_DIR"/*.info 2>/dev/null)" ]; then
    BACKUP_DIR="${INFO_DIR}.backup.$(date +%Y%m%d_%H%M%S)"
    echo_info "Step 4: Backing up existing info files to $BACKUP_DIR"
    cp -r "$INFO_DIR" "$BACKUP_DIR"
    echo_info "Backup created: $BACKUP_DIR"
else
    echo_info "Step 4: No existing info files to backup"
fi

# Step 5: Download info.zip with multiple methods
echo_info "Step 5: Downloading core info files (info.zip)..."

DOWNLOAD_SUCCESS=false

# Method 1: Try curl with SSL verification
echo_info "Attempting download with curl..."
if curl -L -f --connect-timeout 30 --max-time 120 \
    "https://buildbot.libretro.com/assets/frontend/info.zip" \
    -o "$INFO_ZIP_TEMP" 2>>"$LOG_FILE"; then
    if [ -f "$INFO_ZIP_TEMP" ] && [ -s "$INFO_ZIP_TEMP" ]; then
        echo_info "Download successful with curl"
        DOWNLOAD_SUCCESS=true
    else
        echo_warn "Download completed but file is empty or missing"
        rm -f "$INFO_ZIP_TEMP"
    fi
else
    echo_warn "curl download failed, trying alternative methods..."
fi

# Method 2: Try curl without SSL verification (if SSL issues)
if [ "$DOWNLOAD_SUCCESS" = false ]; then
    echo_info "Attempting download with curl (no SSL verification)..."
    if curl -L -f -k --connect-timeout 30 --max-time 120 \
        "https://buildbot.libretro.com/assets/frontend/info.zip" \
        -o "$INFO_ZIP_TEMP" 2>>"$LOG_FILE"; then
        if [ -f "$INFO_ZIP_TEMP" ] && [ -s "$INFO_ZIP_TEMP" ]; then
            echo_info "Download successful with curl (no SSL verification)"
            DOWNLOAD_SUCCESS=true
        else
            rm -f "$INFO_ZIP_TEMP"
        fi
    fi
fi

# Method 3: Try wget
if [ "$DOWNLOAD_SUCCESS" = false ]; then
    echo_info "Attempting download with wget..."
    if wget --timeout=30 --tries=3 \
        "https://buildbot.libretro.com/assets/frontend/info.zip" \
        -O "$INFO_ZIP_TEMP" 2>>"$LOG_FILE"; then
        if [ -f "$INFO_ZIP_TEMP" ] && [ -s "$INFO_ZIP_TEMP" ]; then
            echo_info "Download successful with wget"
            DOWNLOAD_SUCCESS=true
        else
            rm -f "$INFO_ZIP_TEMP"
        fi
    fi
fi

# Method 4: Try wget without SSL verification
if [ "$DOWNLOAD_SUCCESS" = false ]; then
    echo_info "Attempting download with wget (no SSL verification)..."
    if wget --no-check-certificate --timeout=30 --tries=3 \
        "https://buildbot.libretro.com/assets/frontend/info.zip" \
        -O "$INFO_ZIP_TEMP" 2>>"$LOG_FILE"; then
        if [ -f "$INFO_ZIP_TEMP" ] && [ -s "$INFO_ZIP_TEMP" ]; then
            echo_info "Download successful with wget (no SSL verification)"
            DOWNLOAD_SUCCESS=true
        else
            rm -f "$INFO_ZIP_TEMP"
        fi
    fi
fi

if [ "$DOWNLOAD_SUCCESS" = false ]; then
    echo_error "Failed to download info.zip with all methods"
    echo_error "Possible causes:"
    echo_error "  - Network connectivity issues"
    echo_error "  - Firewall blocking HTTPS connections"
    echo_error "  - RetroArch servers may be down"
    echo_error ""
    echo_info "You can try manually downloading from:"
    echo_info "  $INFO_ZIP_URL"
    echo_info "And extracting to: $INFO_DIR"
    exit 1
fi

# Step 6: Verify downloaded file
echo_info "Step 6: Verifying downloaded file..."
FILE_SIZE=$(stat -f%z "$INFO_ZIP_TEMP" 2>/dev/null || stat -c%s "$INFO_ZIP_TEMP" 2>/dev/null)
if [ "$FILE_SIZE" -lt 1000 ]; then
    echo_error "Downloaded file is too small ($FILE_SIZE bytes) - likely corrupted"
    rm -f "$INFO_ZIP_TEMP"
    exit 1
fi
echo_info "File size: $FILE_SIZE bytes"

# Verify it's a valid zip file
if ! unzip -t "$INFO_ZIP_TEMP" >/dev/null 2>&1; then
    echo_error "Downloaded file is not a valid zip archive"
    rm -f "$INFO_ZIP_TEMP"
    exit 1
fi
echo_info "File is a valid zip archive"

# Step 7: Extract info files
echo_info "Step 7: Extracting core info files..."
if unzip -o "$INFO_ZIP_TEMP" -d "$INFO_DIR" >>"$LOG_FILE" 2>&1; then
    INFO_COUNT=$(ls -1 "$INFO_DIR"/*.info 2>/dev/null | wc -l)
    if [ "$INFO_COUNT" -gt 0 ]; then
        echo_info "Successfully extracted $INFO_COUNT core info files"
    else
        echo_error "Extraction completed but no .info files found"
        exit 1
    fi
else
    echo_error "Failed to extract info.zip"
    rm -f "$INFO_ZIP_TEMP"
    exit 1
fi

# Clean up temp file
rm -f "$INFO_ZIP_TEMP"
echo_info "Temporary file cleaned up"

# Step 8: Update RetroArch configuration
echo_info "Step 8: Updating RetroArch configuration..."

# Ensure config file exists
if [ ! -f "$CONFIG_FILE" ]; then
    echo_info "Creating new RetroArch config file..."
    touch "$CONFIG_FILE"
fi

# Update core updater settings
echo_info "Configuring core updater settings..."

# Remove existing settings to avoid duplicates
sed -i '/^core_updater_buildbot_info_url/d' "$CONFIG_FILE"
sed -i '/^core_updater_buildbot_cores_url/d' "$CONFIG_FILE"
sed -i '/^core_updater_buildbot_assets_url/d' "$CONFIG_FILE"
sed -i '/^core_updater_auto_extract_archive/d' "$CONFIG_FILE"
sed -i '/^info_directory/d' "$CONFIG_FILE"
sed -i '/^libretro_directory/d' "$CONFIG_FILE"
sed -i '/^core_updater_show_experimental_cores/d' "$CONFIG_FILE"
sed -i '/^menu_show_core_updater/d' "$CONFIG_FILE"
sed -i '/^menu_show_online_updater/d' "$CONFIG_FILE"

# Add correct settings
cat >> "$CONFIG_FILE" <<EOF

# Core updater configuration (configured by fix_retroarch_core_info.sh)
core_updater_buildbot_info_url = "$INFO_ZIP_URL"
core_updater_buildbot_cores_url = "https://buildbot.libretro.com/nightly/linux/aarch64/latest"
core_updater_buildbot_assets_url = "https://buildbot.libretro.com/assets/"
core_updater_auto_extract_archive = "true"
core_updater_show_experimental_cores = "true"
info_directory = "$INFO_DIR"
libretro_directory = "$CORES_DIR"
menu_show_core_updater = "true"
menu_show_online_updater = "true"
EOF

echo_info "RetroArch configuration updated"

# Step 9: Verify installation
echo_info "Step 9: Verifying installation..."
if [ -d "$INFO_DIR" ] && [ "$(ls -A "$INFO_DIR"/*.info 2>/dev/null)" ]; then
    INFO_COUNT=$(ls -1 "$INFO_DIR"/*.info 2>/dev/null | wc -l)
    echo_info "✓ Core info files installed: $INFO_COUNT files"
    
    # Show a few example files
    echo_info "Sample core info files:"
    ls -1 "$INFO_DIR"/*.info 2>/dev/null | head -5 | while read -r file; do
        echo_info "  - $(basename "$file")"
    done
else
    echo_error "Verification failed - no info files found"
    exit 1
fi

# Step 10: Test RetroArch can see the files
echo_info "Step 10: Testing RetroArch configuration..."
if command -v retroarch >/dev/null 2>&1; then
    echo_info "RetroArch is installed"
    echo_info "You can now:"
    echo_info "  1. Launch RetroArch"
    echo_info "  2. Go to: Online Updater -> Update Core Info Files"
    echo_info "  3. Go to: Online Updater -> Core Downloader"
    echo_info "  4. Download cores for your games"
else
    echo_warn "RetroArch not found in PATH"
    echo_info "Install RetroArch with: sudo apt install retroarch"
fi

echo ""
echo_info "=========================================="
echo_info "Core info files fix completed successfully!"
echo_info "=========================================="
echo_info "Info directory: $INFO_DIR"
echo_info "Cores directory: $CORES_DIR"
echo_info "Config file: $CONFIG_FILE"
echo_info "Log file: $LOG_FILE"
echo ""
echo_info "Next steps:"
echo_info "  1. Launch RetroArch: retroarch"
echo_info "  2. Navigate to: Online Updater -> Core Downloader"
echo_info "  3. Download the cores you need (e.g., Parallel N64, FCEUmm, PCSX Rearmed)"
echo ""

