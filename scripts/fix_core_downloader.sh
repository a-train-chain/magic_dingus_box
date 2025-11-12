#!/bin/bash
# Fix RetroArch Core Downloader showing "no Entries in list"
# This script addresses common issues with RetroArch 1.20.0 on Raspberry Pi

set -e

CONFIG_FILE="$HOME/.config/retroarch/retroarch.cfg"
INFO_DIR="$HOME/.config/retroarch/info"

echo "Fixing RetroArch Core Downloader configuration..."

# Backup config
if [ -f "$CONFIG_FILE" ]; then
    cp "$CONFIG_FILE" "${CONFIG_FILE}.backup.$(date +%Y%m%d_%H%M%S)"
fi

# Try different cores URL formats that RetroArch 1.20.0 might accept
# Some versions need the full path, others need just the base URL

# Remove existing cores URL settings
sed -i '/^core_updater_buildbot_cores_url/d' "$CONFIG_FILE"

# Try the standard format first (with /latest)
echo "core_updater_buildbot_cores_url = \"https://buildbot.libretro.com/nightly/linux/aarch64/latest\"" >> "$CONFIG_FILE"

# Also ensure these critical settings are present
sed -i '/^core_updater_buildbot_info_url/d' "$CONFIG_FILE"
echo "core_updater_buildbot_info_url = \"https://buildbot.libretro.com/assets/frontend/info.zip\"" >> "$CONFIG_FILE"

sed -i '/^core_updater_auto_extract_archive/d' "$CONFIG_FILE"
echo "core_updater_auto_extract_archive = \"true\"" >> "$CONFIG_FILE"

sed -i '/^core_updater_show_experimental_cores/d' "$CONFIG_FILE"
echo "core_updater_show_experimental_cores = \"true\"" >> "$CONFIG_FILE"

# Ensure info directory is set correctly
sed -i '/^info_directory/d' "$CONFIG_FILE"
echo "info_directory = \"$INFO_DIR\"" >> "$CONFIG_FILE"

# Verify info files exist
INFO_COUNT=$(ls -1 "$INFO_DIR"/*.info 2>/dev/null | wc -l)
echo "Found $INFO_COUNT core info files"

if [ "$INFO_COUNT" -eq 0 ]; then
    echo "ERROR: No core info files found in $INFO_DIR"
    echo "Run: ./scripts/fix_retroarch_core_info.sh first"
    exit 1
fi

# Check if RetroArch can read the info files
echo "Testing info file readability..."
if [ -r "$INFO_DIR/fceumm_libretro.info" ]; then
    echo "✓ Info files are readable"
else
    echo "WARNING: Info files may not be readable"
    chmod 644 "$INFO_DIR"/*.info 2>/dev/null || true
fi

echo ""
echo "Configuration updated. Please:"
echo "1. Close RetroArch completely (if running)"
echo "2. Restart RetroArch: retroarch"
echo "3. Go to: Online Updater -> Update Core Info Files (if needed)"
echo "4. Go to: Online Updater -> Core Downloader"
echo ""
echo "If Core Downloader still shows 'no Entries', try:"
echo "  - Restart RetroArch again"
echo "  - Check network connectivity from RetroArch"
echo "  - Use manual download: ./scripts/download_cores_manual.sh"

