#!/bin/bash
# Set CPU governor to performance mode for better video playback

for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    if [ -f "$cpu" ]; then
        echo performance > "$cpu" 2>/dev/null || true
    fi
done

echo "CPU governor set to performance mode"
