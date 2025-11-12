#!/bin/bash
# Install RetroPie cores for Magic Dingus Box
# Adds RetroPie repository and installs required cores via apt

set -e

echo "=== Installing RetroPie Cores for Magic Dingus Box ==="
echo ""

# Check if running as root for apt operations
if [ "$EUID" -ne 0 ]; then 
    echo "This script requires sudo privileges for package installation."
    echo "Please run: sudo $0"
    exit 1
fi

# Try to detect Debian version for RetroPie repository
DEBIAN_VERSION=$(lsb_release -cs 2>/dev/null || echo "bookworm")
echo "   Detected Debian version: $DEBIAN_VERSION"

# RetroPie repository configuration
# Note: RetroPie repository may not be available for all Debian versions
# We'll try the repository first, then fall back to standard Debian packages
RETROPIE_REPO="http://archive.retropie.org.uk/debian"
RETROPIE_DIST="jessie"  # RetroPie uses jessie as base, but may work with newer versions
RETROPIE_COMPONENT="main"
APT_SOURCES_FILE="/etc/apt/sources.list.d/retropie.list"
GPG_KEY_URL="http://archive.retropie.org.uk/debian/archive.key"

echo "1. Checking RetroPie repository availability..."
# Test if repository is reachable
REPO_REACHABLE=false
if command -v curl >/dev/null 2>&1; then
    if curl -s --connect-timeout 5 --max-time 10 "$GPG_KEY_URL" >/dev/null 2>&1; then
        REPO_REACHABLE=true
        echo "   ✓ RetroPie repository is reachable"
    else
        echo "   ✗ RetroPie repository is not reachable (will try standard packages)"
    fi
elif command -v wget >/dev/null 2>&1; then
    if wget --timeout=5 --tries=1 -q --spider "$GPG_KEY_URL" 2>/dev/null; then
        REPO_REACHABLE=true
        echo "   ✓ RetroPie repository is reachable"
    else
        echo "   ✗ RetroPie repository is not reachable (will try standard packages)"
    fi
fi

if [ "$REPO_REACHABLE" = true ]; then
    echo ""
    echo "2. Adding RetroPie repository..."
    if [ -f "$APT_SOURCES_FILE" ]; then
        echo "   Repository already exists, checking configuration..."
        # Update if needed
        if ! grep -q "$RETROPIE_REPO" "$APT_SOURCES_FILE"; then
            echo "deb $RETROPIE_REPO $RETROPIE_DIST $RETROPIE_COMPONENT" > "$APT_SOURCES_FILE"
            echo "   Updated $APT_SOURCES_FILE"
        fi
    else
        echo "deb $RETROPIE_REPO $RETROPIE_DIST $RETROPIE_COMPONENT" > "$APT_SOURCES_FILE"
        echo "   Created $APT_SOURCES_FILE"
    fi

    echo ""
    echo "3. Importing RetroPie GPG key..."
    # Download and add GPG key
    if command -v wget >/dev/null 2>&1; then
        wget -qO - "$GPG_KEY_URL" | apt-key add - 2>/dev/null || {
            echo "   Warning: Failed to add GPG key via apt-key (may need manual import)"
        }
    elif command -v curl >/dev/null 2>&1; then
        curl -sL "$GPG_KEY_URL" | apt-key add - 2>/dev/null || {
            echo "   Warning: Failed to add GPG key via apt-key (may need manual import)"
        }
    else
        echo "   Error: Neither wget nor curl found. Cannot import GPG key."
    fi

    echo ""
    echo "4. Updating package lists..."
    apt-get update -qq 2>&1 | grep -i retropie || true
else
    echo ""
    echo "2. Skipping RetroPie repository (not reachable)"
    echo "   Will use standard Debian/Raspberry Pi OS packages instead"
fi

echo ""
echo "4. Installing RetroArch (if not already installed)..."
if ! command -v retroarch >/dev/null 2>&1; then
    apt-get install -y retroarch
    echo "   RetroArch installed"
else
    echo "   RetroArch already installed: $(which retroarch)"
fi

echo ""
echo "5. Installing cores..."

# Try RetroPie package names first, then fall back to standard Debian names
declare -A CORE_PACKAGES=(
    ["NES"]="lr-fceumm libretro-fceumm libretro-nestopia"
    ["SNES"]="lr-snes9x libretro-snes9x"
    ["N64"]="lr-mupen64plus-next libretro-mupen64plus-next"
    ["PS1"]="lr-pcsx-rearmed libretro-pcsx-rearmed"
)

INSTALLED_CORES=()
FAILED_CORES=()

for system in "${!CORE_PACKAGES[@]}"; do
    packages=(${CORE_PACKAGES[$system]})
    installed=false
    
    echo "   Installing $system core..."
    for package in "${packages[@]}"; do
        if apt-get install -y "$package" 2>&1 | grep -qE "is already the newest version|Setting up|Unpacking"; then
            INSTALLED_CORES+=("$package")
            echo "   ✓ $system core installed: $package"
            installed=true
            break
        fi
    done
    
    if [ "$installed" = false ]; then
        FAILED_CORES+=("$system")
        echo "   ✗ Failed to install $system core (tried: ${packages[*]})"
    fi
done

echo ""
echo "6. Verifying core installation locations..."

# Check common RetroPie core directories
CORE_DIRS=(
    "/opt/retropie/libretrocores"
    "/usr/lib/aarch64-linux-gnu/libretro"
    "/usr/lib/arm-linux-gnueabihf/libretro"
    "/usr/lib/libretro"
    "$HOME/.config/retroarch/cores"
)

FOUND_DIRS=()
for dir in "${CORE_DIRS[@]}"; do
    if [ -d "$dir" ] && [ "$(ls -A "$dir" 2>/dev/null)" ]; then
        FOUND_DIRS+=("$dir")
        echo "   ✓ Found cores in: $dir"
        ls -1 "$dir"/*.so 2>/dev/null | head -5 | while read core; do
            echo "     - $(basename "$core")"
        done
    fi
done

echo ""
echo "7. Updating RetroArch configuration..."

# Determine the home directory of the user who will run RetroArch
# If running as root, try to find the primary user (usually the first user with UID >= 1000)
if [ "$EUID" -eq 0 ]; then
    # Try to find the primary user (usually the one with autologin or first regular user)
    PRIMARY_USER=$(getent passwd | awk -F: '$3 >= 1000 && $3 < 65534 {print $1; exit}')
    if [ -z "$PRIMARY_USER" ]; then
        PRIMARY_USER="alexanderchaney"  # Fallback to default
    fi
    USER_HOME=$(getent passwd "$PRIMARY_USER" | cut -d: -f6)
    echo "   Running as root, using user: $PRIMARY_USER (home: $USER_HOME)"
else
    USER_HOME="$HOME"
    PRIMARY_USER="$USER"
fi

CONFIG_DIR="$USER_HOME/.config/retroarch"
CONFIG_FILE="$CONFIG_DIR/retroarch.cfg"
mkdir -p "$CONFIG_DIR"
# Ensure config directory is owned by the user
if [ "$EUID" -eq 0 ] && [ -n "$PRIMARY_USER" ]; then
    chown -R "$PRIMARY_USER:$PRIMARY_USER" "$CONFIG_DIR" 2>/dev/null || true
fi

# Build libretro_directory with all found directories
CORES_DIR="$CONFIG_DIR/cores"
RETROPIE_CORES="/opt/retropie/libretrocores"
SYSTEM_CORES="/usr/lib/aarch64-linux-gnu/libretro"
SYSTEM_CORES_ARMHF="/usr/lib/arm-linux-gnueabihf/libretro"

LIBRETRO_DIRS="$CORES_DIR"
[ -d "$RETROPIE_CORES" ] && LIBRETRO_DIRS="$LIBRETRO_DIRS;$RETROPIE_CORES"
[ -d "$SYSTEM_CORES" ] && LIBRETRO_DIRS="$LIBRETRO_DIRS;$SYSTEM_CORES"
[ -d "$SYSTEM_CORES_ARMHF" ] && LIBRETRO_DIRS="$LIBRETRO_DIRS;$SYSTEM_CORES_ARMHF"

# Update or create config file
if [ -f "$CONFIG_FILE" ]; then
    # Remove existing libretro_directory line
    sed -i '/^libretro_directory/d' "$CONFIG_FILE"
    echo "libretro_directory = \"$LIBRETRO_DIRS\"" >> "$CONFIG_FILE"
    echo "   Updated RetroArch config: $CONFIG_FILE"
else
    # Create new config file
    cat > "$CONFIG_FILE" <<EOF
# RetroArch config for Magic Dingus Box (RetroPie cores)
libretro_directory = "$LIBRETRO_DIRS"
input_autodetect_enable = "true"
input_joypad_driver = "udev"
menu_driver = "ozone"
EOF
    echo "   Created RetroArch config: $CONFIG_FILE"
fi

# Ensure config file is owned by the user
if [ "$EUID" -eq 0 ] && [ -n "$PRIMARY_USER" ]; then
    chown "$PRIMARY_USER:$PRIMARY_USER" "$CONFIG_FILE" 2>/dev/null || true
fi

echo ""
echo "=== Installation Complete ==="
echo ""
echo "Installed cores:"
for core in "${INSTALLED_CORES[@]}"; do
    echo "  ✓ $core"
done

if [ ${#FAILED_CORES[@]} -gt 0 ]; then
    echo ""
    echo "Failed to install:"
    for core in "${FAILED_CORES[@]}"; do
        echo "  ✗ $core"
    done
fi

echo ""
echo "Core directories configured:"
echo "  $LIBRETRO_DIRS"
echo ""
echo "You can now launch games from Magic Dingus Box!"
echo "Test with: retroarch --list-cores"

