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
WEB_SERVICE=magic-dingus-web.service

# Mechanical toggles bounce. Measured on this hardware 2026-08-03: ONE
# flip of the front switch produced a falling edge and a rising edge 25
# MICROSECONDS apart, then two more edges — four events from one human
# action. Acting on each in turn started the whole stack, tore it down,
# and started it again, SIGKILLing the kiosk mid-launch and leaving the
# box in the opposite state from the switch.
#
# Two independent defences, because either alone still has a hole:
#   1. gpiomon's own --debounce-period, which filters in the kernel.
#   2. reconcile_to_level(), which ignores the edge DIRECTION entirely
#      and acts on the pin's settled level. Needed because stop_services
#      takes several seconds (LED sweep + docker), and any bounce that
#      arrives during it is still sitting in the pipe afterwards.
DEBOUNCE_PERIOD=100ms
SETTLE_SLEEP=0.25

# What we last drove the box to: "running", "standby", or "" for
# not-yet-known. Makes every repeated event an explicit no-op.
LAST_APPLIED=""

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

# Drive the box to whatever the switch is ACTUALLY set to right now, and
# do nothing if we are already there. This is what makes bounce harmless:
# the second, third and fourth events of a single flip all resolve to the
# same level we just applied, and each returns immediately.
#
# It also self-heals. If an event is ever missed — the watcher restarts,
# or an edge lands while stop_services is mid-teardown — the next event
# of any kind re-reads the pin and converges on the truth, instead of
# toggling to the opposite of wherever we drifted to.
reconcile_to_level() {
    local level desired
    level=$(read_gpio)

    case "$level" in
        low)  desired=running ;;   # switch closed to ground = ON
        high) desired=standby ;;
        *)
            # Unreadable. Never guess: acting on a failed read could put
            # a perfectly good box into standby with no way to see why.
            log "GPIO ${GPIO_PIN} unreadable — leaving services untouched"
            return
            ;;
    esac

    if [[ "$desired" == "$LAST_APPLIED" ]]; then
        log "Already ${desired} — ignoring (switch bounce or duplicate event)"
        return
    fi

    LAST_APPLIED="$desired"
    if [[ "$desired" == running ]]; then
        start_services
    else
        stop_services
    fi
}

all_leds_off() {
    for pin in "${LED_PINS[@]}"; do
        pinctrl set "$pin" op dl 2>/dev/null || true
    done
}

# Give RetroArch a chance to exit on its own before anything stops the
# kiosk. While a game is running the kiosk is parked inside waitpid() and
# CANNOT act on SIGTERM — so `systemctl stop` waits out TimeoutStopSec=5
# and then SIGKILLs the whole control group, emulator included, and the
# player's progress since their last save is gone with it.
#
# RetroArch is configured to auto-save SRAM and write a save state on
# exit, so a plain SIGTERM is all it needs. Once it exits, the kiosk's
# waitpid returns, it restores its own display state, and the ordinary
# stop below is clean.
#
# Exact-name match and kill by PID — never `pkill -f retroarch`, whose
# pattern would also match this script's own command line.
quiesce_retroarch() {
    local pids
    pids=$(pgrep -x retroarch 2>/dev/null || true)
    [[ -n "$pids" ]] || return 0

    log "Game in progress — asking RetroArch to exit so it can save first"
    # shellcheck disable=SC2086  # deliberate word-splitting: one or more PIDs
    kill -TERM $pids 2>/dev/null || true

    local waited=0
    while [[ $waited -lt 40 ]]; do
        if ! pgrep -x retroarch >/dev/null 2>&1; then
            log "RetroArch exited cleanly (save written)"
            # The kiosk still has to re-acquire DRM master and re-init
            # input after the emulator hands back. Let that finish rather
            # than stopping it mid-handoff.
            sleep 2
            return 0
        fi
        sleep 0.25
        waited=$((waited + 1))
    done
    log "WARN: RetroArch still running after 10s — continuing with the stop anyway"
}

stop_services() {
    log "Switch OFF detected — stopping kiosk + companion services"

    quiesce_retroarch

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
    local anim_pid=""
    if [[ -x "$SHUTDOWN_ANIM" ]]; then
        ( timeout 6 "$SHUTDOWN_ANIM" >/dev/null 2>&1 || true ) &
        anim_pid=$!
    fi

    for svc in "${COMPANION_SERVICES[@]}"; do
        if systemctl is-active --quiet "$svc"; then
            systemctl stop "$svc" 2>&1 | sed 's/^/    /' || \
                log "WARN: failed to stop $svc"
        fi
    done

    # Wait for the ANIMATION ONLY, by pid.
    #
    # A bare `wait` here deadlocked the box on the first real switch flip.
    # It waits for every background job of this shell — and gpiomon is one:
    # monitor_loop feeds the event loop from `< <(gpiomon ...)`, which bash
    # tracks as a background job. So stop_services sat waiting for the very
    # process that delivers switch events, forever. The box went to standby
    # correctly, then never came back and stopped responding to the toggle
    # entirely, because the watcher never returned to read another event.
    # Observed on hardware 2026-08-03: watcher blocked in kernel_wait4 with
    # gpiomon as its only remaining child.
    if [[ -n "$anim_pid" ]]; then
        wait "$anim_pid" 2>/dev/null || true
    fi
    all_leds_off
    log "Standby state — services stopped, Pi idle (~3W)"
}

start_services() {
    log "Switch ON detected — starting kiosk + companion services"

    # Light the LEDs FIRST, before anything slow, so the flip is acknowledged
    # the instant it happens. Standby leaves them dark and the screen stays
    # black for 10-15s while the kiosk comes up, so without this the owner
    # gets no sign the switch did anything and reasonably concludes the box
    # is dead. The OFF flip already answers with the shutdown sweep; this is
    # its counterpart, and it makes the pair symmetric: sweep down to sleep,
    # chase up to wake.
    #
    # Reuses the cold-boot chase rather than inventing a wake animation, so
    # waking looks exactly like booting — which is what it is, from the
    # owner's side. Nothing new has to stop it either: the kiosk kills this
    # unit as soon as it has the LED lines (main.cpp's stop_boot_led_sequence,
    # called unconditionally), then plays its own intro dance. If the kiosk
    # never comes up, the chase simply keeps running, which is a fair signal
    # that something is wrong rather than a silent black box.
    # ONLY on a genuine wake — i.e. when the kiosk is actually down. The chase
    # is stopped by the kiosk during ITS startup, so if the kiosk is already
    # running, `systemctl start` below is a no-op, nothing ever reclaims the
    # LED lines, and they chase forever with the box sitting at the menu.
    # That is not hypothetical: reconcile_initial_state calls this function on
    # every watcher restart, which happens on every deploy — caught exactly
    # that way while verifying this change.
    if ! systemctl is-active --quiet "$KIOSK_SERVICE"; then
        systemctl start led-boot-sequence.service 2>/dev/null || \
            log "WARN: could not start the wake LED chase (harness may be absent)"
    fi

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
    # The web admin goes up BEFORE the kiosk, and we wait for it, even
    # though that costs a second or two of wake time. It hosts the phone
    # remote's virtual gamepad (/dev/uinput), and InputManager enumerates
    # /dev/input exactly once, at startup, with no hotplug. A kiosk that
    # starts first therefore never sees the remote, and every paired
    # phone is silently dead — no error, no toast, nothing in a log —
    # until something restarts the kiosk again.
    systemctl reset-failed "$WEB_SERVICE" 2>/dev/null || true
    systemctl start "$WEB_SERVICE" 2>&1 | sed 's/^/    /' || \
        log "WARN: failed to start the web admin"

    # systemctl returns as soon as the process is forked; Flask still has
    # to create the uinput device. Poll for the device itself rather than
    # guessing a sleep. Bounded — if it never appears we start the kiosk
    # anyway, because a working screen without a phone remote beats no
    # screen at all.
    for _ in $(seq 1 40); do
        if grep -q "MagicDingus Phone Remote" /proc/bus/input/devices 2>/dev/null; then
            log "Phone-remote gamepad present — starting kiosk"
            break
        fi
        sleep 0.25
    done

    systemctl reset-failed "$KIOSK_SERVICE" 2>/dev/null || true
    systemctl start "$KIOSK_SERVICE" 2>&1 | sed 's/^/    /' || \
        log "WARN: failed to start the kiosk"

    # Docker last and --no-block: six containers and a VPN tunnel take
    # 20-40s, and none of that is on screen. Queue it and return, so the
    # watcher is free to react to the NEXT flip of the switch.
    systemctl reset-failed magic-dingus-services.service 2>/dev/null || true
    systemctl --no-block start magic-dingus-services.service 2>&1 | sed 's/^/    /' || \
        log "WARN: failed to queue the Docker stack"

    # And explicitly re-run the qBittorrent reconciler, which owns the
    # crash-recovery that clears a stuck download throttle.
    #
    # It cannot ride along on its own here: it is WantedBy=multi-user.target,
    # so systemd pulls it in when that TARGET starts — i.e. at boot only.
    # Starting magic-dingus-services directly, as we just did, does not drag
    # it along. Without this line, flipping to standby part-way through a
    # movie leaves the playback trickle cap engaged and NOTHING clears it
    # until the next full reboot: every download on the box crawls at ~2 MB/s
    # with nothing on screen to explain why. That is precisely the failure
    # the reconciler was written to eliminate, on the path most likely to
    # cause it. Its own After=/Requires=magic-dingus-services keeps the
    # ordering correct, and --no-block keeps it off this loop's critical path.
    systemctl reset-failed magic-dingus-sync-qbit-password.service 2>/dev/null || true
    systemctl --no-block start magic-dingus-sync-qbit-password.service 2>&1 | sed 's/^/    /' || \
        log "WARN: failed to queue the qBittorrent reconciler"

    log "Running state — kiosk up; Docker stack starting behind it"
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
            LAST_APPLIED=running
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
        # The event's DIRECTION is deliberately ignored. A bouncing
        # toggle emits both directions within microseconds, and acting on
        # each in turn is what made one flip start-stop-start the box.
        # Let the contacts settle, then ask the pin where it actually is.
        sleep "$SETTLE_SLEEP"
        reconcile_to_level
    done < <(gpiomon --debounce-period "$DEBOUNCE_PERIOD" \
                     --edges=both --chip "$GPIO_CHIP" "$GPIO_PIN" 2>&1)

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
