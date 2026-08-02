#!/bin/bash
# Deploy RetroArch DRM fixes to Magic Dingus Box
# Run this on the Pi when it's back online

set -e

echo "=== Deploying RetroArch DRM Fixes ==="

# Copy updated launcher
echo "1. Copying updated retroarch_launcher.cpp..."
cp /opt/magic_dingus_box/magic_dingus_box_cpp/src/retroarch/retroarch_launcher.cpp.backup /opt/magic_dingus_box/magic_dingus_box_cpp/src/retroarch/retroarch_launcher.cpp.backup.bak 2>/dev/null || true
cp /opt/magic_dingus_box/magic_dingus_box_cpp/src/retroarch/retroarch_launcher.cpp /opt/magic_dingus_box/magic_dingus_box_cpp/src/retroarch/retroarch_launcher.cpp.backup

# Copy DRM utility
echo "2. Copying DRM master access utility..."
cp drm_drop_master.c /opt/magic_dingus_box/magic_dingus_box_cpp/

# Compile DRM utility
echo "3. Compiling DRM utility..."
cd /opt/magic_dingus_box/magic_dingus_box_cpp
gcc -o drm_drop_master drm_drop_master.c -ldrm

# Build and deploy
echo "4. Building updated application..."
cd build
# Same RAM budget as deploy_cpp.sh and update.sh: ~600 MB per cc1plus, and
# this runs on a box that is already hosting the kiosk and the container
# stack. -j4 on a 1.5 GB Pi 4B or a 2 GB Pi 5 goes into swap and risks the
# OOM killer taking the live kiosk instead of the compiler.
MEM_KB=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)
if [ "${MEM_KB}" -ge 4194304 ]; then
    make -j4
else
    make -j2
fi

# Copy to service location
echo "5. Deploying binary..."
cp /opt/magic_dingus_box/magic_dingus_box_cpp/build/magic_dingus_box_cpp /home/magic/magic_dingus_box/magic_dingus_box_cpp/build/magic_dingus_box_cpp

# Restart service
echo "6. Restarting service..."
sudo systemctl restart magic-dingus-box-cpp.service

echo ""
echo "=== Deployment Complete ==="
echo "✓ DRM master access drop utility compiled"
echo "✓ RetroArch launcher updated (direct launch, no systemd-run)"
echo "✓ Additional KMS video options added"
echo "✓ Service restarted"
echo ""
echo "Test by launching Super Mario Bros. 3 from the UI!"
echo "The DRM master access drop should allow RetroArch to take over the display."
