#include "controller_profile.h"

#include <cstdio>

#ifdef __linux__
#include <linux/input-event-codes.h>  // only for the code constants; see below
#endif

namespace retroarch {

// NOTE ON PORTABILITY: this file must build on macOS for the unit tests.
// linux/input-event-codes.h does not exist there, so define the handful of
// codes we need when the header is absent (or not included, as on Mac).
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
        // (contiguous BTN_GAMEPAD range). The physical layout used to be
        // a separate table at the top of controller_mapping.cpp; that
        // table was deleted by the semantic/physical split (this task),
        // and the layout now lives here, in the profile itself.
        //
        // HARDWARE-CONFIRMED (2026-07-29, see .superpowers/sdd/
        // hardware-evidence.md): captured live from a "SWITCH CO.,LTD.
        // Controller (Dinput)" pad (2563:0575, functionally an N64-style
        // adapter) on the production Pi. Capability bitmaps: EV_KEY =
        // 0x1fff<<48 -> codes 304..316 inclusive, 13 contiguous buttons;
        // EV_ABS = 0x30027 -> bits 0,1,2,5,16,17 = ABS_X/Y/Z/RZ plus a
        // REAL ABS_HAT0X/Y (not an 8-bit axis overload). Every code and
        // token below (buttons, hat, and stick) matches that hardware.
        p.controls = {
            {L::N64_C_LEFT, btn(304, "0")},  {L::N64_B, btn(305, "1")},
            {L::N64_A, btn(306, "2")},       {L::N64_C_DOWN, btn(307, "3")},
            // NOTE: evdev names are misleading on this adapter -- BTN_Z
            // (evdev code 309) is physically the R shoulder, and BTN_TL
            // (evdev code 310) is physically the Z trigger. A reader
            // checking linux/input-event-codes.h against the two bindings
            // below would see N64_R at code 309 (BTN_Z) and N64_Z at code
            // 310 (BTN_TL) and might "correct" them -- don't; they are
            // right as written.
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
        // see input_manager.cpp map_button_to_action comment block --
        // that function documents BUTTON codes only, nothing about axes
        // or hats, so the HAT/AXIS assignments below are this profile's
        // own, not transcribed from anywhere).
        //
        // Right-stick tokens +2/+3: NOT merely "legacy" -- per
        // .superpowers/sdd/hardware-evidence.md these are the correct
        // joydev axis INDICES for ABS_Z (raw evdev code 2) / ABS_RZ (raw
        // evdev code 5) respectively. controller_mapping.cpp's DragonRise
        // comment ("Right stick: axes 2 (Rx) / 5 (Ry)") looks like it
        // disagrees but is quoting RAW ABS codes -- a different numbering
        // system from joydev indices: ABS_Z (code 2) is joydev axis index
        // 2, ABS_RZ (code 5) is joydev axis index 3. Both the comment and
        // these tokens are correct; no fix needed.
        p.controls = {
            {L::TRIANGLE, btn(288, "0")}, {L::CIRCLE, btn(289, "1")},
            {L::CROSS, btn(290, "2")},    {L::SQUARE, btn(291, "3")},
            {L::L1, btn(292, "4")},       {L::R1, btn(293, "5")},
            {L::L2, btn(294, "6")},       {L::R2, btn(295, "7")},
            {L::SELECT, btn(296, "8")},   {L::START, btn(297, "9")},
            // PROVISIONAL / UNVERIFIED: assumes a real hat (ABS_HAT0X/Y).
            // input_manager.cpp:27-30,154-166 documents that THIS SAME
            // VID/PID (0079:0006) can instead report its d-pad via
            // ABS_X/Y extremes in an 8-bit (0..255) range, with no hat at
            // all, detected at runtime via ABS_X min==0 && max<=255. No
            // DragonRise pad was on hand when hardware-evidence.md was
            // captured, so which revision this shipped pad actually is
            // remains open -- Task 12's controller_probe resolves it
            // against the physical pad. If the 8-bit variant is
            // confirmed, these four bindings must change to AXIS on
            // ABS_X/Y instead of HAT.
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
