#!/bin/bash
# Diagnose why RetroArch online updater isn't working

set -e

echo "=== RetroArch Online Updater Diagnosis ==="
echo ""

CONFIG_FILE="$HOME/.config/retroarch/retroarch.cfg"
INFO_DIR="$HOME/.config/retroarch/info"

echo "1. Checking RetroArch configuration..."
if [ -f "$CONFIG_FILE" ]; then
    echo "   ✓ Config file exists"
    
    # Check core updater URLs
    if grep -q "core_updater_buildbot_cores_url" "$CONFIG_FILE"; then
        CORES_URL=$(grep "core_updater_buildbot_cores_url" "$CONFIG_FILE" | cut -d'"' -f2)
        echo "   ✓ Cores URL: $CORES_URL"
    else
        echo "   ✗ Cores URL not set"
    fi
    
    if grep -q "core_updater_buildbot_info_url" "$CONFIG_FILE"; then
        INFO_URL=$(grep "core_updater_buildbot_info_url" "$CONFIG_FILE" | cut -d'"' -f2)
        echo "   ✓ Info URL: $INFO_URL"
    else
        echo "   ✗ Info URL not set"
    fi
    
    if grep -q "info_directory" "$CONFIG_FILE"; then
        INFO_DIR_CFG=$(grep "info_directory" "$CONFIG_FILE" | cut -d'"' -f2)
        echo "   ✓ Info directory: $INFO_DIR_CFG"
    else
        echo "   ✗ Info directory not set"
    fi
else
    echo "   ✗ Config file not found"
fi

echo ""
echo "2. Checking info files..."
if [ -d "$INFO_DIR" ]; then
    INFO_COUNT=$(ls -1 "$INFO_DIR"/*.info 2>/dev/null | wc -l)
    echo "   ✓ Info directory exists"
    echo "   ✓ Found $INFO_COUNT info files"
    
    # Check a few specific cores
    for core in fceumm_libretro parallel_n64_libretro pcsx_rearmed_libretro; do
        if [ -f "$INFO_DIR/${core}.info" ]; then
            echo "   ✓ $core.info exists"
        else
            echo "   ✗ $core.info missing"
        fi
    done
else
    echo "   ✗ Info directory not found"
fi

echo ""
echo "3. Testing network connectivity..."
if ping -c 1 -W 5 buildbot.libretro.com >/dev/null 2>&1; then
    echo "   ✓ Can reach buildbot.libretro.com"
else
    echo "   ✗ Cannot reach buildbot.libretro.com"
fi

echo ""
echo "4. Testing buildbot URLs..."
CORES_URL=$(grep "core_updater_buildbot_cores_url" "$CONFIG_FILE" 2>/dev/null | cut -d'"' -f2 || echo "")
if [ -n "$CORES_URL" ]; then
    HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "$CORES_URL" 2>/dev/null || echo "000")
    if [ "$HTTP_CODE" = "200" ] || [ "$HTTP_CODE" = "301" ] || [ "$HTTP_CODE" = "302" ]; then
        echo "   ✓ Cores URL accessible (HTTP $HTTP_CODE)"
    else
        echo "   ✗ Cores URL not accessible (HTTP $HTTP_CODE)"
        echo "     This is likely why Core Downloader shows no entries!"
    fi
fi

INFO_URL=$(grep "core_updater_buildbot_info_url" "$CONFIG_FILE" 2>/dev/null | cut -d'"' -f2 || echo "")
if [ -n "$INFO_URL" ]; then
    HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "$INFO_URL" 2>/dev/null || echo "000")
    if [ "$HTTP_CODE" = "200" ]; then
        echo "   ✓ Info URL accessible (HTTP $HTTP_CODE)"
    else
        echo "   ✗ Info URL not accessible (HTTP $HTTP_CODE)"
    fi
fi

echo ""
echo "5. Checking RetroArch version..."
if command -v retroarch >/dev/null 2>&1; then
    VERSION=$(retroarch --version 2>&1 | head -1)
    echo "   ✓ $VERSION"
else
    echo "   ✗ RetroArch not found"
fi

echo ""
echo "6. Checking architecture..."
ARCH=$(uname -m)
echo "   Architecture: $ARCH"
if [ "$ARCH" = "aarch64" ]; then
    echo "   ⚠ WARNING: RetroArch buildbot may not have cores for aarch64"
    echo "   This is why Core Downloader shows no entries!"
fi

echo ""
echo "=== Diagnosis Complete ==="
echo ""
echo "Root Cause:"
echo "  RetroArch's Core Downloader queries the buildbot server for available cores."
echo "  If the server doesn't have cores for your architecture (aarch64), it shows nothing."
echo ""
echo "Solutions:"
echo "  1. Use system-installed cores (already available)"
echo "  2. Build cores from source"
echo "  3. Download cores manually and place in ~/.config/retroarch/cores/"
echo "  4. Use RetroPie repositories (if available)"

