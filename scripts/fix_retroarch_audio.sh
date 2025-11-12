#!/bin/bash
# Direct script to fix RetroArch audio configuration
# Run this on the Pi to fix audio issues

set -e

CONFIG_DIR="${HOME}/.config/retroarch"
CONFIG_FILE="${CONFIG_DIR}/retroarch.cfg"

echo "=== Fixing RetroArch Audio Configuration ==="
echo ""

# Create config directory if needed
mkdir -p "$CONFIG_DIR"

# Backup existing config
if [ -f "$CONFIG_FILE" ]; then
    BACKUP="${CONFIG_FILE}.bak.$(date +%Y%m%d_%H%M%S)"
    cp "$CONFIG_FILE" "$BACKUP"
    echo "Backed up config to: $BACKUP"
fi

# Detect audio driver
AUDIO_DRIVER="alsa"
CURRENT_USER="${USER:-$(whoami)}"
if command -v pulseaudio >/dev/null 2>&1 && pgrep -u "$CURRENT_USER" pulseaudio >/dev/null 2>&1; then
    AUDIO_DRIVER="pulse"
    echo "Using PulseAudio driver"
else
    echo "Using ALSA driver"
fi

# Detect HDMI audio device
HDMI_AUDIO_DEVICE=""
if [ "$AUDIO_DRIVER" = "pulse" ]; then
    if command -v pactl >/dev/null 2>&1; then
        HDMI_SINK=$(pactl list short sinks 2>/dev/null | grep -i hdmi | head -1 | awk '{print $2}')
        if [ -n "$HDMI_SINK" ]; then
            HDMI_AUDIO_DEVICE="$HDMI_SINK"
            echo "Found PulseAudio HDMI sink: $HDMI_AUDIO_DEVICE"
        else
            DEFAULT_SINK=$(pactl info 2>/dev/null | grep "Default Sink:" | cut -d' ' -f3)
            if [ -n "$DEFAULT_SINK" ]; then
                HDMI_AUDIO_DEVICE="$DEFAULT_SINK"
                echo "Using default PulseAudio sink: $HDMI_AUDIO_DEVICE"
            fi
        fi
    fi
else
    # ALSA detection
    if command -v aplay >/dev/null 2>&1; then
        echo "Detecting ALSA devices..."
        aplay -l
        HDMI_CARD_DEV=$(aplay -l 2>/dev/null | grep -i "hdmi\|vc4hdmi" | head -1 | sed -n 's/.*card \([0-9]*\):.*device \([0-9]*\):.*/\1,\2/p')
        if [ -n "$HDMI_CARD_DEV" ]; then
            CARD=$(echo "$HDMI_CARD_DEV" | cut -d',' -f1)
            DEV=$(echo "$HDMI_CARD_DEV" | cut -d',' -f2)
            CARD_NAME=$(aplay -l 2>/dev/null | grep "^card $CARD:" | sed 's/^card [0-9]*: \([^,]*\).*/\1/' | tr -d ' ' | tr '[:upper:]' '[:lower:]')
            if [ -n "$CARD_NAME" ]; then
                HDMI_AUDIO_DEVICE="alsa/hdmi:CARD=${CARD_NAME},DEV=${DEV}"
                echo "Found ALSA HDMI device: $HDMI_AUDIO_DEVICE"
            fi
        fi
    fi
    
    # Fallback
    if [ -z "$HDMI_AUDIO_DEVICE" ]; then
        if [ -f /proc/asound/cards ]; then
            HDMI_CARD=$(grep -i "vc4hdmi\|bcm2835\|hdmi" /proc/asound/cards | head -1 | awk '{print $1}' | tr -d ':')
            if [ -n "$HDMI_CARD" ]; then
                CARD_NAME=$(grep "^ *${HDMI_CARD} " /proc/asound/cards | sed 's/.*\[\(.*\)\].*/\1/' | tr '[:upper:]' '[:lower:]' | tr -d ' ')
                if [ -z "$CARD_NAME" ]; then
                    CARD_NAME="vc4hdmi${HDMI_CARD}"
                fi
                HDMI_AUDIO_DEVICE="alsa/hdmi:CARD=${CARD_NAME},DEV=0"
                echo "Using fallback ALSA device: $HDMI_AUDIO_DEVICE"
            fi
        fi
    fi
    
    # Final fallback
    if [ -z "$HDMI_AUDIO_DEVICE" ]; then
        HDMI_AUDIO_DEVICE="alsa/hdmi:CARD=vc4hdmi0,DEV=0"
        echo "Using default ALSA device: $HDMI_AUDIO_DEVICE"
    fi
fi

echo ""
echo "Applying audio settings to RetroArch config..."

# Remove existing audio settings
sed -i '/^audio_driver/d' "$CONFIG_FILE" 2>/dev/null || true
sed -i '/^audio_device/d' "$CONFIG_FILE" 2>/dev/null || true
sed -i '/^audio_enable/d' "$CONFIG_FILE" 2>/dev/null || true
sed -i '/^audio_mute_enable/d' "$CONFIG_FILE" 2>/dev/null || true
sed -i '/^audio_volume/d' "$CONFIG_FILE" 2>/dev/null || true
sed -i '/^audio_latency/d' "$CONFIG_FILE" 2>/dev/null || true
sed -i '/^audio_resampler/d' "$CONFIG_FILE" 2>/dev/null || true
sed -i '/^audio_out_rate/d' "$CONFIG_FILE" 2>/dev/null || true
sed -i '/^audio_sync/d' "$CONFIG_FILE" 2>/dev/null || true

# Add new audio settings
cat >> "$CONFIG_FILE" <<EOF

# Audio settings - configured by fix_retroarch_audio.sh
audio_driver = "$AUDIO_DRIVER"
EOF

if [ -n "$HDMI_AUDIO_DEVICE" ]; then
    echo "audio_device = \"$HDMI_AUDIO_DEVICE\"" >> "$CONFIG_FILE"
fi

cat >> "$CONFIG_FILE" <<EOF
audio_enable = "true"
audio_mute_enable = "false"
audio_volume = "1.0"
audio_latency = "32"
audio_resampler = "sinc"
audio_out_rate = "48000"
audio_sync = "true"
EOF

echo ""
echo "Audio configuration applied!"
echo ""
echo "Current audio settings:"
grep "^audio_" "$CONFIG_FILE" | grep -v "^#" || echo "  (no audio settings found)"
echo ""
echo "To test:"
echo "  1. Launch a RetroArch game"
echo "  2. Press Select+Start to open menu"
echo "  3. Go to Settings > Audio"
echo "  4. Verify settings match above"
echo ""
echo "If audio still doesn't work, try:"
echo "  - Check system volume: amixer get Master"
echo "  - Test system audio: speaker-test -t sine -f 1000 -c 2"
echo "  - Check RetroArch log: tail -f /tmp/magic_retroarch_launch.log"

