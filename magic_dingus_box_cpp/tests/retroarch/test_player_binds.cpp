#include <catch2/catch_test_macros.hpp>
#include <sstream>
#include "retroarch/controller_detector.h"
#include "retroarch/controller_mapping.h"

using namespace retroarch;

static std::string emit(ControllerType t, const std::string& core, int player) {
    std::ostringstream o;
    write_player_binds(o, get_mapping(t, core), player);
    return o.str();
}

TEST_CASE("player binds match the legacy launcher block line-for-line",
          "[player_binds]") {
    const std::string cfg = emit(ControllerType::PS_STYLE_DRAGONRISE,
                                 "pcsx_rearmed_libretro", 1);
    // Exact lines the launcher used to emit (spot-check the shape + a few values)
    REQUIRE(cfg.find("input_player1_analog_dpad_mode = \"0\"\n") != std::string::npos);
    REQUIRE(cfg.find("input_player1_b_btn = \"2\"\n") != std::string::npos);
    REQUIRE(cfg.find("input_player1_l2_btn = \"6\"\n") != std::string::npos);
    REQUIRE(cfg.find("input_player1_l_x_plus_axis = \"+0\"\n") != std::string::npos);
    REQUIRE(cfg.find("input_player1_up_axis = \"-1\"\n") != std::string::npos);
    // Unconditional emission: an empty value still writes the line
    const std::string nes = emit(ControllerType::N64_ADAPTER, "nestopia_libretro", 1);
    REQUIRE(nes.find("input_player1_l2_btn = \"\"\n") != std::string::npos);
}

TEST_CASE("player 2 mirrors with the player2 prefix and no player1 lines",
          "[player_binds]") {
    const std::string cfg = emit(ControllerType::N64_ADAPTER,
                                 "mupen64plus_next_libretro", 2);
    REQUIRE(cfg.find("input_player2_r_x_plus_btn = \"8\"") != std::string::npos);
    REQUIRE(cfg.find("player1") == std::string::npos);
}

TEST_CASE("two different mappings produce genuinely different P1/P2 blocks",
          "[player_binds]") {
    std::ostringstream o;
    write_player_binds(o, get_mapping(ControllerType::PS_STYLE_DRAGONRISE,
                                      "snes9x2010_libretro"), 1);
    write_player_binds(o, get_mapping(ControllerType::N64_ADAPTER,
                                      "snes9x2010_libretro"), 2);
    const std::string cfg = o.str();
    REQUIRE(cfg.find("input_player1_b_btn = \"2\"") != std::string::npos);  // Cross
    REQUIRE(cfg.find("input_player2_b_btn = \"1\"") != std::string::npos);  // N64 B
}
