#!/usr/bin/env bash
#
# Magic Dingus Box - Kiosk Standby Watcher
#
# Monitors GPIO 3 (the front-panel toggle switch) and stops / starts
# the kiosk + companion services in response to switch position
# changes. Provides a "kiosk standby" mode without fully shutting
# down the Pi:
#
#   Switch ON  → GPIO 3 LOW  → kiosk + Content Manager + Docker stack
#                              are all RUNNING (~6-8W typical draw)
#   Switch OFF → GPIO 3 HIGH → all three services STOPPED, Pi remains
#                              powered up but idle (~3W with HDMI off,
#                              kernel level, ready to wake when switch
#                              flips back).
#
# Why not the gpio-shutdown overlay? That triggers `systemctl poweroff`
# on switch flip, requiring a hardware boot to recover. Kiosk-standby
# is faster (10s wake instead of 90s+) and avoids SD-card fsck cycles.
#
# Wiring (per the original power-switch design comment in early
# setup_boot_service.sh):
#   Toggle COM       -> GPIO 3
#   Toggle ON throw  -> GND   (closed circuit pulls GPIO 3 LOW)
#   Toggle OFF throw -> open  (pull-up resistor floats GPIO 3 HIGH)
#
# This script runs as a systemd long-running service. It does TWO
# things:
#   1. On startup, reads the current GPIO 3 state and reconciles
#      services to match (catches the case where the Pi was rebooted
#      with the switch in OFF position).
#   2. Enters an event loop with `gpiomon` (libgpiod) to react to
#      runtime transitions in real time.
#
# Idempotent: starting an already-running service or stopping an
# already-stopped service is a no-op for systemctl.

set -euo pipefail

GPIO_PIN=3
GPIO_CHIP=gpiochip0
LOG_TAG=kiosk-standby

# The kiosk is handled on its own, first on the way down and first on the
# way up (see stop_services / start_services for why). These two are the
# rest of the stack.
COMPANION_SERVICES=(
    magic-dingus-web.service
    magic-dingus-services.service
)

KIOSK_SERVICE=magic-dingus-box-cpp.service

# LED "powering down" sweep, shared with the restart button's in-app
# version. Absent on a board with no harness — every use is guarded.
SHUTDOWN_ANIM=/opt/magic_dingus_box/magic_dingus_box_cpp/scripts/led_shutdown_animation.sh
LED_PINS=(12 16 26 20)

log() {
    echo "[kiosk-standby] $1"
    logger -t "$LOG_TAG" "$1" 2>/dev/null || true
}

read_gpio() {
    # Returns "high" or "low".
    local raw
    raw=$(pinctrl get "$GPIO_PIN" 2>/dev/null) || true
    if echo "$raw" | grep -q '| hi'; then
        echo high
    elif echo "$raw" | grep -q '| lo'; then
        echo low
    else
        # Fallback: if pinctrl isn't available, return UNKNOWN and
        # let the caller decide. A failed read shouldn't accidentally
        # put us in standby; treat unknown as "running".
        echo unknown
    fi
}

all_leds_off() {
    for pin in "${LED_PINS[@]}"; do
        pinctrl set "$pin" op dl 2>/dev/null || true
    done
}

stop_services() {
    log "Switch OFF detected — stopping kiosk + companion services"

    # The kiosk goes down FIRST, alone, for two reasons pointing the same
    # way. It owns the four LED lines through libgpiod, and the sweep
    # below drives those same pins with pinctrl — running both at once is
    # two writers on one pin. And it is the thing the owner is looking
    # at, so killing it first is what makes the flip feel immediate.
    #
    # SIGTERM here is handled, not fatal: main.cpp flushes the watch
    # position (so flipping to standby part-way through an episode keeps
    # your place) and tears down GL/DRM cleanly.
    if systemctl is-active --quiet "$KIOSK_SERVICE"; then
        systemctl stop "$KIOSK_SERVICE" 2>&1 | sed 's/^/    /' || \
            log "WARN: failed to stop the kiosk"
    fi

    # The powering-down indication. Without it the screen simply goes
    # black, and a box working exactly as designed looks identical to one
    # that has crashed. Backgrounded under a hard timeout so a wedged
    # animation can never hold up standby itself: the LEDs are the
    # courtesy, the power saving is the point.
    if [[ -x "$SHUTDOWN_ANIM" ]]; then
        ( timeout 6 "$SHUTDOWN_ANIM" >/dev/null 2>&1 || true ) &
    fi

    for svc in "${COMPANION_SERVICES[@]}"; do
        if systemctl is-active --quiet "$svc"; then
            systemctl stop "$svc" 2>&1 | sed 's/^/    /' || \
                log "WARN: failed to stop $svc"
        fi
    done

    wait 2>/dev/null || true
    all_leds_off
    log "Standby state — services stopped, Pi idle (~3W)"
}

start_services() {
    log "Switch ON detected — starting kiosk + companion services"

    # Wake order is deliberately NOT the dependency order. The kiosk goes
    # first because it is the only part the owner can see; the Docker
    # stack needs 20-40s to bring six containers and a VPN tunnel back,
    # and making the picture wait on that turned "flip it back on" into a
    # long stare at a black TV.
    #
    # Starting out of order breaks nothing: the kiosk polls Radarr for
    # Media Browser availability and simply shows the tunnel-down state
    # until the containers answer — exactly what it already does on every
    # cold boot, where the same race exists.
    systemctl reset-failed "$KIOSK_SERVICE" 2>/dev/null || true
    systemctl start "$KIOSK_SERVICE" 2>&1 | sed 's/^/    /' || \
        log "WARN: failed to start the kiosk"

    for svc in "${COMPANION_SERVICES[@]}"; do
        systemctl reset-failed "$svc" 2>/dev/null || true
        # --no-block: queue it and move on. The Docker stack especially
        # must not hold this function, or a slow container start would
        # delay the watcher's reaction to the NEXT flip of the switch.
        systemctl --no-block start "$svc" 2>&1 | sed 's/^/    /' || \
            log "WARN: failed to queue $svc"
    done

    log "Running state — kiosk up; Content Manager + Docker starting behind it"
}

reconcile_initial_state() {
    # On watcher startup, read the current GPIO state for visibility.
    # We do NOT stop services on a HIGH read at boot — only act on
    # actual transitions during runtime (handled by the gpiomon event
    # loop below).
    #
    # Why the asymmetry: HIGH is ambiguous. It can mean either
    #   (a) switch wired and currently in OFF position, or
    #   (b) no switch wired at all — line floats HIGH via the kernel's
    #       default pull-up.
    #
    # The (b) case happens on every fresh clone of the golden image
    # before the operator finishes faceplate assembly. Treating HIGH-
    # at-boot as "switch OFF → stop services" leaves the cloned Pi
    # with a black screen and a "Failed to start magic-dingus-box-
    # cpp.service" line in the journal — the operator has no idea the
    # box actually works fine; they just have no switch to flip.
    #
    # Transitions are unambiguous: a HIGH→LOW or LOW→HIGH event during
    # runtime is a deliberate flip of a wired switch by the operator.
    # gpiomon catches those reliably and fires start_services /
    # stop_services accordingly. So the kiosk respects the switch
    # whenever it's actually wired up — we just don't try to GUESS at
    # boot when we can't tell wired-OFF from unwired-floating.
    #
    # LOW at boot is unambiguous (you can't read LOW without something
    # actively pulling it down, which means a wired switch is closed
    # = ON), so we still call start_services there. Idempotent — if
    # the kiosk is already running we just wake the watcher without
    # disturbing anything.
    local state
    state=$(read_gpio)
    log "Initial GPIO ${GPIO_PIN} state: ${state}"
    case "$state" in
        low)
            # Switch wired and ON (or someone is actively pulling
            # GPIO 3 to ground). Make sure services are running.
            log "  → start_services (switch ON, or runtime restart of watcher)"
            start_services
            ;;
        high|*)
            # Either: switch wired and OFF, switch not wired at all,
            # or GPIO read failed. All three cases collapse to
            # "leave services in their current state". The operator
            # uses the physical switch to put the box in standby
            # (LOW→HIGH transition fires stop_services).
            log "  → no boot-time action (transitions handled by event loop)"
            ;;
    esac
}

monitor_loop() {
    log "Entering gpiomon event loop on ${GPIO_CHIP} line ${GPIO_PIN}"

    # gpiomon (libgpiod v2 syntax) emits one line per event. Format:
    #   <ts> <chip>:<line> <event>
    # We read line-by-line and react to each.
    #
    # --edges=both: capture both rising and falling
    # --localtime: human-readable timestamp
    # The line is specified by offset because gpio-shutdown overlay
    # claims the named line; the offset still works.
    while IFS= read -r line; do
        log "GPIO event: ${line}"
        # libgpiod v2 prints "rising"/"falling" in the line
        case "$line" in
            *rising*|*RISING*)
                # Pin went LOW -> HIGH. With our wiring, that means
                # switch flipped from ON to OFF.
                stop_services
                ;;
            *falling*|*FALLING*)
                # Pin went HIGH -> LOW. Switch flipped from OFF to ON.
                start_services
                ;;
            *)
                log "Unrecognized event line: ${line}"
                ;;
        esac
    done < <(gpiomon --edges=both --chip "$GPIO_CHIP" "$GPIO_PIN" 2>&1)

    # If gpiomon exits unexpectedly, log and let systemd restart the
    # service (Restart=always in the unit file).
    log "gpiomon exited unexpectedly — service will restart"
    exit 1
}

# ---------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------
log "Starting kiosk-standby-watcher (PID=$$)"

reconcile_initial_state
monitor_loop
