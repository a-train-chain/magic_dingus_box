#!/bin/bash
# led_boot_sequence.sh
# Plays LED chase animation during Pi boot
# Starts at 80 BPM and accelerates to 110 BPM over ~25 seconds
# Gets killed when main app starts

# LED GPIO pins (active high)
LED_PINS=(12 16 26 20)
NUM_LEDS=${#LED_PINS[@]}

# Timing config
START_BPM=55
END_BPM=110
RAMP_DURATION_SEC=25

START_TIME=$(date +%s)

# Simple on/off 
set_led() {
    local pin=$1
    local state=$2  # 1=on, 0=off
    if [ "$state" = "1" ]; then
        pinctrl set $pin op dh 2>/dev/null
    else
        pinctrl set $pin op dl 2>/dev/null
    fi
}

all_leds_off() {
    for pin in "${LED_PINS[@]}"; do
        pinctrl set $pin op dl 2>/dev/null
    done
}

# Chase step
chase_step() {
    local current=$1
    local on_ms=$2
    
    # Turn off all first to ensure clean state
    all_leds_off
    
    # Turn on current LED
    set_led ${LED_PINS[$current]} 1
    
    # Sleep with floating point format (0.xxx)
    sleep 0.$(printf '%03d' $on_ms)
}

# Main loop
logger -t led-boot "Starting LED boot sequence (80->110 BPM)"
trap 'all_leds_off; logger -t led-boot "LED boot sequence stopped"; exit 0' SIGTERM SIGINT

current_led=0

while true; do
    # Calculate current BPM
    NOW=$(date +%s)
    ELAPSED=$((NOW - START_TIME))
    
    if [ $ELAPSED -ge $RAMP_DURATION_SEC ]; then
        CURRENT_BPM=$END_BPM
    else
        # Linear interpolation
        # BPM = START + (END - START) * ELAPSED / DURATION
        BPM_INC=$(( (END_BPM - START_BPM) * ELAPSED / RAMP_DURATION_SEC ))
        CURRENT_BPM=$((START_BPM + BPM_INC))
    fi
    
    # Calculate delay in ms
    # 60000 ms per minute
    # beat_ms = 60000 / BPM
    # step_ms = beat_ms / NUM_LEDS
    # Combined: step_ms = (60000 / NUM_LEDS) / BPM = 15000 / BPM
    
    STEP_MS=$((15000 / CURRENT_BPM))
    
    chase_step $current_led $STEP_MS
    
    current_led=$(( (current_led + 1) % NUM_LEDS ))
done
