#include "controller_profile.h"

#include <cstdio>

#ifdef __linux__
#include <linux/input-event-codes.h>  // only for the code constants; see below
#endif

namespace retroarch {

// NOTE ON PORTABILITY: this file must build on macOS for the unit tests.
// linux/input-event-codes.h does not exist there, so define the handful of
// codes we need when the header is absent (or not included, as on Mac).
#ifndef BTN_SOUTH
#define BTN_SOUTH 0x130
#endif
#ifndef ABS_X
#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_Z 0x02
#define ABS_RZ 0x05
#define ABS_HAT0X 0x10
#define ABS_HAT0Y 0x11
#endif

namespace {
using L = LogicalControl;
using K = PhysicalBinding::Kind;

PhysicalBinding btn(uint16_t code, const char* tok) { return {K::BUTTON, code, 0, tok}; }
PhysicalBinding hat(uint16_t code, int dir, const char* tok) { return {K::HAT, code, dir, tok}; }
PhysicalBinding axis(uint16_t code, int dir, const char* tok) { return {K::AXIS, code, dir, tok}; }
}  // namespace

const PhysicalProfile& builtin_n64_adapter_profile() {
    static const PhysicalProfile p = [] {
        PhysicalProfile p;
        p.name = "SWITCH CO.,LTD. Controller (N64 adapter)";
        p.style = ControllerStyle::N64_STYLE;
        p.vid = 0x0e6d; p.pid = 0x111d;
        // Joystick index i lives at evdev code 304+i on this adapter
        // (contiguous BTN_GAMEPAD range; indices verified via evtest, see
        // controller_mapping.cpp's physical table).
        p.controls = {
            {L::N64_C_LEFT, btn(304, "0")},  {L::N64_B, btn(305, "1")},
            {L::N64_A, btn(306, "2")},       {L::N64_C_DOWN, btn(307, "3")},
            {L::N64_L, btn(308, "4")},       {L::N64_R, btn(309, "5")},
            {L::N64_Z, btn(310, "6")},       {L::N64_C_RIGHT, btn(312, "8")},
            {L::N64_C_UP, btn(313, "9")},    {L::N64_START, btn(316, "12")},
            {L::N64_DPAD_UP, hat(ABS_HAT0Y, -1, "h0up")},
            {L::N64_DPAD_DOWN, hat(ABS_HAT0Y, +1, "h0down")},
            {L::N64_DPAD_LEFT, hat(ABS_HAT0X, -1, "h0left")},
            {L::N64_DPAD_RIGHT, hat(ABS_HAT0X, +1, "h0right")},
            {L::N64_STICK_UP, axis(ABS_Y, -1, "-1")},
            {L::N64_STICK_DOWN, axis(ABS_Y, +1, "+1")},
            {L::N64_STICK_LEFT, axis(ABS_X, -1, "-0")},
            {L::N64_STICK_RIGHT, axis(ABS_X, +1, "+0")},
        };
        return p;
    }();
    return p;
}

const PhysicalProfile& builtin_dragonrise_profile() {
    static const PhysicalProfile p = [] {
        PhysicalProfile p;
        p.name = "DragonRise Generic USB Joystick";
        p.style = ControllerStyle::PS_STYLE;
        p.vid = 0x0079; p.pid = 0x0006;
        // Joystick index i lives at evdev code 288+i (BTN_TRIGGER range,
        // see input_manager.cpp map_button_to_action comment block).
        // Right-stick axis tokens are the LEGACY +2/+3 (what shipped code
        // emits); the on-Pi probe (Task 12) re-verifies against hardware.
        p.controls = {
            {L::TRIANGLE, btn(288, "0")}, {L::CIRCLE, btn(289, "1")},
            {L::CROSS, btn(290, "2")},    {L::SQUARE, btn(291, "3")},
            {L::L1, btn(292, "4")},       {L::R1, btn(293, "5")},
            {L::L2, btn(294, "6")},       {L::R2, btn(295, "7")},
            {L::SELECT, btn(296, "8")},   {L::START, btn(297, "9")},
            {L::DPAD_UP, hat(ABS_HAT0Y, -1, "h0up")},
            {L::DPAD_DOWN, hat(ABS_HAT0Y, +1, "h0down")},
            {L::DPAD_LEFT, hat(ABS_HAT0X, -1, "h0left")},
            {L::DPAD_RIGHT, hat(ABS_HAT0X, +1, "h0right")},
            {L::LSTICK_UP, axis(ABS_Y, -1, "-1")},
            {L::LSTICK_DOWN, axis(ABS_Y, +1, "+1")},
            {L::LSTICK_LEFT, axis(ABS_X, -1, "-0")},
            {L::LSTICK_RIGHT, axis(ABS_X, +1, "+0")},
            {L::RSTICK_UP, axis(ABS_RZ, -1, "-3")},
            {L::RSTICK_DOWN, axis(ABS_RZ, +1, "+3")},
            {L::RSTICK_LEFT, axis(ABS_Z, -1, "-2")},
            {L::RSTICK_RIGHT, axis(ABS_Z, +1, "+2")},
        };
        return p;
    }();
    return p;
}

std::string vidpid_key(uint16_t vid, uint16_t pid) {
    char buf[10];
    std::snprintf(buf, sizeof(buf), "%04x:%04x", vid, pid);
    return buf;
}

}  // namespace retroarch
