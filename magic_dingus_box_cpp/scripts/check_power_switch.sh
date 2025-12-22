#!/bin/bash
# check_power_switch.sh
# Runs at early boot to check if power switch is in ON position
# If OFF (GPIO3 = HIGH), halt immediately instead of continuing boot

GPIO_PIN=3
HALT_DELAY=2  # Seconds to wait before halting (allows filesystem to settle)

# Use pinctrl to read GPIO state (more reliable than sysfs, works even when
# gpio-shutdown overlay is loaded)
GPIO_STATE=$(pinctrl get ${GPIO_PIN} 2>/dev/null)

# Log the raw state
logger -t power-switch "GPIO${GPIO_PIN} raw state: ${GPIO_STATE}"

# Parse the state - pinctrl output format: "3: ip pu | hi" or "3: ip pu | lo"
# We look for "hi" (HIGH) or "lo" (LOW) at the end
if echo "$GPIO_STATE" | grep -q "| hi"; then
    GPIO_VALUE="HIGH"
elif echo "$GPIO_STATE" | grep -q "| lo"; then
    GPIO_VALUE="LOW"
else
    GPIO_VALUE="UNKNOWN"
fi

logger -t power-switch "GPIO${GPIO_PIN} state at boot: ${GPIO_VALUE}"

# GPIO3 wiring:
#   Switch ON:  GPIO3 = LOW (connected to GND)
#   Switch OFF: GPIO3 = HIGH (floating/pulled up)

if [ "$GPIO_VALUE" = "HIGH" ]; then
    logger -t power-switch "Power switch is OFF - halting system"
    echo "Power switch is OFF - system will halt in ${HALT_DELAY} seconds..."
    sleep ${HALT_DELAY}
    /sbin/poweroff
elif [ "$GPIO_VALUE" = "LOW" ]; then
    logger -t power-switch "Power switch is ON - continuing boot"
else
    logger -t power-switch "Could not determine GPIO${GPIO_PIN} state - continuing boot"
fi

exit 0
