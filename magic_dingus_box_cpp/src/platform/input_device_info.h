#pragma once

// What the kiosk knows about an input device before it decides to use it.
// Std-only (no libevdev, no Linux headers) so the Mac suites test it:
// InputManager fills InputDeviceTraits from libevdev, and the game launcher's
// controller detector reads the same facts from sysfs through
// read_sysfs_input_info().

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace platform {

// The capability bits that decide "is this a controller?".
struct InputDeviceTraits {
    bool has_abs_x = false;       // EV_ABS ABS_X
    bool has_abs_hat0x = false;   // EV_ABS ABS_HAT0X
    bool has_btn_touch = false;   // EV_KEY BTN_TOUCH (touchpads, touchscreens)
    bool has_btn_digi = false;    // EV_KEY BTN_DIGI (pen tablets)
    bool accelerometer = false;   // INPUT_PROP_ACCELEROMETER (motion sensors)
};

// True for a joystick/gamepad: it has a stick or a hat, and it is NOT a
// touchpad, digitizer or motion sensor. Mirrors the kernel joydev blacklist,
// so the kiosk opens exactly the devices that also get a /dev/input/js node.
// Modern pads split themselves into several input devices — a DualSense has
// a gamepad, a "Motion Sensors" device and a "Touchpad" device, and both of
// the extra ones report ABS_X — so "has ABS_X" alone is not enough.
bool is_joystick_device(const InputDeviceTraits& traits);

// Parse a sysfs capability bitmap (capabilities/key, capabilities/abs,
// properties): space-separated hex words, the MOST significant word first,
// each word `word_bits` wide (the kernel's BITS_PER_LONG). Returns the set
// bit numbers ascending. Empty on empty or malformed text.
std::vector<uint16_t> parse_capability_bitmap(const std::string& text,
                                              unsigned word_bits = 64);

// One /sys/class/input/<node> (js*, event*) as sysfs describes it.
struct SysfsInputInfo {
    std::string driver;   // kernel driver of the parent device: "playstation",
                          // "xpad", "nintendo", "microsoft", "hid-generic"...
                          // Empty when the parent has no driver link.
    uint16_t bustype = 0; // 0x03 USB, 0x05 Bluetooth
    uint16_t vid = 0, pid = 0;
    std::string name;
    std::vector<uint16_t> key_codes;   // ascending EV_KEY codes
    std::vector<uint16_t> abs_codes;   // ascending EV_ABS codes (incl. hats)
    bool accelerometer = false;
};

// Read <class_input_dir>/<node>/device/... nullopt when the node is gone.
// Fields that cannot be read stay at their zero values.
std::optional<SysfsInputInfo> read_sysfs_input_info(const std::string& class_input_dir,
                                                    const std::string& node);

}  // namespace platform
