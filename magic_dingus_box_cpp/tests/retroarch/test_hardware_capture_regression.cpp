#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "retroarch/controller_detector.h"
#include "retroarch/controller_mapping.h"
#include "retroarch/controller_profile.h"

// ============================================================================
// GROUND TRUTH SOURCE: a real capture from PRODUCTION HARDWARE, not a
// hand-written expectation.
//
//   Pad:  SWITCH CO.,LTD. Controller (Dinput), VID:PID 2563:0575 -- NOT in
//         match_vid_pid()'s whitelist (see controller_detector.cpp), so it
//         resolves to ControllerType::UNKNOWN and rides the legacy
//         N64-adapter fallback. This is the common real-world case on
//         fielded boxes, not a contrived edge case.
//   Core: snes9x2010_libretro
//   Date: 2026-07-29, captured from a running game launch on a fielded
//         Magic Dingus Box's generated /home/magic/retroarch_launcher.sh,
//         saved verbatim at .superpowers/sdd/live-launcher-binds.txt.
//   Only that one pad was connected, so player 2 mirrored player 1.
//
// Task 7 (commit eb32c4e, "per-port controller mapping resolution at
// launch") made player 1 and player 2 resolve their RetroArch button
// mappings independently, one per connected controller port. Its entire
// risk surface is regressions on already-fielded boxes. This test pins the
// exact runtime text the SHIPPED (pre-Task-7) code produced for this real
// pad/core combination, so any future change to resolve_port_mappings(),
// resolve_mapping_for_pad(), get_mapping(), or write_player_binds() that
// would alter the launcher's output for this fielded configuration fails
// here first -- permanently, on every run, not just as a one-time manual
// diff against a saved file.
// ============================================================================

namespace {

// The exact VID/PID observed on the fielded hardware during capture.
constexpr uint16_t kCapturedVid = 0x2563;
constexpr uint16_t kCapturedPid = 0x0575;
constexpr char kCapturedCore[] = "snes9x2010_libretro";
constexpr char kCapturedPadName[] = "SWITCH CO.,LTD. Controller (Dinput)";

// Verbatim from .superpowers/sdd/live-launcher-binds.txt (the 44
// input_player1_*/input_player2_* lines only -- the trailing "---CORE---"
// marker and the 3 non-bind lines after it belong to a different part of
// the launcher and are not write_player_binds()'s output, so they are
// excluded here).
//
// EXCLUDED-THEN-RECONSTRUCTED LINES: the raw capture's player-1 half starts
// at "input_player1_y_btn" -- it is missing
// "input_player1_analog_dpad_mode" and "input_player1_b_btn", the two
// lines write_player_binds() always writes FIRST for every player
// (controller_mapping.cpp, write_player_binds()). This is a gap in the
// capture, not a real difference in what the code emits: write_player_binds()
// is unconditional and prefix-identical for both players (confirmed by
// reading its source, and independently pinned by
// test_player_binds_legacy_equivalence.cpp), and only one pad was connected
// during this capture, so per resolve_port_mappings()'s one-pad-mirrors
// contract player 1 and player 2 MUST be byte-identical. The capture's own
// player-2 half (which IS complete) has
// `input_player2_analog_dpad_mode = "0"` and `input_player2_b_btn = "1"` --
// so the two lines below are that same real capture, read via its
// untruncated other half, not an invented expectation.
constexpr char kExpectedBinds[] =
    "input_player1_analog_dpad_mode = \"0\"\n"  // reconstructed -- see note above
    "input_player1_b_btn = \"1\"\n"              // reconstructed -- see note above
    "input_player1_y_btn = \"3\"\n"
    "input_player1_select_btn = \"10\"\n"
    "input_player1_start_btn = \"12\"\n"
    "input_player1_up_btn = \"h0up\"\n"
    "input_player1_down_btn = \"h0down\"\n"
    "input_player1_left_btn = \"h0left\"\n"
    "input_player1_right_btn = \"h0right\"\n"
    "input_player1_a_btn = \"2\"\n"
    "input_player1_x_btn = \"0\"\n"
    "input_player1_l_btn = \"4\"\n"
    "input_player1_r_btn = \"5\"\n"
    "input_player1_l2_btn = \"\"\n"
    "input_player1_r2_btn = \"\"\n"
    "input_player1_l_x_plus_axis = \"+0\"\n"
    "input_player1_l_x_minus_axis = \"-0\"\n"
    "input_player1_l_y_plus_axis = \"+1\"\n"
    "input_player1_l_y_minus_axis = \"-1\"\n"
    "input_player1_up_axis = \"-1\"\n"
    "input_player1_down_axis = \"+1\"\n"
    "input_player1_left_axis = \"-0\"\n"
    "input_player1_right_axis = \"+0\"\n"
    "input_player2_analog_dpad_mode = \"0\"\n"
    "input_player2_b_btn = \"1\"\n"
    "input_player2_y_btn = \"3\"\n"
    "input_player2_select_btn = \"10\"\n"
    "input_player2_start_btn = \"12\"\n"
    "input_player2_up_btn = \"h0up\"\n"
    "input_player2_down_btn = \"h0down\"\n"
    "input_player2_left_btn = \"h0left\"\n"
    "input_player2_right_btn = \"h0right\"\n"
    "input_player2_a_btn = \"2\"\n"
    "input_player2_x_btn = \"0\"\n"
    "input_player2_l_btn = \"4\"\n"
    "input_player2_r_btn = \"5\"\n"
    "input_player2_l2_btn = \"\"\n"
    "input_player2_r2_btn = \"\"\n"
    "input_player2_l_x_plus_axis = \"+0\"\n"
    "input_player2_l_x_minus_axis = \"-0\"\n"
    "input_player2_l_y_plus_axis = \"+1\"\n"
    "input_player2_l_y_minus_axis = \"-1\"\n"
    "input_player2_up_axis = \"-1\"\n"
    "input_player2_down_axis = \"+1\"\n"
    "input_player2_left_axis = \"-0\"\n"
    "input_player2_right_axis = \"+0\"\n";

}  // namespace

TEST_CASE(
    "resolve_port_mappings() + write_player_binds() reproduce the real "
    "production-hardware capture byte-for-byte",
    "[retroarch][hardware_capture][port_resolution][player_binds]") {
    // One pad detected on port 0, matching exactly what was plugged into
    // the fielded box during capture. fallback_type is irrelevant here --
    // resolve_port_mappings() only consults it on the zero-pads path -- but
    // ControllerType::UNKNOWN is what match_vid_pid(0x2563, 0x0575) itself
    // returns, so it also matches reality.
    const std::vector<retroarch::DetectedPad> pads = {
        {0, kCapturedVid, kCapturedPid, kCapturedPadName},
    };
    // No captured profile existed for this VID/PID on the fielded box --
    // that's precisely why it rode the legacy N64-adapter fallback instead
    // of a captured or built-in profile.
    const std::map<std::string, retroarch::PhysicalProfile> store;

    const auto resolved = retroarch::resolve_port_mappings(
        pads, retroarch::ControllerType::UNKNOWN, store, kCapturedCore);

    std::ostringstream actual;
    retroarch::write_player_binds(actual, resolved.p1, 1);
    retroarch::write_player_binds(actual, resolved.p2, 2);

    REQUIRE(actual.str() == kExpectedBinds);
}
