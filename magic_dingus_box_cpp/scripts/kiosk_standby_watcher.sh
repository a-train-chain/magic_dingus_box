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

# Services controlled by switch position.
# Order matters on stop: kiosk first (it depends on services + web
# admin), then content manager, then docker stack. Reverse on start.
SERVICES_STOP_ORDER=(
    magic-dingus-box-cpp.service
    magic-dingus-web.service
    magic-dingus-services.service
)
SERVICES_START_ORDER=(
    magic-dingus-services.service
    magic-dingus-web.service
    magic-dingus-box-cpp.service
)

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

stop_services() {
    log "Switch OFF detected — stopping kiosk + companion services"
    for svc in "${SERVICES_STOP_ORDER[@]}"; do
        if systemctl is-active --quiet "$svc"; then
            systemctl stop "$svc" 2>&1 | sed 's/^/    /' || \
                log "WARN: failed to stop $svc"
        fi
    done
    log "Standby state — services stopped, Pi idle (~3W)"
}

start_services() {
    log "Switch ON detected — starting kiosk + companion services"
    for svc in "${SERVICES_START_ORDER[@]}"; do
        if ! systemctl is-active --quiet "$svc"; then
            # reset-failed first in case the unit is in failed state
            # from a prior crash or external manipulation.
            systemctl reset-failed "$svc" 2>/dev/null || true
            systemctl start "$svc" 2>&1 | sed 's/^/    /' || \
                log "WARN: failed to start $svc"
        fi
    done
    log "Running state — kiosk + Content Manager + Docker stack up"
}

reconcile_initial_state() {
    # On watcher startup (boot or service restart), read the current
    # GPIO state and align services to match.
    local state
    state=$(read_gpio)
    log "Initial GPIO ${GPIO_PIN} state: ${state}"
    case "$state" in
        low)
            # Switch is ON. Services should be running. Most boots
            # take this path.
            start_services
            ;;
        high)
            # Switch is OFF. Services should NOT run. The kiosk
            # service may auto-start via its [Install] WantedBy=
            # before we get here — stop it.
            stop_services
            ;;
        *)
            # Couldn't read GPIO. Default to safe behavior (services
            # running). Better to have the kiosk up than to leave it
            # standby because of a transient pinctrl glitch.
            log "GPIO read failed at startup — defaulting to RUNNING"
            start_services
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
