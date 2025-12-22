#!/bin/bash
# led_shutdown_animation.sh
# Plays LED flicker/fade animation when system is shutting down

# LED GPIO pins (active high)
LED_PINS=(12 16 26 20)

set_all_leds() {
    local state=$1  # "on" or "off"
    for pin in "${LED_PINS[@]}"; do
        if [ "$state" = "on" ]; then
            pinctrl set $pin op dh 2>/dev/null
        else
            pinctrl set $pin op dl 2>/dev/null
        fi
    done
}

logger -t led-shutdown "Starting shutdown LED animation"

# Fast flicker slowing down with fading effect
# Format: delay_ms cycles
flicker_stages=(
    "20 10"
    "25 8"
    "30 7"
    "40 5"
    "50 4"
    "70 3"
    "90 3"
    "110 2"
    "140 2"
    "170 2"
    "200 1"
)

stage_num=0
total_stages=${#flicker_stages[@]}

for stage in "${flicker_stages[@]}"; do
    read delay cycles <<< "$stage"
    
    # Calculate simulated fade - reduce on_time as we progress
    fade_factor=$((total_stages - stage_num))
    on_time=$((delay * fade_factor / total_stages))
    off_time=$((delay * 2 - on_time))
    
    for ((c=0; c<cycles; c++)); do
        set_all_leds "on"
        sleep 0.$(printf '%03d' $on_time)
        set_all_leds "off"
        sleep 0.$(printf '%03d' $off_time)
    done
    
    ((stage_num++))
done

# Final dying pulses - shorter and shorter
for i in 5 4 3 2 1; do
    set_all_leds "on"
    sleep 0.0$((i * 10))
    set_all_leds "off"
    sleep 0.1
done

# Ensure all off at end
set_all_leds "off"

logger -t led-shutdown "Shutdown LED animation complete"
exit 0
