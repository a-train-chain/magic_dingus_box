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
// VERIFY ON HARDWARE before trusting (Task 12's controller_probe): the
// builtin-profile tokens are the ground truth this must reproduce.
int button_index(const std::vector<uint16_t>& key_codes, uint16_t code);
int axis_index(const std::vector<uint16_t>& abs_codes, uint16_t code);
int hat_number(uint16_t abs_code);

std::string bind_token(const std::vector<uint16_t>& key_codes,
                       const std::vector<uint16_t>& abs_codes,
                       PhysicalBinding::Kind kind, uint16_t code,
                       int direction);

}  // namespace retroarch
