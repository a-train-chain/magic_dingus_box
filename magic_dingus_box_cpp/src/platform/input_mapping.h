#pragma once

// The kiosk's BUILT-IN evdev code -> InputAction tables (no per-model
// overlay; see menu_nav_overlay.h for those). Std-only, so the Mac suite
// tests them — including parity with the phone remote, whose virtual
// gamepad (magic_dingus_box/web/remote/uinput_writer.py) can only work if
// every code it emits lands on the action its button is labelled with
// (tests/platform/test_phone_remote_parity.cpp parses that file).
//
// The numeric codes are linux/input-event-codes.h values spelled out here
// because that header does not exist on macOS; input_manager.cpp
// static_asserts each one against the real constant on Linux.

#include <cstdint>

#include "menu_nav_overlay.h"

namespace platform::mapping {

namespace codes {
// Keyboard (EV_KEY)
constexpr uint16_t kKeyEsc       = 1;
constexpr uint16_t kKeyQ         = 16;
constexpr uint16_t kKeyP         = 25;
constexpr uint16_t kKeyEnter     = 28;
constexpr uint16_t kKeyN         = 49;
constexpr uint16_t kKeySpace     = 57;
constexpr uint16_t kKeyPlayPause = 164;
// Gamepad buttons (EV_KEY), named by their linux constant; the comment
// says what the kiosk's known pads put there.
constexpr uint16_t kBtnTrigger = 288;   // 0x120 — PS-style pad Triangle
constexpr uint16_t kBtnThumb   = 289;   // 0x121 — PS-style pad Circle
constexpr uint16_t kBtnThumb2  = 290;   // 0x122 — PS-style pad Cross
constexpr uint16_t kBtnTop2    = 292;   // 0x124 — PS-style pad L1
constexpr uint16_t kBtnPinkie  = 293;   // 0x125 — PS-style pad R1
constexpr uint16_t kBtnBase4   = 297;   // 0x129 — PS-style pad Start
constexpr uint16_t kBtnSouth   = 304;   // 0x130 — standard A
constexpr uint16_t kBtnEast    = 305;   // 0x131 — standard B
constexpr uint16_t kBtnC       = 306;   // 0x132 — N64 adapter A
constexpr uint16_t kBtnWest    = 308;   // 0x134 — N64 adapter L
constexpr uint16_t kBtnZ       = 309;   // 0x135 — N64 adapter R
constexpr uint16_t kBtnTl      = 310;   // 0x136 — N64 adapter Z
constexpr uint16_t kBtnStart   = 315;   // 0x13b
constexpr uint16_t kBtnMode    = 316;   // 0x13c — N64 adapter START
// D-pad hat (EV_ABS)
constexpr uint16_t kAbsHat0X = 0x10;
constexpr uint16_t kAbsHat0Y = 0x11;
}  // namespace codes

// EV_KEY on a joystick-class device (real pads AND the phone remote's
// uinput pad) when no overlay claims the code. Release events map to the
// same action as presses; callers check InputEvent::pressed.
InputAction joystick_button_action(uint16_t code);

// EV_KEY on a keyboard (arrow keys are handled in poll() as rotation and
// never reach this table).
InputAction keyboard_key_action(uint16_t code);

}  // namespace platform::mapping
