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

// Return the first recognized legacy type from the same filtered, numeric
// event-device inventory as detect_connected_controllers(). UNKNOWN when
// no gamepad has a known VID/PID; modern automatic mapping uses pad metadata.
ControllerType detect_primary_controller();
// Explicit filesystem roots for offline inventory inspection and tests.
ControllerType detect_primary_controller(const std::string& input_dir,
                                         const std::string& class_input_dir);

// Short human-readable identifier for logs ("N64_ADAPTER", "PS_STYLE_DRAGONRISE",
// "UNKNOWN"). Guaranteed not to throw.
std::string controller_type_name(ControllerType t);

// One gamepad event device, as enumerated from /dev/input/eventN.
// `port` is the 0-based index in numeric event-node order (event2 before
// event10), phone remote and auxiliaries skipped; port N is player N+1. This is
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

// Enumerate canonical /dev/input/eventN nodes in numeric order, without
// opening or grabbing them. Classify sysfs capabilities with the kiosk's
// shared joystick policy and skip the phone remote, motion/touch/digitizer
// nodes, and incomplete classification reads. Aliases are never scanned;
// distinct same-model pads retain separate ports. VID/PID read failures
// stay 0000 so reservation emission can leave those devices unreserved.
// Empty inventory preserves the launcher's existing single-mapping fallback.
std::vector<DetectedPad> detect_connected_controllers();
// Explicit filesystem roots, with the same policy as the live scan.
std::vector<DetectedPad> detect_connected_controllers(const std::string& input_dir,
                                                     const std::string& class_input_dir);

} // namespace retroarch
