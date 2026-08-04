#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <vector>

#include "retroarch/controller_detector.h"
#include "retroarch/controller_mapping.h"

// Legacy 0e6d:111d adapter's physical button IDs. Numeric token ordering
// from another N64-style USB VID/PID does not establish these face labels:
//   0=C-Left, 1=B, 2=A, 3=C-Down, 4=L shoulder, 5=R shoulder,
//   6=Z trigger, 8=C-Right, 9=C-Up, 12=Start
//   Axes: 0/1 = analog stick, hat0 = D-pad

using retroarch::ControllerType;
using retroarch::get_mapping;

TEST_CASE("N64 games on the legacy N64 adapter use native semantic slots",
          "[retroarch][mapping][n64]") {
    for (const auto& core : {"mupen64plus_next_libretro",
                             "parallel_n64_libretro"}) {
        INFO("core=" << core);
        const auto map = get_mapping(ControllerType::N64_ADAPTER, core);

        CHECK(map.b_btn == "2");       // physical A -> native A
        CHECK(map.y_btn == "1");       // physical B -> native B
        CHECK(map.a_btn == "3");       // C-Down
        CHECK(map.x_btn == "9");       // C-Up
        CHECK(map.l_btn == "0");       // C-Left
        CHECK(map.r_btn == "8");       // C-Right
        CHECK(map.l2_btn == "6");      // Z
        CHECK(map.r2_btn == "5");      // R shoulder
        CHECK(map.select_btn == "4");  // L shoulder
        CHECK(map.start_btn == "12");

        CHECK(map.r_x_plus.empty());
        CHECK(map.r_x_minus.empty());
        CHECK(map.r_y_plus.empty());
        CHECK(map.r_y_minus.empty());
        CHECK(map.r_x_plus_btn.empty());
        CHECK(map.r_x_minus_btn.empty());
        CHECK(map.r_y_plus_btn.empty());
        CHECK(map.r_y_minus_btn.empty());

        // Z (6) held + Start (12) = direct QUIT; menu toggle dropped.
        CHECK(map.enable_hotkey_btn == "6");
        CHECK(map.menu_toggle_btn.empty());
        CHECK(map.exit_emulator_btn == "12");
    }
}

TEST_CASE("N64 adapter uses the universal layer-free PS1 layout",
          "[retroarch][mapping][ps1][n64_style]") {
    for (const auto& core : {"pcsx_rearmed_libretro",
                             "beetle_psx_libretro",
                             "swanstation_libretro"}) {
        INFO("core=" << core);
        const auto map = get_mapping(ControllerType::N64_ADAPTER, core);

        CHECK(map.name == "PS1 (N64 Controller)");
        CHECK(map.core_option_pad_type == "analog");
        CHECK(map.analog_dpad_mode == "0");

        CHECK(map.b_btn == "2");       // N64 A      -> Cross
        CHECK(map.y_btn == "1");       // N64 B      -> Square
        CHECK(map.x_btn == "0");       // C-Left     -> Triangle
        CHECK(map.a_btn == "3");       // C-Down     -> Circle
        CHECK(map.l_btn == "4");       // L           -> L1
        CHECK(map.r_btn == "5");       // R           -> R1
        CHECK(map.l2_btn == "6");      // Z           -> L2
        CHECK(map.r2_btn == "8");      // C-Right     -> R2
        CHECK(map.select_btn == "9");  // C-Up        -> Select
        CHECK(map.start_btn == "12");

        CHECK(map.up_btn == "h0up");
        CHECK(map.down_btn == "h0down");
        CHECK(map.left_btn == "h0left");
        CHECK(map.right_btn == "h0right");
        CHECK(map.l_x_plus == "+0");
        CHECK(map.l_x_minus == "-0");
        CHECK(map.l_y_plus == "+1");
        CHECK(map.l_y_minus == "-1");

        CHECK(map.up_axis.empty());
        CHECK(map.down_axis.empty());
        CHECK(map.left_axis.empty());
        CHECK(map.right_axis.empty());
        CHECK(map.r_x_plus.empty());
        CHECK(map.r_x_minus.empty());
        CHECK(map.r_y_plus.empty());
        CHECK(map.r_y_minus.empty());
        CHECK(map.r_x_plus_btn.empty());
        CHECK(map.r_x_minus_btn.empty());
        CHECK(map.r_y_plus_btn.empty());
        CHECK(map.r_y_minus_btn.empty());
        CHECK(map.l3_btn.empty());
        CHECK(map.r3_btn.empty());
        CHECK(map.enable_hotkey_btn == "6");
        CHECK(map.menu_toggle_btn.empty());
        CHECK(map.exit_emulator_btn == "12");

        for (int player : {1, 2}) {
            INFO("player=" << player);
            std::ostringstream out;
            retroarch::write_player_binds(out, map, player);
            const std::string cfg = out.str();
            const std::string p =
                "input_player" + std::to_string(player) + "_";

            CHECK(cfg.find(p + "b_btn = \"2\"\n") != std::string::npos);
            CHECK(cfg.find(p + "y_btn = \"1\"\n") != std::string::npos);
            CHECK(cfg.find(p + "select_btn = \"9\"\n") != std::string::npos);
            CHECK(cfg.find(p + "start_btn = \"12\"\n") != std::string::npos);
            CHECK(cfg.find(p + "a_btn = \"3\"\n") != std::string::npos);
            CHECK(cfg.find(p + "x_btn = \"0\"\n") != std::string::npos);
            CHECK(cfg.find(p + "l_btn = \"4\"\n") != std::string::npos);
            CHECK(cfg.find(p + "r_btn = \"5\"\n") != std::string::npos);
            CHECK(cfg.find(p + "l2_btn = \"6\"\n") != std::string::npos);
            CHECK(cfg.find(p + "r2_btn = \"8\"\n") != std::string::npos);
            CHECK(cfg.find(p + "l3_btn = \"nul\"\n") != std::string::npos);
            CHECK(cfg.find(p + "r3_btn = \"nul\"\n") != std::string::npos);
            CHECK(cfg.find(p + "up_axis = \"\"\n") != std::string::npos);
            CHECK(cfg.find(p + "down_axis = \"\"\n") != std::string::npos);
            CHECK(cfg.find(p + "r_x_") == std::string::npos);
            CHECK(cfg.find("_btn = \"\"\n") == std::string::npos);
        }
    }
}

TEST_CASE("N64-style controllers use physical Z+Start on every core",
          "[retroarch][mapping][hotkeys][n64_style]") {
    const std::vector<std::string> cores = {
        "nestopia_libretro",
        "fceumm_libretro",
        "snes9x2010_libretro",
        "genesis_plus_gx_libretro",
        "pcsx_rearmed_libretro",
        "beetle_psx_libretro",
        "swanstation_libretro",
        "mednafen_pce_fast_libretro",
        "prosystem_libretro",
        "fbneo_libretro",
        "mupen64plus_next_libretro",
        "parallel_n64_libretro",
        "flycast_libretro",
        "totally_unknown_core",
    };

    for (const auto& core : cores) {
        INFO("core=" << core);
        const auto map =
            get_mapping(ControllerType::N64_ADAPTER, core);
        CHECK(map.enable_hotkey_btn == "6");
        CHECK(map.menu_toggle_btn.empty());
        CHECK(map.exit_emulator_btn == "12");
    }
}

TEST_CASE("N64-style hotkeys serialize as a direct exit chord, no menu",
          "[retroarch][mapping][hotkeys][config]") {
    // Z (6) held + Start (12) quits the game outright. The menu-toggle
    // bind is deliberately absent: the owner's verdict after using the
    // PS-style Select+Start exit was that RetroArch's menu "isn't needed
    // at all" on the kiosk — auto-save-on-exit already covers what the
    // menu was for.
    const auto map = get_mapping(
        ControllerType::N64_ADAPTER, "pcsx_rearmed_libretro");
    std::ostringstream out;
    retroarch::write_hotkey_binds(out, map);
    CHECK(out.str() ==
          "input_enable_hotkey_btn = \"6\"\n"
          "input_exit_emulator_btn = \"12\"\n");
    CHECK(out.str().find("input_menu_toggle_btn") ==
          std::string::npos);
}

TEST_CASE("N64 adapter uses the role-consistent Dreamcast layout",
          "[retroarch][mapping][dreamcast][n64_style]") {
    const auto map =
        get_mapping(ControllerType::N64_ADAPTER, "flycast_libretro");

    CHECK(map.name == "Dreamcast (N64 pad)");
    CHECK(map.analog_dpad_mode == "0");
    CHECK(map.b_btn == "2");       // N64 A      -> DC A
    CHECK(map.y_btn == "1");       // N64 B      -> DC X
    CHECK(map.x_btn == "0");       // C-Left     -> DC Y
    CHECK(map.a_btn == "3");       // C-Down     -> DC B
    CHECK(map.l2_btn == "4");      // L          -> left trigger
    CHECK(map.r2_btn == "5");      // R          -> right trigger
    CHECK(map.start_btn == "12");

    CHECK(map.select_btn.empty());
    CHECK(map.l_btn.empty());
    CHECK(map.r_btn.empty());
    CHECK(map.l3_btn.empty());
    CHECK(map.r3_btn.empty());
    CHECK(map.r_x_plus.empty());
    CHECK(map.r_x_plus_btn.empty());
    CHECK(map.up_axis.empty());
    CHECK(map.down_axis.empty());
    CHECK(map.left_axis.empty());
    CHECK(map.right_axis.empty());

    CHECK(map.up_btn == "h0up");
    CHECK(map.down_btn == "h0down");
    CHECK(map.left_btn == "h0left");
    CHECK(map.right_btn == "h0right");
    CHECK(map.l_x_plus == "+0");
    CHECK(map.l_x_minus == "-0");
    CHECK(map.l_y_plus == "+1");
    CHECK(map.l_y_minus == "-1");

    CHECK(map.enable_hotkey_btn == "6");
    CHECK(map.menu_toggle_btn.empty());
        CHECK(map.exit_emulator_btn == "12");

    for (int player : {1, 2}) {
        INFO("player=" << player);
        std::ostringstream out;
        retroarch::write_player_binds(out, map, player);
        const std::string cfg = out.str();
        const std::string p =
            "input_player" + std::to_string(player) + "_";

        CHECK(cfg.find(p + "b_btn = \"2\"\n") != std::string::npos);
        CHECK(cfg.find(p + "y_btn = \"1\"\n") != std::string::npos);
        CHECK(cfg.find(p + "select_btn = \"nul\"\n") != std::string::npos);
        CHECK(cfg.find(p + "start_btn = \"12\"\n") != std::string::npos);
        CHECK(cfg.find(p + "a_btn = \"3\"\n") != std::string::npos);
        CHECK(cfg.find(p + "x_btn = \"0\"\n") != std::string::npos);
        CHECK(cfg.find(p + "l_btn = \"nul\"\n") != std::string::npos);
        CHECK(cfg.find(p + "r_btn = \"nul\"\n") != std::string::npos);
        CHECK(cfg.find(p + "l2_btn = \"4\"\n") != std::string::npos);
        CHECK(cfg.find(p + "r2_btn = \"5\"\n") != std::string::npos);
        CHECK(cfg.find(p + "l3_btn = \"nul\"\n") != std::string::npos);
        CHECK(cfg.find(p + "r3_btn = \"nul\"\n") != std::string::npos);
        CHECK(cfg.find(p + "up_axis = \"\"\n") != std::string::npos);
        CHECK(cfg.find(p + "down_axis = \"\"\n") != std::string::npos);
        CHECK(cfg.find(p + "r_x_") == std::string::npos);
        CHECK(cfg.find("_btn = \"\"\n") == std::string::npos);
    }
}

TEST_CASE("PS-style pads use the universal N64 layout",
          "[retroarch][mapping][n64][ps_style]") {
    for (const auto& core : {"mupen64plus_next_libretro",
                             "parallel_n64_libretro"}) {
        INFO("core=" << core);
        const auto map =
            get_mapping(ControllerType::PS_STYLE_DRAGONRISE, core);

        CHECK(map.b_btn == "2");       // Cross    -> N64 A
        CHECK(map.y_btn == "3");       // Square   -> N64 B
        CHECK(map.x_btn == "0");       // Triangle -> C-Up
        CHECK(map.r_btn == "1");       // Circle   -> C-Right
        CHECK(map.select_btn == "4");  // L1       -> N64 L
        CHECK(map.l2_btn == "6");      // L2       -> N64 Z
        CHECK(map.r2_btn == "5");      // R1       -> N64 R
        CHECK(map.a_btn == "7");       // R2       -> C-Down
        CHECK(map.l_btn.empty());      // C-Left stays on the right stick
        CHECK(map.start_btn == "9");
        // PS-style: hold Select + press Start = direct QUIT (see the
        // semantic preamble); the menu-toggle hotkey is gone.
        CHECK(map.enable_hotkey_btn == "8");
        CHECK(map.menu_toggle_btn.empty());
        CHECK(map.exit_emulator_btn == "9");
        CHECK(map.l3_btn.empty());
        CHECK(map.r3_btn.empty());

        CHECK(map.up_btn == "h0up");
        CHECK(map.down_btn == "h0down");
        CHECK(map.left_btn == "h0left");
        CHECK(map.right_btn == "h0right");
        CHECK(map.l_x_plus == "+0");
        CHECK(map.l_x_minus == "-0");
        CHECK(map.l_y_plus == "+1");
        CHECK(map.l_y_minus == "-1");
        CHECK(map.r_x_plus == "+2");
        CHECK(map.r_x_minus == "-2");
        CHECK(map.r_y_plus == "+3");
        CHECK(map.r_y_minus == "-3");
    }
}

TEST_CASE("unknown controllers still fall back to the N64 adapter mapping",
          "[retroarch][mapping]") {
    const auto known = get_mapping(ControllerType::N64_ADAPTER,
                                   "mupen64plus_next_libretro");
    const auto fallback = get_mapping(ControllerType::UNKNOWN,
                                      "mupen64plus_next_libretro");
    REQUIRE(fallback.b_btn == known.b_btn);
    REQUIRE(fallback.y_btn == known.y_btn);
    REQUIRE(fallback.select_btn == known.select_btn);
    REQUIRE(fallback.r2_btn == known.r2_btn);
}

TEST_CASE("right-stick binds reach the config in the form the pad needs",
          "[retroarch][mapping][config]") {
    // A mapping is only worth as much as what actually lands in the
    // RetroArch config. PS-style pads drive N64 C buttons from their real
    // right-stick axes; native N64 pads instead use direct RetroPad slots.
    SECTION("N64 adapter emits no right-stick binds") {
        std::ostringstream out;
        retroarch::write_right_stick_binds(
            out, get_mapping(ControllerType::N64_ADAPTER,
                             "mupen64plus_next_libretro"), 1);
        REQUIRE(out.str().empty());
    }

    SECTION("PS-style pad emits the axis form only") {
        std::ostringstream out;
        retroarch::write_right_stick_binds(
            out, get_mapping(ControllerType::PS_STYLE_DRAGONRISE,
                             "mupen64plus_next_libretro"), 1);
        const std::string cfg = out.str();
        REQUIRE(cfg.find("input_player1_r_x_plus_axis = \"+2\"") !=
                std::string::npos);
        REQUIRE(cfg.find("input_player1_r_y_plus_axis = \"+3\"") !=
                std::string::npos);
        REQUIRE(cfg.find("_btn") == std::string::npos);
    }

    SECTION("single-stick cores emit nothing at all") {
        // Emitting empty values would UNBIND the stick rather than leave
        // it alone.
        std::ostringstream out;
        retroarch::write_right_stick_binds(
            out, get_mapping(ControllerType::N64_ADAPTER,
                             "nestopia_libretro"), 1);
        REQUIRE(out.str().empty());
    }
}

TEST_CASE("native N64 fields are serialized for both players",
          "[retroarch][mapping][config]") {
    const auto map = get_mapping(ControllerType::N64_ADAPTER,
                                 "mupen64plus_next_libretro");
    for (const int player : {1, 2}) {
        INFO("player=" << player);
        std::ostringstream out;
        retroarch::write_player_binds(out, map, player);
        const std::string cfg = out.str();
        const std::string p = "input_player" + std::to_string(player) + "_";

        CHECK(cfg.find(p + "b_btn = \"2\"\n") != std::string::npos);
        CHECK(cfg.find(p + "y_btn = \"1\"\n") != std::string::npos);
        CHECK(cfg.find(p + "a_btn = \"3\"\n") != std::string::npos);
        CHECK(cfg.find(p + "x_btn = \"9\"\n") != std::string::npos);
        CHECK(cfg.find(p + "l_btn = \"0\"\n") != std::string::npos);
        CHECK(cfg.find(p + "r_btn = \"8\"\n") != std::string::npos);
        CHECK(cfg.find(p + "select_btn = \"4\"\n") != std::string::npos);
        CHECK(cfg.find(p + "r2_btn = \"5\"\n") != std::string::npos);
        CHECK(cfg.find(p + "r_x_") == std::string::npos);
        CHECK(cfg.find(p + "r_y_") == std::string::npos);
    }
}

TEST_CASE("every shipped core has a real mapping on both pads",
          "[retroarch][mapping]") {
    // Guards the silent failure mode: an unmatched core falls through to a
    // default-constructed ControllerMapping named "Default", which is
    // wrong for every system and produces "the buttons are scrambled"
    // with nothing logged.
    const std::vector<std::string> cores = {
        "nestopia_libretro",        "snes9x2010_libretro",
        "genesis_plus_gx_libretro", "pcsx_rearmed_libretro",
        "mednafen_pce_fast_libretro", "prosystem_libretro",
        "fbneo_libretro",           "mupen64plus_next_libretro",
        "parallel_n64_libretro",    "flycast_libretro",
    };
    for (const auto& core : cores) {
        INFO("core: " << core);
        REQUIRE(get_mapping(ControllerType::N64_ADAPTER, core).name !=
                "Default");
        REQUIRE(get_mapping(ControllerType::PS_STYLE_DRAGONRISE, core).name !=
                "Default");
    }
}
