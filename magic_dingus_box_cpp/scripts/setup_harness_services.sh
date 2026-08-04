#!/usr/bin/env bash
#
# Magic Dingus Box - Physical harness services setup (power switch,
# restart button, LED animations)
#
# Installs and enables the front-panel hardware services for harness
# units. Idempotent — safe to re-run any time. Run on the Pi:
#
#   ./scripts/setup_harness_services.sh
#
# What ships enabled (decided 2026-08-03, harness-SKU golden image):
#
#   kiosk-standby-watcher.service   GPIO 3 toggle switch → kiosk standby.
#                                   CLONE-SAFE BY DESIGN: it acts only on
#                                   runtime switch transitions and never
#                                   stops services on an ambiguous
#                                   HIGH-at-boot (unwired switch floats
#                                   HIGH — see reconcile_initial_state()
#                                   in kiosk_standby_watcher.sh).
#   led-boot-sequence.service       LED chase during boot; the kiosk stops
#                                   it when it takes the LED lines over
#                                   (gpio_manager.cpp).
#   led-shutdown-animation.service  LED sweep on halt/reboot.
#   GPIO 24 restart button          Owned by the KIOSK BINARY, not by a
#                                   device-tree overlay. Press -> LED
#                                   shutdown sweep -> `systemctl restart
#                                   magic-dingus-box-cpp` -> intro video ->
#                                   main menu, ~10s. This script's job for
#                                   that pin is to make sure NO overlay
#                                   claims it (see Step 3).
#
# What is installed but deliberately NOT enabled:
#
#   power-switch-check.service      Early-boot halt-if-switch-OFF. It
#                                   contradicts the watcher's clone-safety:
#                                   it halts on the same ambiguous HIGH the
#                                   watcher refuses to act on, which would
#                                   power off every not-yet-assembled unit
#                                   at first boot (and on Pi 5 it can't read
#                                   the pin that early anyway — unconfigured
#                                   RP1 pins report no level). Enable
#                                   per-unit only if strict
#                                   halt-when-switch-off-at-boot semantics
#                                   are ever wanted on a fully-wired box.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SYSTEMD_SRC="${SCRIPT_DIR}/../systemd"

echo "=== Magic Dingus Box harness services setup ==="

# ---------------------------------------------------------------------------
# Step 1: Install unit files
# ---------------------------------------------------------------------------
UNITS=(
    kiosk-standby-watcher.service
    led-boot-sequence.service
    led-shutdown-animation.service
    power-switch-check.service
)
for u in "${UNITS[@]}"; do
    if [ ! -f "${SYSTEMD_SRC}/${u}" ]; then
        echo "ERROR: ${SYSTEMD_SRC}/${u} not found — run from a synced checkout" >&2
        exit 1
    fi
    sudo install -m 0644 "${SYSTEMD_SRC}/${u}" "/etc/systemd/system/${u}"
done
sudo systemctl daemon-reload
echo "Installed ${#UNITS[@]} unit files."

# ---------------------------------------------------------------------------
# Step 2: Remove the retired gpio-shutdown overlay if present
# ---------------------------------------------------------------------------
# Early revisions (setup_boot_service.sh Step 3.5) put
# `dtoverlay=gpio-shutdown,gpio_pin=3,...` in config.txt: flip the switch
# OFF and the Pi POWERS OFF. The standby watcher replaced that design
# (standby wakes in ~10s; poweroff needs a 90s+ boot and an fsck cycle),
# and both acting on GPIO 3 at once means the poweroff always wins. Any
# box that ever ran the old setup still carries the line — strip it.
BOOT_CONFIG="/boot/firmware/config.txt"
[ -f "$BOOT_CONFIG" ] || BOOT_CONFIG="/boot/config.txt"

if [ -f "$BOOT_CONFIG" ] && grep -q '^dtoverlay=gpio-shutdown' "$BOOT_CONFIG"; then
    sudo sed -i '/^dtoverlay=gpio-shutdown/d; /^# GPIO3 power switch (Magic Dingus Box)/d; /^# ON position = GPIO3 LOW/d' "$BOOT_CONFIG"
    echo "Removed retired gpio-shutdown overlay from ${BOOT_CONFIG} (reboot to apply)."
fi

# ---------------------------------------------------------------------------
# Step 3: Make sure NOTHING in the device tree claims GPIO 24
# ---------------------------------------------------------------------------
# The restart button is handled by the kiosk binary (GpioManager), which
# restarts the SERVICE — intro video back to the main menu in about ten
# seconds. A gpio-key overlay was tried here on 2026-08-03 to get a clean
# full reboot via logind instead; it worked, and was rejected on the
# hardware for being a 90-second cold boot when the owner just wants the
# UI back at the start.
#
# It must be actively removed, not merely "not added". Two reasons, and
# the second one cost a dead harness:
#   1. It reboots the box, which is the behavior we are undoing.
#   2. libgpiod requests every kiosk input line in ONE bulk call. With the
#      kernel holding GPIO 24, that whole request fails — all four
#      buttons, the encoder switch, and all four LEDs go dead together,
#      explained only by "Error: Could not request input GPIO lines".
if [ -f "$BOOT_CONFIG" ] && grep -q 'gpio-key.*gpio=24' "$BOOT_CONFIG"; then
    sudo sed -i '/dtoverlay=gpio-key.*gpio=24/d' "$BOOT_CONFIG"
    sudo sed -i '/# GPIO 24 front-panel restart button/d; /# Press = KEY_RESTART/d; /# scripts\/setup_harness_services.sh for the poweroff/d' "$BOOT_CONFIG"
    echo "Removed the GPIO 24 gpio-key overlay from ${BOOT_CONFIG} — REBOOT REQUIRED"
    echo "  (until you reboot, the kernel still owns pin 24 and the kiosk's"
    echo "   buttons/LEDs will stay dead)"
else
    echo "GPIO 24 is free of device-tree overlays (correct)."
fi

# ---------------------------------------------------------------------------
# Step 4: Enable the harness services (watcher + LEDs; never the boot check)
# ---------------------------------------------------------------------------
sudo systemctl enable led-boot-sequence.service led-shutdown-animation.service >/dev/null 2>&1
sudo systemctl enable --now kiosk-standby-watcher.service
echo "Enabled: kiosk-standby-watcher (started), led-boot-sequence, led-shutdown-animation."
echo "power-switch-check installed but NOT enabled (see header for why)."

# ---------------------------------------------------------------------------
# Step 5: Report
# ---------------------------------------------------------------------------
sleep 1
echo ""
echo "GPIO 3 now: $(pinctrl get 3 2>/dev/null || echo 'unreadable')"
echo "  (lo = switch wired + ON; hi = OFF or unwired; watcher acts on flips only)"
systemctl is-active kiosk-standby-watcher.service >/dev/null 2>&1 \
    && echo "kiosk-standby-watcher: active" \
    || echo "WARN: kiosk-standby-watcher not active — journalctl -u kiosk-standby-watcher"
echo ""
echo "Hardware acceptance:"
echo "  switch OFF -> LED sweep, screen dark in ~5s, Pi stays powered"
echo "  switch ON  -> intro video in ~10s (Docker finishes behind it)"
echo "  restart button -> LED sweep, then intro video + main menu in ~10s"
echo "                    (the SERVICE restarts; the machine does not reboot)"
