// PS1's second analog stick and its two stick-click buttons (L3/R3).
//
// THE DEFECT THIS FILE PINS. A PlayStation-style pad captured through the
// Controller Setup wizard yields 24 controls -- both sticks, L3/R3, and a
// real hat -- but only 18 of them ever reached a PS1 game:
//
//   * semantic_ps_style()'s PS1 branch never set r_up/r_down/r_left/r_right,
//     so build_mapping()'s right-stick block had nothing to resolve and the
//     four r_*_axis fields stayed empty. pcsx_rearmed runs with
//     core_option_pad_type = "analog", i.e. the emulated console believes it
//     has a DualShock -- so the game polled a right stick that the config
//     never bound. Dual-analog titles (Ape Escape is built entirely around
//     both sticks) were unplayable and right-stick camera games had no
//     camera. Only the two N64 branches had ever populated these fields.
//
//   * L3/R3 were worse: the plumbing did not exist at all. ControllerMapping
//     had no l3_btn/r3_btm fields and write_player_binds() emitted no l3/r3
//     lines, so no pad on ANY core could reach a stick click. RetroArch
//     exposes them as input_playerN_l3_btn / input_playerN_r3_btn (RetroPad
//     L3/R3), which pcsx_rearmed maps to the DualShock's stick clicks.
//
// Assertions here are written against the EMITTED CONFIG TEXT rather than
// against ControllerMapping's fields, deliberately: the text is what the
// running box acts on, and phrasing them this way let every one of them fail
// against the pre-fix build instead of failing to compile.
//
// SCOPE GUARD. The second test case is the one that matters most for the
// consoles this fix must NOT touch. NES, SNES, Genesis, Atari 7800, PC
// Engine and Arcade had neither a second stick nor stick clicks; the N64 has
// no stick clicks and already spends its right stick on the C-button
// cluster; the Dreamcast has one stick plus two analog triggers. All of them
// must keep emitting EMPTY l3/r3 lines -- present, because RetroArch
// distinguishes an empty value from an absent line, but empty.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "retroarch/controller_detector.h"
#include "retroarch/controller_mapping.h"
#include "retroarch/controller_profile.h"
#include "retroarch/logical_controls.h"

using retroarch::ControllerStyle;
using retroarch::ControllerType;
using retroarch::get_mapping;
using retroarch::PhysicalBinding;
using retroarch::PhysicalProfile;
using retroarch::write_player_binds;
using K = retroarch::PhysicalBinding::Kind;
using L = retroarch::LogicalControl;

namespace {

// evdev codes, hardcoded because linux/input-event-codes.h does not exist on
// macOS where this binary builds. Same convention as test_mapping_kinds.cpp.
constexpr uint16_t kAbsX = 0x00, kAbsY = 0x01, kAbsZ = 0x02, kAbsRz = 0x05;
constexpr uint16_t kAbsHat0X = 0x10, kAbsHat0Y = 0x11;

PhysicalBinding btn(uint16_t code, const char* tok) { return {K::BUTTON, code, 0, tok}; }
PhysicalBinding hat(uint16_t code, int dir, const char* tok) { return {K::HAT, code, dir, tok}; }
PhysicalBinding axis(uint16_t code, int dir, const char* tok) { return {K::AXIS, code, dir, tok}; }

// The pad this bug was found on: a SHANWAN "Android Gamepad", VID:PID
// 2563:0526, captured through the Controller Setup wizard on real hardware.
// All 24 PS-style controls came out bound -- four d-pad directions on a real
// hat, four face buttons, L1/R1/L2/R2, L3/R3, Select/Start, and both analog
// sticks.
//
// PROVENANCE OF THE TOKENS. The two sets this test actually asserts on are
// the ones the hardware capture reported: L3 = "13" / R3 = "14", and the
// right stick on axis indices 2 (X) and 3 (Y), i.e. "-2"/"+2" and
// "-3"/"+3". The remaining tokens are a self-consistent stand-in in the
// usual Android-HID shape (contiguous indices with the gaps that layout
// leaves) -- no test below depends on their exact values, only on their
// being distinguishable from the built-in DragonRise profile's so a passing
// assertion proves the captured profile really was consulted.
PhysicalProfile shanwan_profile() {
    PhysicalProfile p;
    p.name = "SHANWAN Android Gamepad";
    p.style = ControllerStyle::PS_STYLE;
    p.vid = 0x2563;
    p.pid = 0x0526;
    p.controls = {
        {L::CROSS, btn(304, "0")},    {L::CIRCLE, btn(305, "1")},
        {L::SQUARE, btn(308, "3")},   {L::TRIANGLE, btn(307, "4")},
        {L::L1, btn(310, "6")},       {L::R1, btn(311, "7")},
        {L::L2, btn(312, "8")},       {L::R2, btn(313, "9")},
        {L::SELECT, btn(314, "10")},  {L::START, btn(315, "11")},
        // The two controls with no plumbing before this change.
        {L::L3, btn(317, "13")},      {L::R3, btn(318, "14")},
        {L::DPAD_UP, hat(kAbsHat0Y, -1, "h0up")},
        {L::DPAD_DOWN, hat(kAbsHat0Y, +1, "h0down")},
        {L::DPAD_LEFT, hat(kAbsHat0X, -1, "h0left")},
        {L::DPAD_RIGHT, hat(kAbsHat0X, +1, "h0right")},
        {L::LSTICK_UP, axis(kAbsY, -1, "-1")},
        {L::LSTICK_DOWN, axis(kAbsY, +1, "+1")},
        {L::LSTICK_LEFT, axis(kAbsX, -1, "-0")},
        {L::LSTICK_RIGHT, axis(kAbsX, +1, "+0")},
        // Right stick: joydev axis index 2 (X) and 3 (Y).
        {L::RSTICK_LEFT, axis(kAbsZ, -1, "-2")},
        {L::RSTICK_RIGHT, axis(kAbsZ, +1, "+2")},
        {L::RSTICK_UP, axis(kAbsRz, -1, "-3")},
        {L::RSTICK_DOWN, axis(kAbsRz, +1, "+3")},
    };
    return p;
}

// A partially-captured pad of the same shape: identical to the SHANWAN above
// except the operator skipped the two stick-click steps (both are optional
// per required_controls(), so the wizard will happily persist this). Used to
// prove a missing capture degrades to an empty bind rather than to a token
// borrowed from somewhere else.
PhysicalProfile shanwan_profile_without_stick_clicks() {
    PhysicalProfile p = shanwan_profile();
    p.controls.erase(L::L3);
    p.controls.erase(L::R3);
    return p;
}

std::map<std::string, PhysicalProfile> store_of(const PhysicalProfile& p) {
    return {{retroarch::vidpid_key(p.vid, p.pid), p}};
}

// The config text the launcher would write for this pad on this core.
std::string emit_for(const PhysicalProfile& p, const std::string& core,
                     int player = 1) {
    std::ostringstream o;
    write_player_binds(
        o, retroarch::resolve_mapping_for_pad(p.vid, p.pid, store_of(p), core),
        player);
    return o.str();
}

std::string emit_builtin(ControllerType t, const std::string& core,
                         int player = 1) {
    std::ostringstream o;
    write_player_binds(o, get_mapping(t, core), player);
    return o.str();
}

bool has_line(const std::string& cfg, const std::string& line) {
    return cfg.find(line) != std::string::npos;
}

// Every core the box ships, minus PS1 -- the consoles that must be left
// alone. Same core-name list test_mapping_snapshot.cpp golden-locks, so
// this stays in step with the snapshot's coverage.
const std::vector<std::string>& non_ps1_cores() {
    static const std::vector<std::string> cores = {
        "nestopia_libretro",          "snes9x2010_libretro",
        "genesis_plus_gx_libretro",   "mednafen_pce_fast_libretro",
        "prosystem_libretro",         "fbneo_libretro",
        "mupen64plus_next_libretro",  "parallel_n64_libretro",
        "flycast_libretro",           "totally_unknown_core",
    };
    return cores;
}

}  // namespace

TEST_CASE("PS1 on a PS-style pad binds the captured right stick and L3/R3",
          "[ps1_analog][player_binds]") {
    const std::string cfg = emit_for(shanwan_profile(), "pcsx_rearmed_libretro");

    // The right stick, in AXIS form -- the pad's own captured tokens. X on
    // axis 2, Y on axis 3; plus = right/down, minus = left/up, matching
    // build_mapping()'s right-stick block.
    CHECK(has_line(cfg, "input_player1_r_x_plus_axis = \"+2\"\n"));
    CHECK(has_line(cfg, "input_player1_r_x_minus_axis = \"-2\"\n"));
    CHECK(has_line(cfg, "input_player1_r_y_plus_axis = \"+3\"\n"));
    CHECK(has_line(cfg, "input_player1_r_y_minus_axis = \"-3\"\n"));

    // The stick clicks. RetroPad L3/R3; pcsx_rearmed maps these to the
    // DualShock's stick-click buttons.
    CHECK(has_line(cfg, "input_player1_l3_btn = \"13\"\n"));
    CHECK(has_line(cfg, "input_player1_r3_btn = \"14\"\n"));

    // The BUTTON form of the right stick stays out of it: this pad has a
    // real second stick, and emitting both forms would bind the same
    // RetroPad function twice.
    CHECK_FALSE(has_line(cfg, "input_player1_r_x_plus_btn"));
    CHECK_FALSE(has_line(cfg, "input_player1_r_y_plus_btn"));

    // Nothing about the rest of the pad moved.
    CHECK(has_line(cfg, "input_player1_b_btn = \"0\"\n"));      // Cross
    CHECK(has_line(cfg, "input_player1_l2_btn = \"8\"\n"));
    CHECK(has_line(cfg, "input_player1_l_x_plus_axis = \"+0\"\n"));
}

TEST_CASE("consoles with no stick clicks keep emitting EMPTY l3/r3",
          "[ps1_analog][player_binds]") {
    // The scope guard. Every one of these consoles genuinely lacks stick
    // clicks, so the lines must be PRESENT (RetroArch distinguishes an empty
    // value from an absent line) and EMPTY. A token leaking in here would
    // mean this fix escaped PS1.
    for (const auto& core : non_ps1_cores()) {
        INFO("core=" << core);
        const std::string cfg = emit_for(shanwan_profile(), core);
        CHECK(has_line(cfg, "input_player1_l3_btn = \"\"\n"));
        CHECK(has_line(cfg, "input_player1_r3_btn = \"\"\n"));
    }
    // Same for the two built-in pads, across every shipped core, on both
    // players -- this is the path a box with no captured profile takes.
    for (auto pad : {ControllerType::N64_ADAPTER,
                     ControllerType::PS_STYLE_DRAGONRISE,
                     ControllerType::UNKNOWN}) {
        for (const auto& core : non_ps1_cores()) {
            INFO("pad=" << retroarch::controller_type_name(pad) << " core=" << core);
            for (int player : {1, 2}) {
                const std::string cfg = emit_builtin(pad, core, player);
                const std::string p = "input_player" + std::to_string(player) + "_";
                CHECK(has_line(cfg, p + "l3_btn = \"\"\n"));
                CHECK(has_line(cfg, p + "r3_btn = \"\"\n"));
            }
        }
    }
}

TEST_CASE("N64 on a PS-style pad keeps the right stick on the C cluster",
          "[ps1_analog][player_binds]") {
    // The N64's four C buttons live on the RetroPad right stick by core
    // convention. That mapping was already correct and hardware-verified;
    // this asserts the PS1 fix did not disturb it, and that the N64 gained
    // no stick clicks (the console has none).
    for (const auto& core : {"mupen64plus_next_libretro", "parallel_n64_libretro"}) {
        INFO("core=" << core);
        const std::string cfg = emit_for(shanwan_profile(), core);
        CHECK(has_line(cfg, "input_player1_r_x_plus_axis = \"+2\"\n"));
        CHECK(has_line(cfg, "input_player1_r_x_minus_axis = \"-2\"\n"));
        CHECK(has_line(cfg, "input_player1_r_y_plus_axis = \"+3\"\n"));
        CHECK(has_line(cfg, "input_player1_r_y_minus_axis = \"-3\"\n"));
        CHECK(has_line(cfg, "input_player1_l3_btn = \"\"\n"));
        CHECK(has_line(cfg, "input_player1_r3_btn = \"\"\n"));

        // And on the built-in DragonRise, whose golden snapshot entry pins
        // these same four tokens.
        const std::string builtin =
            emit_builtin(ControllerType::PS_STYLE_DRAGONRISE, core);
        CHECK(has_line(builtin, "input_player1_r_x_plus_axis = \"+2\"\n"));
        CHECK(has_line(builtin, "input_player1_r_y_minus_axis = \"-3\"\n"));
    }
}

TEST_CASE("a profile with no L3/R3 captured degrades to an empty bind",
          "[ps1_analog][player_binds]") {
    // A partially-captured profile must not produce a garbage token. Both
    // stick clicks are optional in required_controls(), so this profile is
    // persistable and a fielded box can genuinely be in this state.
    const std::string cfg =
        emit_for(shanwan_profile_without_stick_clicks(), "pcsx_rearmed_libretro");
    CHECK(has_line(cfg, "input_player1_l3_btn = \"\"\n"));
    CHECK(has_line(cfg, "input_player1_r3_btn = \"\"\n"));
    // The right stick WAS captured on this pad, so it is still bound: a
    // missing stick click degrades only itself.
    CHECK(has_line(cfg, "input_player1_r_x_plus_axis = \"+2\"\n"));

    // The built-in DragonRise profile is the same case from the other
    // direction: it has a real right stick but has never had L3/R3, so PS1
    // on the shipped pad gets the stick and empty clicks.
    const std::string builtin = emit_builtin(ControllerType::PS_STYLE_DRAGONRISE,
                                             "pcsx_rearmed_libretro");
    CHECK(has_line(builtin, "input_player1_r_x_plus_axis = \"+2\"\n"));
    CHECK(has_line(builtin, "input_player1_r_x_minus_axis = \"-2\"\n"));
    CHECK(has_line(builtin, "input_player1_r_y_plus_axis = \"+3\"\n"));
    CHECK(has_line(builtin, "input_player1_r_y_minus_axis = \"-3\"\n"));
    CHECK(has_line(builtin, "input_player1_l3_btn = \"\"\n"));
    CHECK(has_line(builtin, "input_player1_r3_btn = \"\"\n"));
}

TEST_CASE("both players get the l3/r3 lines", "[ps1_analog][player_binds]") {
    // There is a documented history of the P2 block drifting out of sync and
    // silently losing the right stick (that is why write_player_binds()
    // exists at all). Over a third of the box's library is two-player, so a
    // P2 without the new binds is the same bug again.
    for (int player : {1, 2}) {
        INFO("player=" << player);
        const std::string cfg =
            emit_for(shanwan_profile(), "pcsx_rearmed_libretro", player);
        const std::string p = "input_player" + std::to_string(player) + "_";
        CHECK(has_line(cfg, p + "l3_btn = \"13\"\n"));
        CHECK(has_line(cfg, p + "r3_btn = \"14\"\n"));
        CHECK(has_line(cfg, p + "r_x_plus_axis = \"+2\"\n"));
        CHECK(has_line(cfg, p + "r_y_minus_axis = \"-3\"\n"));
    }

    // The two blocks together, as the launcher writes them: P1 and P2 must
    // each carry their own copy and neither may leak the other's prefix.
    std::ostringstream both;
    const auto p = shanwan_profile();
    const auto map = retroarch::resolve_mapping_for_pad(
        p.vid, p.pid, store_of(p), "pcsx_rearmed_libretro");
    write_player_binds(both, map, 1);
    write_player_binds(both, map, 2);
    const std::string cfg = both.str();
    CHECK(has_line(cfg, "input_player1_l3_btn = \"13\"\n"));
    CHECK(has_line(cfg, "input_player2_l3_btn = \"13\"\n"));
    CHECK(has_line(cfg, "input_player1_r3_btn = \"14\"\n"));
    CHECK(has_line(cfg, "input_player2_r3_btn = \"14\"\n"));

    // Exactly one l3 line per player, i.e. the field is emitted once and not
    // duplicated by a stray copy of the block.
    size_t count = 0;
    for (size_t at = cfg.find("_l3_btn"); at != std::string::npos;
         at = cfg.find("_l3_btn", at + 1)) {
        ++count;
    }
    CHECK(count == 2);
}
