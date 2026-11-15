#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace retroarch {

// Which controller type the kiosk has detected as the primary joystick.
// Expand this enum + update detect_primary_controller() / the mapping
// dispatcher in retroarch_launcher.cpp when adding support for a new one.
enum class ControllerType {
    UNKNOWN,
    N64_ADAPTER,          // SWITCH CO.,LTD. 0e6d:111d
    PS_STYLE_DRAGONRISE,  // DragonRise/Microntek Generic USB Joystick 0079:0006
};

// Match a (vendor, product) pair to a known controller type. Exposed (moved
// out of the anonymous namespace) for per-pad mapping resolution — see
// resolve_mapping_for_pad() in controller_mapping.h.
ControllerType match_vid_pid(uint16_t vid, uint16_t pid);

// Scan /dev/input/js* lexicographically and return the first recognized type.
// Reads VID/PID from /sys/class/input/jsN/device/id/{vendor,product}.
// Returns UNKNOWN if no joystick is present or the VID/PID doesn't match any
// supported device.
ControllerType detect_primary_controller();

// Short human-readable identifier for logs ("N64_ADAPTER", "PS_STYLE_DRAGONRISE",
// "UNKNOWN"). Guaranteed not to throw.
std::string controller_type_name(ControllerType t);

// One physical joystick node, as enumerated from /dev/input/js*.
// `port` is the 0-based index in lexicographic js-node order (js0 -> 0,
// js1 -> 1, ...), phone remote skipped; port N here is player N+1. This is
// NOT necessarily RetroArch's own joypad order (its udev driver enumerates
// the udev DB in syspath order and counts the phone remote) -- the launcher
// pins each player to its pad with write_port_reservations().
struct DetectedPad {
    DetectedPad() = default;
    // The identity-only form every caller used before the sysfs fields
    // below existed ({port, vid, pid, name}). A constructor rather than
    // aggregate init so those sites stay warning-free under
    // -Wmissing-field-initializers.
    DetectedPad(int port_, uint16_t vid_, uint16_t pid_, std::string name_)
        : port(port_), vid(vid_), pid(pid_), name(std::move(name_)) {}

    int port = 0;
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::string name;
    // From sysfs (platform::read_sysfs_input_info): the kernel driver, the
    // bus (0x03 USB, 0x05 Bluetooth) and the capability lists. They let a
    // modern pad with no saved profile get an automatic layout
    // (retroarch/standard_layout.h) instead of the legacy N64 fallback.
    std::string driver;
    uint16_t bustype = 0;
    std::vector<uint16_t> key_codes;   // ascending EV_KEY codes
    std::vector<uint16_t> abs_codes;   // ascending EV_ABS codes (incl. hats)
};

// Enumerate /dev/input/js* lexicographically (same walk as
// detect_primary_controller()) and return one DetectedPad per node, in
// port order. VID/PID come from /sys/class/input/jsN/device/id/{vendor,
// product} (0000 on any read/parse failure, same as detect_primary_controller);
// `name` comes from the sysfs device/name line. Returns an empty vector if
// no /dev/input/js* nodes exist -- callers must treat that as "fall back to
// the existing single-mapping path", not as an error.
//
// This does NOT replace detect_primary_controller(): the autoconfig-file
// emission branch in retroarch_launcher.cpp still calls that function for
// its single ControllerType decision. This is for per-port mapping
// resolution (see resolve_mapping_for_pad() in controller_mapping.h),
// which needs every connected pad's own VID/PID, not just the first match.
std::vector<DetectedPad> detect_connected_controllers();

} // namespace retroarch
