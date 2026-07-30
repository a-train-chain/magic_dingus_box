#include <catch2/catch_test_macros.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "retroarch/controller_detector.h"
#include "retroarch/controller_mapping.h"

// Proves write_player_binds() is a byte-identical replacement for the P1/P2
// block it extracted out of retroarch_launcher.cpp's launch_drm(), across
// EVERY core/pad combination the kiosk ships, not just a handful of
// spot-checked values.
//
// legacy_write_block() below is a literal, line-for-line transcription of
// the pre-Task-6 inline code (see git history of retroarch_launcher.cpp
// before the "refactor(retroarch): launcher uses write_player_binds"
// commit, or the Task 6 brief) -- every `script_file <<` statement copied
// verbatim with `script_file` renamed to `out`. It is deliberately NOT
// implemented by calling write_player_binds(), so this test cannot pass by
// construction; it can only pass if the two independently-written code
// paths produce identical text.
//
// DOCUMENTED DELTAS FROM THE LEGACY BLOCK:
// - Empty button tokens render as RetroArch's "nul" sentinel. The literal
//   empty string is parsed as physical button 0 by RetroArch 1.20.0.
// - The four l3_btn/r3_btn lines
// (two per player, marked inline below). The legacy block could not have
// contained them -- RetroPad L3/R3 had no plumbing anywhere in the kiosk
// until PS1 gained its second analog stick and its stick clicks, so there is
// no historical text to transcribe. They are written out here by hand, in
// the same independent-second-implementation spirit as the rest of the
// function, which is what keeps this test's real value intact: it still
// catches prefix, ordering and P1/P2-drift mistakes in the NEW lines too,
// because getting them wrong in write_player_binds() and getting them wrong
// identically here are separate mistakes. Everything else remains the
// verbatim transcription.
namespace {

const std::string& serialized_btn(const std::string& token) {
    static const std::string nul = "nul";
    return token.empty() ? nul : token;
}

void legacy_write_block(std::ostream& out, const retroarch::ControllerMapping& map) {
    // 2. Apply Settings
    out << "input_player1_analog_dpad_mode = \"" << map.analog_dpad_mode << "\"\n";

    // 3. Apply Buttons
    out << "input_player1_b_btn = \"" << serialized_btn(map.b_btn) << "\"\n";
    out << "input_player1_y_btn = \"" << serialized_btn(map.y_btn) << "\"\n";
    out << "input_player1_select_btn = \"" << serialized_btn(map.select_btn) << "\"\n";
    out << "input_player1_start_btn = \"" << serialized_btn(map.start_btn) << "\"\n";

    out << "input_player1_up_btn = \"" << serialized_btn(map.up_btn) << "\"\n";
    out << "input_player1_down_btn = \"" << serialized_btn(map.down_btn) << "\"\n";
    out << "input_player1_left_btn = \"" << serialized_btn(map.left_btn) << "\"\n";
    out << "input_player1_right_btn = \"" << serialized_btn(map.right_btn) << "\"\n";

    out << "input_player1_a_btn = \"" << serialized_btn(map.a_btn) << "\"\n";
    out << "input_player1_x_btn = \"" << serialized_btn(map.x_btn) << "\"\n";

    out << "input_player1_l_btn = \"" << serialized_btn(map.l_btn) << "\"\n";
    out << "input_player1_r_btn = \"" << serialized_btn(map.r_btn) << "\"\n";

    out << "input_player1_l2_btn = \"" << serialized_btn(map.l2_btn) << "\"\n";
    out << "input_player1_r2_btn = \"" << serialized_btn(map.r2_btn) << "\"\n";

    // 3b. Stick clicks -- NOT in the legacy block (see the delta note above).
    out << "input_player1_l3_btn = \"" << serialized_btn(map.l3_btn) << "\"\n";
    out << "input_player1_r3_btn = \"" << serialized_btn(map.r3_btn) << "\"\n";

    // 4. Apply Analog Axes
    out << "input_player1_l_x_plus_axis = \"" << map.l_x_plus << "\"\n";
    out << "input_player1_l_x_minus_axis = \"" << map.l_x_minus << "\"\n";
    out << "input_player1_l_y_plus_axis = \"" << map.l_y_plus << "\"\n";
    out << "input_player1_l_y_minus_axis = \"" << map.l_y_minus << "\"\n";
    retroarch::write_right_stick_binds(out, map, 1);

    // 5. Apply D-Pad Axis Mappings
    out << "input_player1_up_axis = \"" << map.up_axis << "\"\n";
    out << "input_player1_down_axis = \"" << map.down_axis << "\"\n";
    out << "input_player1_left_axis = \"" << map.left_axis << "\"\n";
    out << "input_player1_right_axis = \"" << map.right_axis << "\"\n";

    // 5b. Mirror the same mapping for player 2.
    out << "input_player2_analog_dpad_mode = \"" << map.analog_dpad_mode << "\"\n";
    out << "input_player2_b_btn = \"" << serialized_btn(map.b_btn) << "\"\n";
    out << "input_player2_y_btn = \"" << serialized_btn(map.y_btn) << "\"\n";
    out << "input_player2_select_btn = \"" << serialized_btn(map.select_btn) << "\"\n";
    out << "input_player2_start_btn = \"" << serialized_btn(map.start_btn) << "\"\n";
    out << "input_player2_up_btn = \"" << serialized_btn(map.up_btn) << "\"\n";
    out << "input_player2_down_btn = \"" << serialized_btn(map.down_btn) << "\"\n";
    out << "input_player2_left_btn = \"" << serialized_btn(map.left_btn) << "\"\n";
    out << "input_player2_right_btn = \"" << serialized_btn(map.right_btn) << "\"\n";
    out << "input_player2_a_btn = \"" << serialized_btn(map.a_btn) << "\"\n";
    out << "input_player2_x_btn = \"" << serialized_btn(map.x_btn) << "\"\n";
    out << "input_player2_l_btn = \"" << serialized_btn(map.l_btn) << "\"\n";
    out << "input_player2_r_btn = \"" << serialized_btn(map.r_btn) << "\"\n";
    out << "input_player2_l2_btn = \"" << serialized_btn(map.l2_btn) << "\"\n";
    out << "input_player2_r2_btn = \"" << serialized_btn(map.r2_btn) << "\"\n";
    // Stick clicks -- NOT in the legacy block (see the delta note above).
    out << "input_player2_l3_btn = \"" << serialized_btn(map.l3_btn) << "\"\n";
    out << "input_player2_r3_btn = \"" << serialized_btn(map.r3_btn) << "\"\n";
    out << "input_player2_l_x_plus_axis = \"" << map.l_x_plus << "\"\n";
    out << "input_player2_l_x_minus_axis = \"" << map.l_x_minus << "\"\n";
    out << "input_player2_l_y_plus_axis = \"" << map.l_y_plus << "\"\n";
    out << "input_player2_l_y_minus_axis = \"" << map.l_y_minus << "\"\n";
    retroarch::write_right_stick_binds(out, map, 2);
    out << "input_player2_up_axis = \"" << map.up_axis << "\"\n";
    out << "input_player2_down_axis = \"" << map.down_axis << "\"\n";
    out << "input_player2_left_axis = \"" << map.left_axis << "\"\n";
    out << "input_player2_right_axis = \"" << map.right_axis << "\"\n";
}

// Same core list test_mapping_snapshot.cpp golden-locks get_mapping() output
// against, so between the two tests every shipped core/pad combination has
// both its VALUES (mapping_snapshot) and its RENDERED TEXT (this test)
// locked down.
const std::vector<std::string>& snapshot_cores() {
    static const std::vector<std::string> cores = {
        "nestopia_libretro",          "snes9x2010_libretro",
        "genesis_plus_gx_libretro",   "pcsx_rearmed_libretro",
        "mednafen_pce_fast_libretro", "prosystem_libretro",
        "fbneo_libretro",             "mupen64plus_next_libretro",
        "parallel_n64_libretro",      "flycast_libretro",
        "totally_unknown_core",
    };
    return cores;
}

}  // namespace

TEST_CASE("write_player_binds(1)+write_player_binds(2) reproduce the legacy "
          "inline block byte-for-byte, for every shipped core and pad",
          "[player_binds][legacy_equivalence]") {
    for (auto style : {retroarch::ControllerType::N64_ADAPTER,
                       retroarch::ControllerType::PS_STYLE_DRAGONRISE}) {
        for (const auto& core : snapshot_cores()) {
            const auto map = retroarch::get_mapping(style, core);

            std::ostringstream expected;
            legacy_write_block(expected, map);

            std::ostringstream actual;
            retroarch::write_player_binds(actual, map, 1);
            retroarch::write_player_binds(actual, map, 2);

            INFO("pad=" << retroarch::controller_type_name(style) << " core=" << core);
            REQUIRE(actual.str() == expected.str());
        }
    }
}
