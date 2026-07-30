#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <vector>

#include "retroarch/controller_detector.h"
#include "retroarch/controller_mapping.h"

// Physical button IDs on the USB N64-style adapter (verified via evtest,
// see controller_mapping.h):
//   0=C-Left, 1=B, 2=A, 3=C-Down, 4=L shoulder, 5=R shoulder,
//   6=Z trigger, 8=C-Right, 9=C-Up, 12=Start
//   Axes: 0/1 = analog stick, hat0 = D-pad

using retroarch::ControllerType;
using retroarch::get_mapping;

TEST_CASE("N64 games on the N64 adapter map 1:1 to the real pad",
          "[retroarch][mapping][n64]") {
    const auto map = get_mapping(ControllerType::N64_ADAPTER,
                                 "mupen64plus_next_libretro");

    // The whole point of this adapter: the physical button labelled A
    // drives N64 A. mupen64plus takes N64 A on RetroPad B.
    REQUIRE(map.b_btn == "2");   // physical A -> RetroPad B -> N64 A
    REQUIRE(map.a_btn == "1");   // physical B -> RetroPad A -> N64 B
    REQUIRE(map.l_btn == "4");   // L shoulder
    REQUIRE(map.r_btn == "5");   // R shoulder
    REQUIRE(map.l2_btn == "6");  // Z trigger -> RetroPad L2 -> N64 Z
    REQUIRE(map.start_btn == "12");

    // Real analog stick, not d-pad emulation.
    REQUIRE(map.analog_dpad_mode == "0");
    REQUIRE(map.l_x_plus == "+0");
    REQUIRE(map.l_y_plus == "+1");
}

TEST_CASE("N64 adapter C-buttons reach the core via the right stick",
          "[retroarch][mapping][n64]") {
    const auto map = get_mapping(ControllerType::N64_ADAPTER,
                                 "mupen64plus_next_libretro");

    // The C cluster is four DIGITAL buttons on real hardware, but
    // mupen64plus_next expects the C buttons on the RetroPad RIGHT STICK.
    // RetroArch can drive an analog bind from a digital button, which is
    // what the _btn (rather than _axis) form is for. Without these four,
    // the C buttons do nothing at all -- which breaks the camera in every
    // 3D game on the list.
    REQUIRE(map.r_x_plus_btn == "8");   // C-Right
    REQUIRE(map.r_x_minus_btn == "0");  // C-Left
    REQUIRE(map.r_y_plus_btn == "3");   // C-Down
    REQUIRE(map.r_y_minus_btn == "9");  // C-Up

    // The adapter has no second physical stick, so the axis form must NOT
    // also be emitted -- it would bind the C buttons to axes that do not
    // exist on this device.
    REQUIRE(map.r_x_plus.empty());
    REQUIRE(map.r_y_plus.empty());
}

TEST_CASE("N64 adapter keeps the established menu hotkey on N64 games",
          "[retroarch][mapping][n64]") {
    const auto map = get_mapping(ControllerType::N64_ADAPTER,
                                 "mupen64plus_next_libretro");
    // Z + Start is the kiosk-wide convention across every core.
    REQUIRE(map.enable_hotkey_btn == "6");
    REQUIRE(map.menu_toggle_btn == "12");
}

TEST_CASE("Dreamcast is playable on the N64 adapter",
          "[retroarch][mapping][dreamcast]") {
    const auto map = get_mapping(ControllerType::N64_ADAPTER,
                                 "flycast_libretro");

    REQUIRE(map.b_btn == "2");   // physical A -> DC A
    REQUIRE(map.a_btn == "1");   // physical B -> DC B
    REQUIRE(map.y_btn == "0");   // C-Left    -> DC X
    REQUIRE(map.x_btn == "3");   // C-Down    -> DC Y
    // The DC's analog triggers land on the only shoulders this pad has.
    REQUIRE(map.l2_btn == "4");
    REQUIRE(map.r2_btn == "5");
    REQUIRE(map.start_btn == "12");
    REQUIRE(map.enable_hotkey_btn == "6");
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
        CHECK(map.enable_hotkey_btn == "8");
        CHECK(map.menu_toggle_btn == "9");
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
    REQUIRE(fallback.r_x_plus_btn == known.r_x_plus_btn);
}

TEST_CASE("right-stick binds reach the config in the form the pad needs",
          "[retroarch][mapping][config]") {
    // A mapping is only worth as much as what actually lands in the
    // RetroArch config. The N64 adapter drives the C cluster from digital
    // buttons (_btn) and the PS pad from real axes (_axis); emitting the
    // wrong form, or dropping one, is a silent no-op at runtime.
    SECTION("N64 adapter emits the button form only") {
        std::ostringstream out;
        retroarch::write_right_stick_binds(
            out, get_mapping(ControllerType::N64_ADAPTER,
                             "mupen64plus_next_libretro"), 1);
        const std::string cfg = out.str();
        REQUIRE(cfg.find("input_player1_r_x_plus_btn = \"8\"") !=
                std::string::npos);
        REQUIRE(cfg.find("input_player1_r_x_minus_btn = \"0\"") !=
                std::string::npos);
        REQUIRE(cfg.find("input_player1_r_y_plus_btn = \"3\"") !=
                std::string::npos);
        REQUIRE(cfg.find("input_player1_r_y_minus_btn = \"9\"") !=
                std::string::npos);
        REQUIRE(cfg.find("_axis") == std::string::npos);
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

TEST_CASE("player 2 gets the C-buttons too",
          "[retroarch][mapping][config]") {
    // Six of the eighteen N64 games on the box are two-player (Mario Kart
    // 64, Smash Bros., Mario Party 3, GoldenEye, Perfect Dark, Diddy Kong
    // Racing). The launcher mirrors P1's mapping onto P2 so the second pad
    // works at all -- but the right stick was never part of that mirror, so
    // P2 would have had no camera control in any of them.
    std::ostringstream out;
    retroarch::write_right_stick_binds(
        out, get_mapping(ControllerType::N64_ADAPTER,
                         "mupen64plus_next_libretro"), 2);
    const std::string cfg = out.str();
    REQUIRE(cfg.find("input_player2_r_x_plus_btn = \"8\"") !=
            std::string::npos);
    REQUIRE(cfg.find("input_player2_r_y_minus_btn = \"9\"") !=
            std::string::npos);
    REQUIRE(cfg.find("player1") == std::string::npos);
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
