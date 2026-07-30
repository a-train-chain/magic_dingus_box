// build_mapping() must route a captured binding by its KIND, not just by
// which semantic slot it fills.
//
// THE DEFECT THIS FILE PINS. build_mapping() used to write
// `m.<field> = profile.token(slot)` unconditionally: the semantic table fixed
// the FIELD (and therefore the form RetroArch expects) while the profile
// supplied the TOKEN, and nothing checked the two agreed. On the exact pad
// class the Controller Setup wizard exists to support -- an 8-bit pad whose
// d-pad is ABS_X/ABS_Y extremes with no hat at all, and whose L2/R2 are
// analog axes -- a capture came out as:
//
//     up_btn = "-1"   left_btn = "-0"   right_btn = "+0"   down_btn = "+1"
//     l2_btn = "+2"   r2_btn   = "+3"      up_axis = ""    left_axis = ""
//
// RetroArch's *_btn parser treats any value not starting with 'h' as a
// NUMERIC BUTTON INDEX, so "-0" and "+0" both resolve to physical button 0 --
// not inert, silently wrong, and pointing left and right at the same button.
// The wizard's TEST phase could not catch it either: binding_lit_() reads the
// captured PhysicalBinding (which is correctly AXIS-kind), so every one of
// those controls lit green while the config that would be emitted for them
// could not work.
//
// The synthetic pad below is that pad. The built-in cases at the bottom are
// the proof the fix changed nothing for the two pads the box actually ships
// with -- [mapping_snapshot] is the wider version of that same guarantee.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "retroarch/controller_mapping.h"
#include "retroarch/controller_profile.h"
#include "retroarch/logical_controls.h"

using retroarch::build_mapping;
using retroarch::ControllerStyle;
using retroarch::get_semantic_mapping;
using retroarch::LogicalControl;
using retroarch::PhysicalBinding;
using retroarch::PhysicalProfile;
using K = retroarch::PhysicalBinding::Kind;
using L = retroarch::LogicalControl;

namespace {

// evdev codes, hardcoded because linux/input-event-codes.h does not exist on
// macOS where this binary builds.
constexpr uint16_t kAbsX = 0x00, kAbsY = 0x01, kAbsZ = 0x02, kAbsRz = 0x05;

PhysicalBinding btn(uint16_t code, const char* tok) { return {K::BUTTON, code, 0, tok}; }
PhysicalBinding axis(uint16_t code, int dir, const char* tok) { return {K::AXIS, code, dir, tok}; }

// A DragonRise-class pad of the 8-bit revision: the d-pad IS ABS_X/ABS_Y
// (there is no hat on the device at all) and the shoulder triggers are
// analog. Tokens are exactly what CaptureSession/bind_token() produce for
// this capability list -- ABS_X/Y/Z/RZ are joydev axis indices 0/1/2/3.
//
// The left stick comes out UNBOUND on purpose: it shares its axis pair with
// the d-pad, so CaptureSession rejects LSTICK_* as duplicates and the user
// has no choice but to skip those four steps. That is correct duplicate
// detection, and it is why the d-pad's axis binds must survive the
// stick_to_dpad pass rather than being overwritten with "".
PhysicalProfile eight_bit_pad() {
    PhysicalProfile p;
    p.name = "8-bit PS-style pad (d-pad on ABS_X/Y, analog triggers)";
    p.style = ControllerStyle::PS_STYLE;
    p.vid = 0x0079;
    p.pid = 0x0006;
    p.controls = {
        {L::DPAD_UP,    axis(kAbsY, -1, "-1")},
        {L::DPAD_DOWN,  axis(kAbsY, +1, "+1")},
        {L::DPAD_LEFT,  axis(kAbsX, -1, "-0")},
        {L::DPAD_RIGHT, axis(kAbsX, +1, "+0")},
        {L::CROSS,    btn(290, "2")}, {L::CIRCLE,   btn(289, "1")},
        {L::SQUARE,   btn(291, "3")}, {L::TRIANGLE, btn(288, "0")},
        {L::L1,       btn(292, "4")}, {L::R1,       btn(293, "5")},
        {L::L2, axis(kAbsZ, +1, "+2")}, {L::R2, axis(kAbsRz, +1, "+3")},
        {L::SELECT,   btn(296, "8")}, {L::START,    btn(297, "9")},
    };
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------
// The reported defect
// ---------------------------------------------------------------------------

TEST_CASE("an AXIS d-pad binds the *_axis fields, never *_btn",
          "[build_mapping][kinds]") {
    const auto p = eight_bit_pad();
    // Two cores with opposite stick_to_dpad settings: pcsx sets it (so the
    // stick pass runs and would clobber), mupen64plus clears it.
    for (const char* core : {"pcsx_rearmed_libretro", "snes9x2010_libretro",
                             "mupen64plus_next_libretro"}) {
        INFO("core: " << core);
        const auto m = build_mapping(
            get_semantic_mapping(ControllerStyle::PS_STYLE, core), p);

        // No axis token may sit in a _btn field. "-0"/"+0" there would BOTH
        // resolve to physical button 0.
        CHECK(m.up_btn == "");
        CHECK(m.down_btn == "");
        CHECK(m.left_btn == "");
        CHECK(m.right_btn == "");

        // ...and the d-pad is genuinely bound, in the field that can hold it.
        CHECK(m.up_axis == "-1");
        CHECK(m.down_axis == "+1");
        CHECK(m.left_axis == "-0");
        CHECK(m.right_axis == "+0");
    }
}

TEST_CASE("an analog trigger never produces a bogus l2_btn/r2_btn",
          "[build_mapping][kinds]") {
    // pcsx_rearmed is the only PS-style core that binds L2/R2 at all, which
    // is what made this reachable: "+2" in l2_btn reads as button 2 -- the
    // same physical button as CROSS on this pad.
    const auto m = build_mapping(
        get_semantic_mapping(ControllerStyle::PS_STYLE, "pcsx_rearmed_libretro"),
        eight_bit_pad());
    CHECK(m.l2_btn == "");
    CHECK(m.r2_btn == "");
    // The pad's real buttons are untouched -- this is not "clear everything".
    CHECK(m.b_btn == "2");    // CROSS
    CHECK(m.a_btn == "1");    // CIRCLE
    CHECK(m.l_btn == "4");    // L1
    CHECK(m.r_btn == "5");    // R1
    CHECK(m.select_btn == "8");
    CHECK(m.start_btn == "9");
    // ...including the hotkey pair, whose absence disables the RetroArch menu
    // combo entirely (retroarch_launcher.cpp skips the whole block).
    CHECK(m.enable_hotkey_btn == "8");
    CHECK(m.menu_toggle_btn == "9");
}

TEST_CASE("an unbound shared left stick cannot erase the d-pad axis binds",
          "[build_mapping][kinds]") {
    // stick_to_dpad is on for pcsx_rearmed and LSTICK_* is unbound (rejected
    // as a duplicate of the d-pad during capture). Written naively, that pass
    // assigns "" over the four *_axis fields and the pad ends up with no
    // d-pad in ANY field.
    const auto m = build_mapping(
        get_semantic_mapping(ControllerStyle::PS_STYLE, "pcsx_rearmed_libretro"),
        eight_bit_pad());
    CHECK(m.l_x_plus == "");     // stick genuinely unbound: honest
    CHECK(m.l_x_minus == "");
    CHECK(m.up_axis == "-1");    // d-pad survives
    CHECK(m.right_axis == "+0");
}

TEST_CASE("a BUTTON captured for a stick slot does not land in an axis field",
          "[build_mapping][kinds]") {
    // The mirror of the d-pad case. RetroArch's axis parser wants "+N"/"-N";
    // a plain button index there fails to parse, so the bind is already dead
    // -- emitting "" says so instead of leaving a value that reads as
    // configured. (A button CAN drive an analog bind, but only through the
    // _btn form, which is what r_*_btn exists for.)
    PhysicalProfile p = eight_bit_pad();
    p.controls[L::LSTICK_UP] = btn(298, "10");
    p.controls[L::LSTICK_DOWN] = btn(299, "11");
    const auto m = build_mapping(
        get_semantic_mapping(ControllerStyle::PS_STYLE, "pcsx_rearmed_libretro"), p);
    CHECK(m.l_y_minus == "");
    CHECK(m.l_y_plus == "");
}

// ---------------------------------------------------------------------------
// The two built-in pads must be bit-identical
// ---------------------------------------------------------------------------

TEST_CASE("kind-awareness changes nothing for the two built-in profiles",
          "[build_mapping][kinds]") {
    // Both built-ins bind every *_btn slot to a BUTTON and every d-pad slot
    // to a HAT, so the kind check is a no-op for them by construction. Spot
    // checks here; [mapping_snapshot]'s 33 goldens are the exhaustive form.
    const auto n64 = build_mapping(
        get_semantic_mapping(ControllerStyle::N64_STYLE, "pcsx_rearmed_libretro"),
        retroarch::builtin_n64_adapter_profile());
    CHECK(n64.up_btn == "h0up");
    CHECK(n64.down_btn == "h0down");
    CHECK(n64.left_btn == "h0left");
    CHECK(n64.right_btn == "h0right");
    CHECK(n64.up_axis.empty());         // native d-pad stays independent
    CHECK(n64.r2_btn == "8");          // C-Right, a real BUTTON
    CHECK(n64.enable_hotkey_btn == "6");
    CHECK(n64.menu_toggle_btn == "12");

    const auto ps = build_mapping(
        get_semantic_mapping(ControllerStyle::PS_STYLE, "pcsx_rearmed_libretro"),
        retroarch::builtin_dragonrise_profile());
    CHECK(ps.up_btn == "h0up");
    CHECK(ps.right_btn == "h0right");
    CHECK(ps.l2_btn == "6");
    CHECK(ps.r2_btn == "7");
    CHECK(ps.up_axis == "-1");
    CHECK(ps.l_x_plus == "+0");
}

TEST_CASE("a HAT d-pad still reaches the *_btn fields on a captured profile",
          "[build_mapping][kinds]") {
    // The other DragonRise revision -- real hat, same VID/PID. Whichever
    // revision the shipped pad turns out to be, the wizard must produce a
    // working config for it; this is the branch the 8-bit test above is not.
    PhysicalProfile p = eight_bit_pad();
    p.controls[L::DPAD_UP] = {K::HAT, 0x11, -1, "h0up"};
    p.controls[L::DPAD_DOWN] = {K::HAT, 0x11, +1, "h0down"};
    p.controls[L::DPAD_LEFT] = {K::HAT, 0x10, -1, "h0left"};
    p.controls[L::DPAD_RIGHT] = {K::HAT, 0x10, +1, "h0right"};
    p.controls[L::LSTICK_UP] = axis(kAbsY, -1, "-1");
    p.controls[L::LSTICK_DOWN] = axis(kAbsY, +1, "+1");
    p.controls[L::LSTICK_LEFT] = axis(kAbsX, -1, "-0");
    p.controls[L::LSTICK_RIGHT] = axis(kAbsX, +1, "+0");
    const auto m = build_mapping(
        get_semantic_mapping(ControllerStyle::PS_STYLE, "pcsx_rearmed_libretro"), p);
    CHECK(m.up_btn == "h0up");
    CHECK(m.right_btn == "h0right");
    // ...and with a real, distinct stick the stick_to_dpad binds stand.
    CHECK(m.up_axis == "-1");
    CHECK(m.l_x_plus == "+0");
}
