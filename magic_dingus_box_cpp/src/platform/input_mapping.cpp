#include "input_mapping.h"

namespace platform::mapping {

using namespace codes;

InputAction joystick_button_action(uint16_t code) {
    // N64 Controller mappings (matching Python evdev_joystick.py)
    //   304 BTN_SOUTH (standard A) / 306 N64 A / 316 START -> SELECT
    //   305 BTN_EAST (B)                                   -> SETTINGS_MENU
    //   310 Z -> PLAY_PAUSE, 309 R -> NEXT, 308 L -> PREV
    //
    // PS-style USB pad (DragonRise/Microntek 0079:0006) uses the "joystick"
    // button range (BTN_TRIGGER..BTN_BASE6 = 288..299):
    //   288 Triangle / 289 Circle / 290 Cross / 291 Square
    //   292 L1 / 293 R1 / 294 L2 / 295 R2 / 296 Select / 297 Start
    //
    // Operator preference for the Magic Dingus Box kiosk:
    //   Cross   → SELECT         (confirm — Americas PlayStation convention)
    //   Circle  → SETTINGS_MENU  (open kiosk settings)
    //   Square  → unassigned (free to remap later)
    //   Triangle → PLAY_PAUSE
    //   L1 / R1  → PREV / NEXT
    //
    // The phone remote's virtual pad emits 304/305/308/309/310 — see
    // tests/platform/test_phone_remote_parity.cpp before changing those.
    switch (code) {
        case kBtnSouth:
        case kBtnC:
        case kBtnMode:
        case kBtnThumb2:   // Cross
        case kBtnBase4:    // PS Start
            return InputAction::SELECT;
        case kBtnEast:
        case kBtnThumb:    // Circle
            return InputAction::SETTINGS_MENU;
        case kBtnTl:       // N64 Z
        case kBtnTrigger:  // Triangle
            return InputAction::PLAY_PAUSE;
        case kBtnZ:        // N64 R
        case kBtnPinkie:   // R1
            return InputAction::NEXT;
        case kBtnWest:     // N64 L
        case kBtnTop2:     // L1
            return InputAction::PREV;
        default:
            return InputAction::NONE;
    }
}

InputAction keyboard_key_action(uint16_t code) {
    // Keyboard mappings (matching Python keyboard.py)
    switch (code) {
        case kKeyEnter:
        case kKeySpace:
            return InputAction::SELECT;
        case kKeyN:
            return InputAction::NEXT;
        case kKeyP:
            return InputAction::PREV;
        case kKeyPlayPause:
            return InputAction::PLAY_PAUSE;
        case kKeyEsc:
        case kKeyQ:
            return InputAction::QUIT;
        default:
            return InputAction::NONE;
    }
}

}  // namespace platform::mapping
