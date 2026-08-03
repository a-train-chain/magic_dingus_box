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
#   GPIO 24 restart button          dtoverlay=gpio-key emitting KEY_RESTART;
#                                   systemd-logind's HandleRebootKey (default
#                                   `reboot`) performs a clean reboot. Zero
#                                   daemons; works even when the kiosk is
#                                   down — which is exactly when a physical
#                                   restart control earns its keep. The
#                                   kiosk never swallows it: InputManager
#                                   only opens/grabs EV_ABS (joystick-like)
#                                   devices, and a gpio-key device is
#                                   EV_KEY-only. (The wiring design doc
#                                   called this pin "shutdown button"; the
#                                   product decision 2026-08-03 is RESTART.
#                                   To make it power off instead, change
#                                   keycode=408 to keycode=116 (KEY_POWER)
#                                   below and logind's HandlePowerKey
#                                   default `poweroff` takes over.)
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
# Step 3: Restart button overlay (GPIO 24, momentary, active-low)
# ---------------------------------------------------------------------------
# keycode 408 = KEY_RESTART (input-event-codes.h 0x198). udev tags
# gpio-keys devices as power-switch; logind picks the key up with no
# per-unit config. Appended under an explicit [all] header: config.txt
# ends with [pi4]/[pi5] conditional sections, and a bare append would
# land inside whichever section is last and silently apply to one board
# only (dual-board contract rule 5).
if [ -f "$BOOT_CONFIG" ] && ! grep -q 'gpio-key,gpio=24' "$BOOT_CONFIG"; then
    sudo tee -a "$BOOT_CONFIG" >/dev/null <<'EOF'

[all]
# GPIO 24 front-panel restart button (Magic Dingus Box harness).
# Press = KEY_RESTART -> systemd-logind clean reboot. See
# scripts/setup_harness_services.sh for the poweroff alternative.
dtoverlay=gpio-key,gpio=24,active_low=1,gpio_pull=up,keycode=408,label=mdb-restart
EOF
    echo "Added GPIO 24 restart-button overlay to ${BOOT_CONFIG} (reboot to apply)."
else
    echo "Restart-button overlay already present (or no config.txt)."
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
echo "Hardware acceptance: flip switch OFF (kiosk stops ~5s) -> ON (kiosk"
echo "returns ~10s); press restart button (clean reboot; LED sweep plays)."
echo "Restart-button overlay requires one reboot before its first use."
