#pragma once

#include <string>

namespace retroarch {

// Which controller type the kiosk has detected as the primary joystick.
// Expand this enum + update detect_primary_controller() / the mapping
// dispatcher in retroarch_launcher.cpp when adding support for a new one.
enum class ControllerType {
    UNKNOWN,
    N64_ADAPTER,          // SWITCH CO.,LTD. 0e6d:111d
    PS_STYLE_DRAGONRISE,  // DragonRise/Microntek Generic USB Joystick 0079:0006
};

// Scan /dev/input/js* lexicographically and return the first recognized type.
// Reads VID/PID from /sys/class/input/jsN/device/id/{vendor,product}.
// Returns UNKNOWN if no joystick is present or the VID/PID doesn't match any
// supported device.
ControllerType detect_primary_controller();

// Short human-readable identifier for logs ("N64_ADAPTER", "PS_STYLE_DRAGONRISE",
// "UNKNOWN"). Guaranteed not to throw.
std::string controller_type_name(ControllerType t);

} // namespace retroarch
