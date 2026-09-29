#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# kiosk_standby_watcher.sh resolves the 40-pin header's gpiochip by LABEL,
# mirroring PlatformProfile::gpiochip_labels / gpio_manager.cpp, instead of
# hardcoding gpiochip0 (the header was gpiochip4 on early Pi 5 kernels).
# The script is sourceable: main() only runs when executed directly.

WATCHER="$CPP_DIR/scripts/kiosk_standby_watcher.sh"

pick() {
    # $1 = gpiodetect output
    bash -c 'source "$1"; printf "%s" "$2" | pick_header_chip' _ "$WATCHER" "$1"
}

@test "watcher no longer hardcodes GPIO_CHIP=gpiochip0" {
    run grep -nE '^GPIO_CHIP=gpiochip0' "$WATCHER"
    [ "$status" -ne 0 ]
}

@test "sourcing the watcher does not start the event loop" {
    run bash -c 'source "$1"; echo sourced-ok' _ "$WATCHER"
    [ "$status" -eq 0 ]
    [ "$output" = "sourced-ok" ]
}

@test "Pi 4B: pinctrl-bcm2711 on gpiochip0" {
    run pick 'gpiochip0 [pinctrl-bcm2711] (58 lines)
gpiochip1 [raspberrypi-exp-gpio] (8 lines)'
    [ "$status" -eq 0 ]
    [ "$output" = "gpiochip0" ]
}

@test "Pi 5 early kernel: pinctrl-rp1 on gpiochip4" {
    run pick 'gpiochip0 [gpio-brcmstb@107d508500] (32 lines)
gpiochip1 [gpio-brcmstb@107d508520] (4 lines)
gpiochip2 [gpio-brcmstb@107d517c00] (17 lines)
gpiochip3 [gpio-brcmstb@107d517c20] (6 lines)
gpiochip4 [pinctrl-rp1] (54 lines)'
    [ "$status" -eq 0 ]
    [ "$output" = "gpiochip4" ]
}

@test "Pi 5 kernel >= 6.6.47: pinctrl-rp1 back on gpiochip0" {
    run pick 'gpiochip0 [pinctrl-rp1] (54 lines)
gpiochip10 [gpio-brcmstb@107d508500] (32 lines)'
    [ "$status" -eq 0 ]
    [ "$output" = "gpiochip0" ]
}

@test "rp1 preferred over bcm2835 when both present" {
    run pick 'gpiochip0 [pinctrl-bcm2835] (54 lines)
gpiochip7 [pinctrl-rp1] (54 lines)'
    [ "$output" = "gpiochip7" ]
}

@test "label must match exactly, not as a substring" {
    run pick 'gpiochip3 [pinctrl-rp1-extra] (4 lines)'
    [ "$output" = "gpiochip0" ]
}

@test "no gpiodetect output falls back to gpiochip0" {
    run pick ''
    [ "$status" -eq 0 ]
    [ "$output" = "gpiochip0" ]
}

@test "resolve_gpio_chip survives a failing gpiodetect under set -euo pipefail" {
    run bash -c 'source "$1"; set -euo pipefail; gpiodetect() { return 127; }; chip="$(resolve_gpio_chip)"; echo "$chip"' _ "$WATCHER"
    [ "$status" -eq 0 ]
    [ "$output" = "gpiochip0" ]
}
