#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# resolve_audio_sink.sh — picks the PulseAudio sink for a desired output
# from `pactl list short sinks` piped on stdin. Replaces the Pi-4-only
# hardcoded platform-fef00700/fe00b840 sink names so the same script
# works on Pi 4 (bcm2711 addresses), Pi 5 (different platform addresses,
# no 3.5mm jack), and with USB DACs.
#
# Usage: resolve_audio_sink.sh <hdmi|headphone|auto>  < pactl-output

RESOLVER="$CPP_DIR/scripts/resolve_audio_sink.sh"

PI4_SINKS='0	alsa_output.platform-fe00b840.mailbox.stereo-fallback	module-alsa-card.c	s16le 2ch 48000Hz	SUSPENDED
1	alsa_output.platform-fef00700.hdmi.hdmi-stereo	module-alsa-card.c	s16le 2ch 48000Hz	RUNNING'

PI5_SINKS='0	alsa_output.platform-107c701400.hdmi.hdmi-stereo	module-alsa-card.c	s16le 2ch 48000Hz	IDLE'

PI5_USB_DAC_SINKS='0	alsa_output.platform-107c701400.hdmi.hdmi-stereo	module-alsa-card.c	s16le 2ch 48000Hz	IDLE
1	alsa_output.usb-C-Media_Electronics_Inc._USB_Audio_Device-00.analog-stereo	module-alsa-card.c	s16le 2ch 44100Hz	SUSPENDED'

@test "resolver script exists and is executable" {
    [ -x "$RESOLVER" ]
}

@test "shellcheck clean: resolve_audio_sink.sh" {
    command -v shellcheck >/dev/null 2>&1 || skip "shellcheck not installed"
    run shellcheck -S error "$RESOLVER"
    [ "$status" -eq 0 ]
}

@test "hdmi request resolves Pi 4 HDMI sink" {
    run bash -c "printf '%s\n' \"\$1\" | '$RESOLVER' hdmi" _ "$PI4_SINKS"
    [ "$status" -eq 0 ]
    [ "$output" = "alsa_output.platform-fef00700.hdmi.hdmi-stereo" ]
}

@test "hdmi request resolves Pi 5 HDMI sink despite different platform address" {
    run bash -c "printf '%s\n' \"\$1\" | '$RESOLVER' hdmi" _ "$PI5_SINKS"
    [ "$status" -eq 0 ]
    [ "$output" = "alsa_output.platform-107c701400.hdmi.hdmi-stereo" ]
}

@test "headphone request resolves Pi 4 3.5mm mailbox sink" {
    run bash -c "printf '%s\n' \"\$1\" | '$RESOLVER' headphone" _ "$PI4_SINKS"
    [ "$status" -eq 0 ]
    [ "$output" = "alsa_output.platform-fe00b840.mailbox.stereo-fallback" ]
}

@test "headphone request falls back to HDMI on Pi 5 (no analog jack)" {
    run bash -c "printf '%s\n' \"\$1\" | '$RESOLVER' headphone" _ "$PI5_SINKS"
    [ "$status" -eq 0 ]
    [ "$output" = "alsa_output.platform-107c701400.hdmi.hdmi-stereo" ]
}

@test "headphone request prefers USB DAC analog sink when present" {
    run bash -c "printf '%s\n' \"\$1\" | '$RESOLVER' headphone" _ "$PI5_USB_DAC_SINKS"
    [ "$status" -eq 0 ]
    [ "$output" = "alsa_output.usb-C-Media_Electronics_Inc._USB_Audio_Device-00.analog-stereo" ]
}

@test "auto request behaves like hdmi" {
    run bash -c "printf '%s\n' \"\$1\" | '$RESOLVER' auto" _ "$PI4_SINKS"
    [ "$status" -eq 0 ]
    [ "$output" = "alsa_output.platform-fef00700.hdmi.hdmi-stereo" ]
}

@test "no sinks at all fails with non-zero exit" {
    run bash -c ": | '$RESOLVER' hdmi"
    [ "$status" -ne 0 ]
}

@test "init_audio.sh no longer hardcodes Pi 4 platform sink addresses" {
    run grep -E 'platform-(fef00700|fe00b840)' "$CPP_DIR/scripts/init_audio.sh"
    [ "$status" -ne 0 ]
}

@test "init_audio.sh masks PipeWire units (stock Trixie ships PipeWire)" {
    grep -q "systemctl --global mask pipewire" "$CPP_DIR/scripts/init_audio.sh"
    grep -q "wireplumber.service" "$CPP_DIR/scripts/init_audio.sh"
}
