#!/bin/bash
# Resolve the PulseAudio sink name for a desired audio output.
#
# Reads `pactl list short sinks` output on stdin and prints the chosen
# sink name. Replaces the old hardcoded Pi-4 sink addresses
# (platform-fef00700.hdmi / platform-fe00b840.mailbox) so the same logic
# works on Pi 4, Pi 5 (different platform addresses, no 3.5mm jack), and
# USB DACs / I2S HATs. Mirrors platform::resolve_sink() in
# src/platform/platform_profile.cpp — keep the two in sync.
#
# Usage: pactl list short sinks | resolve_audio_sink.sh <hdmi|headphone|auto>
# Exit:  0 with sink name on stdout, 1 if no usable sink found.

set -euo pipefail

WANT="${1:-auto}"

SINKS="$(cat)"

# Sink name is the second tab-separated field.
hdmi_sink() {
    awk -F'\t' 'tolower($2) ~ /hdmi/ { print $2; exit }' <<< "$SINKS"
}

# Analog = Pi 4 3.5mm "mailbox" sink, or USB DAC / I2S HAT "analog"
# sinks. Never HDMI.
analog_sink() {
    awk -F'\t' 'tolower($2) !~ /hdmi/ && tolower($2) ~ /mailbox|analog/ { print $2; exit }' <<< "$SINKS"
}

case "$WANT" in
    headphone)
        RESULT="$(analog_sink)"
        [ -n "$RESULT" ] || RESULT="$(hdmi_sink)"
        ;;
    hdmi|auto|*)
        RESULT="$(hdmi_sink)"
        [ -n "$RESULT" ] || RESULT="$(analog_sink)"
        ;;
esac

if [ -z "$RESULT" ]; then
    exit 1
fi
printf '%s\n' "$RESULT"
