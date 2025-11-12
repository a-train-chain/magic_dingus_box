#!/bin/bash
# Check RetroArch audio logs and configuration
# Run this on the Pi to diagnose audio issues

set -e

LOG_FILE="/tmp/magic_retroarch_launch.log"
CONFIG_FILE="${HOME}/.config/retroarch/retroarch.cfg"
RETROARCH_LOG="${HOME}/.config/retroarch/retroarch.log"

echo "=== RetroArch Audio Diagnostic ==="
echo ""

# Check if log file exists
if [ -f "$LOG_FILE" ]; then
    echo "1. Recent log entries (last 100 lines):"
    echo "----------------------------------------"
    tail -100 "$LOG_FILE"
    echo ""
    
    echo "2. Audio-related log entries:"
    echo "----------------------------------------"
    grep -iE "(audio|device|hdmi|alsa|pulse|driver)" "$LOG_FILE" | tail -30 || echo "  No audio-related log entries found"
    echo ""
    
    echo "3. Audio configuration from launch log:"
    echo "----------------------------------------"
    grep -i "Configuring audio\|audio_driver\|audio_device\|Audio configuration complete" "$LOG_FILE" | tail -10 || echo "  No audio configuration found in log"
    echo ""
    
    echo "4. Any errors in log:"
    echo "----------------------------------------"
    grep -iE "(error|fail|warn|cannot|unable)" "$LOG_FILE" | tail -20 || echo "  No errors found"
    echo ""
else
    echo "Launch log file not found: $LOG_FILE"
    echo "This might mean RetroArch hasn't been launched yet, or log was cleared."
    echo ""
fi

# Check RetroArch's own log file
if [ -f "$RETROARCH_LOG" ]; then
    echo "5. RetroArch internal log (audio-related):"
    echo "----------------------------------------"
    grep -iE "(audio|alsa|pulse|device|driver)" "$RETROARCH_LOG" | tail -20 || echo "  No audio entries in RetroArch log"
    echo ""
    
    echo "6. RetroArch log errors:"
    echo "----------------------------------------"
    grep -iE "(error|fail|warn)" "$RETROARCH_LOG" | tail -10 || echo "  No errors in RetroArch log"
    echo ""
fi

# Check current RetroArch config
if [ -f "$CONFIG_FILE" ]; then
    echo "7. Current RetroArch audio settings:"
    echo "----------------------------------------"
    grep "^audio_" "$CONFIG_FILE" | grep -v "^#" || echo "  No audio settings found in config"
    echo ""
    
    echo "8. Menu toggle and hotkey settings:"
    echo "----------------------------------------"
    grep -E "input_menu_toggle|input_menu_toggle_gamepad|input_enable_hotkey" "$CONFIG_FILE" | grep -v "^#" || echo "  No menu toggle settings found"
    echo ""
    
    echo "9. Controller input settings:"
    echo "----------------------------------------"
    grep -E "input_autodetect|input_joypad_driver|input_player1" "$CONFIG_FILE" | grep -v "^#" | head -10 || echo "  No controller settings found"
    echo ""
else
    echo "RetroArch config file not found: $CONFIG_FILE"
    echo ""
fi

# Check system audio
echo "10. System audio status:"
echo "----------------------------------------"
if command -v aplay >/dev/null 2>&1; then
    echo "Available ALSA devices:"
    aplay -l 2>&1 || echo "  ERROR: aplay failed"
else
    echo "  aplay not found"
fi
echo ""

# Check PulseAudio
CURRENT_USER="${USER:-$(whoami)}"
if command -v pulseaudio >/dev/null 2>&1 && pgrep -u "$CURRENT_USER" pulseaudio >/dev/null 2>&1; then
    echo "PulseAudio is running. Sinks:"
    pactl list short sinks 2>&1 || echo "  ERROR: Failed to list sinks"
else
    echo "PulseAudio is not running"
fi
echo ""

# Check system volume
echo "11. System volume levels:"
echo "----------------------------------------"
if command -v amixer >/dev/null 2>&1; then
    amixer get Master 2>&1 || echo "  ERROR: Failed to get Master volume"
    echo ""
    amixer get PCM 2>&1 || echo "  ERROR: Failed to get PCM volume"
else
    echo "  amixer not found"
fi
echo ""

echo "=== Diagnostic Complete ==="
echo ""
echo "Key things to check:"
echo "  1. Audio driver should be 'alsa' or 'pulse'"
echo "  2. Audio device should show HDMI device (e.g., 'alsa/hdmi:CARD=...')"
echo "  3. audio_enable should be 'true'"
echo "  4. audio_mute_enable should be 'false'"
echo "  5. audio_volume should be '1.0'"
echo "  6. input_menu_toggle_gamepad_combo should be '2+3' (Select+Start)"
echo "  7. input_enable_hotkey should be 'true'"
echo ""
echo "If audio is still not working, try:"
echo "  1. Run: ./scripts/fix_retroarch_audio.sh"
echo "  2. Check if RetroArch is using the correct audio device"
echo "  3. Verify system volume is not muted: amixer set Master unmute && amixer set Master 100%"
echo "  4. Test system audio: speaker-test -t sine -f 1000 -c 2"
echo ""
echo "If menu combo doesn't work:"
echo "  1. Check controller is detected: jstest /dev/input/js0"
echo "  2. Try different combo: Some controllers use different button numbers"
echo "  3. Check RetroArch config for input_menu_toggle_gamepad_combo"

