#include "joydev_index.h"
#include <algorithm>

namespace retroarch {

namespace {
constexpr uint16_t kBtnMisc = 0x100;
constexpr uint16_t kBtnJoystick = 0x120;
constexpr uint16_t kHatFirst = 0x10;  // ABS_HAT0X
constexpr uint16_t kHatLast = 0x17;   // ABS_HAT3Y
}  // namespace

int button_index(const std::vector<uint16_t>& key_codes, uint16_t code) {
    std::vector<uint16_t> sorted = key_codes;
    std::sort(sorted.begin(), sorted.end());
    int idx = 0;
    for (uint16_t c : sorted)                       // high range first
        if (c >= kBtnJoystick) { if (c == code) return idx; ++idx; }
    for (uint16_t c : sorted)                       // BTN_MISC wraps after
        if (c >= kBtnMisc && c < kBtnJoystick) { if (c == code) return idx; ++idx; }
    return -1;
}

int hat_number(uint16_t abs_code) {
    if (abs_code < kHatFirst || abs_code > kHatLast) return -1;
    return (abs_code - kHatFirst) / 2;
}

int axis_index(const std::vector<uint16_t>& abs_codes, uint16_t code) {
    if (hat_number(code) >= 0) return -1;
    std::vector<uint16_t> sorted = abs_codes;
    std::sort(sorted.begin(), sorted.end());
    int idx = 0;
    for (uint16_t c : sorted) {
        if (hat_number(c) >= 0) continue;
        if (c == code) return idx;
        ++idx;
    }
    return -1;
}

std::string bind_token(const std::vector<uint16_t>& key_codes,
                       const std::vector<uint16_t>& abs_codes,
                       PhysicalBinding::Kind kind, uint16_t code,
                       int direction) {
    switch (kind) {
        case PhysicalBinding::Kind::BUTTON: {
            int i = button_index(key_codes, code);
            return i < 0 ? "" : std::to_string(i);
        }
        case PhysicalBinding::Kind::HAT: {
            int h = hat_number(code);
            if (h < 0) return "";
            const bool is_y = (code - kHatFirst) % 2 == 1;
            const char* dir = is_y ? (direction < 0 ? "up" : "down")
                                   : (direction < 0 ? "left" : "right");
            return "h" + std::to_string(h) + dir;
        }
        case PhysicalBinding::Kind::AXIS: {
            int i = axis_index(abs_codes, code);
            if (i < 0) return "";
            return (direction < 0 ? "-" : "+") + std::to_string(i);
        }
    }
    return "";
}

}  // namespace retroarch
