#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "controller_profile.h"

namespace retroarch {

// RetroArch's udev joypad driver numbers buttons the same way the kernel
// joystick API does: EV_KEY codes in [BTN_JOYSTICK(0x120), KEY_MAX] get
// indices first in ascending code order, then codes in
// [BTN_MISC(0x100), BTN_JOYSTICK) wrap after them. Axes are the device's
// non-hat ABS codes in ascending order; ABS_HAT0X..ABS_HAT3Y become hats.
//
// HARDWARE STATUS (2026-07-29, src/tools/controller_probe.cpp run on the
// production Pi 5 against the attached "SWITCH CO.,LTD. Controller (Dinput)"
// 2563:0575 N64-style adapter -- 13 buttons at codes 304..316, axes
// ABS_X/Y/Z/RZ + a real ABS_HAT0X/Y):
//
//   VERIFIED  Ascending order in the high (>= BTN_JOYSTICK) button range.
//             Codes 304..316 -> indices 0..12, reproducing all ten of
//             builtin_n64_adapter_profile()'s button tokens exactly
//             (18/18 tokens matched, buttons + hat + stick).
//   VERIFIED  Non-hat axes ascending. ABS_X/Y/Z/RZ (codes 0,1,2,5) -> axis
//             indices 0,1,2,3 -- so ABS_RZ really is axis 3, which is what
//             the DragonRise profile's "+3"/"-3" right-stick tokens depend
//             on. Corroborated independently by the kernel's own joydev
//             JSIOCGAXMAP on the same pad (0->0 1->1 2->2 3->5).
//
//   CONSISTENT WITH (not proven by) that measurement: hats being EXCLUDED
//             from the axis numbering rather than merely SORTED LAST. The
//             two hypotheses are indistinguishable on any real pad, because
//             evdev hat codes are always >= ABS_HAT0X (16) while analog axis
//             codes are always < 16 -- so a hat can never land before an
//             analog axis under either rule, and every measurement to date
//             agrees with both. axis_index() implements exclusion. To tell
//             them apart you would need a synthetic uinput pad advertising a
//             hat code below an analog code, which evdev's own numbering
//             makes impossible; reading udev_joypad.c is the practical route.
//   VERIFIED  ABS_HAT0X/Y map to hat 0 with the left/right/up/down suffixes.
//
//   ASSUMED   The [BTN_MISC, BTN_JOYSTICK) WRAP-AFTER branch in
//             button_index(). No pad available to this project reports any
//             code in that range -- both shipped pads and the phone-remote
//             uinput device are entirely >= BTN_JOYSTICK -- so the second
//             loop has never executed against real hardware. It is still
//             read from RetroArch's documented behavior, not measured.
//             RetroArch here is the distro binary (1.20.0+dfsg-2+b1) with
//             no source package installed and no autoconfig .cfg profiles
//             on disk, so input/drivers_joypad/udev_joypad.c could not be
//             consulted locally either. To settle it: read that file from
//             the 1.20.0 source, or enumerate a synthetic uinput pad that
//             advertises codes in both ranges and compare.
int button_index(const std::vector<uint16_t>& key_codes, uint16_t code);
int axis_index(const std::vector<uint16_t>& abs_codes, uint16_t code);
int hat_number(uint16_t abs_code);

std::string bind_token(const std::vector<uint16_t>& key_codes,
                       const std::vector<uint16_t>& abs_codes,
                       PhysicalBinding::Kind kind, uint16_t code,
                       int direction);

}  // namespace retroarch
