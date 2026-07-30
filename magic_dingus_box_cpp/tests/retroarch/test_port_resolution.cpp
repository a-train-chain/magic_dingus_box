#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

#include "retroarch/controller_detector.h"
#include "retroarch/controller_mapping.h"
#include "retroarch/controller_profile.h"

// Task 7: detect_connected_controllers() itself reads /dev/input and
// /sys/class/input, so it has no meaningful behavior to test on macOS (see
// controller_detector.cpp). resolve_port_mappings() is the pure part of the
// launcher's per-port wiring -- it consumes a DetectedPad list rather than
// scanning for one -- so it IS fully testable here with a synthetic list.
// These tests exist to pin the three behaviors the task brief calls out as
// "must not regress": no-pads falls back to today's single-mapping path,
// one-pad mirrors P1 exactly, and two-or-more pads resolve independently.

using namespace retroarch;

namespace {
// Real VID/PID pair observed on fielded hardware (see .superpowers/sdd
// task brief): SWITCH CO.,LTD. Controller (Dinput), NOT in match_vid_pid's
// whitelist. Exercises the "common case in the field is actually the
// unrecognized-pad path" scenario.
constexpr uint16_t kUnknownVid = 0x2563;
constexpr uint16_t kUnknownPid = 0x0575;
}  // namespace

TEST_CASE("no pads detected falls back to today's single-mapping path, unchanged",
          "[retroarch][port_resolution]") {
    const std::vector<DetectedPad> pads;  // empty: nothing detected
    const std::map<std::string, PhysicalProfile> store;  // empty profile store

    const auto today = get_mapping(ControllerType::N64_ADAPTER, "mupen64plus_next_libretro");
    const auto resolved = resolve_port_mappings(pads, ControllerType::N64_ADAPTER,
                                                store, "mupen64plus_next_libretro");

    // Both players must land on EXACTLY the mapping detect_primary_controller()
    // + get_mapping() would have produced before this task -- a regression
    // here would silently give every currently-fielded box a worse mapping
    // than it has today.
    REQUIRE(resolved.p1.name == today.name);
    REQUIRE(resolved.p1.b_btn == today.b_btn);
    REQUIRE(resolved.p1.r_x_plus_btn == today.r_x_plus_btn);
    REQUIRE(resolved.p2.name == today.name);
    REQUIRE(resolved.p2.b_btn == today.b_btn);

    // The fallback type is whatever detect_primary_controller() found, not
    // hardcoded UNKNOWN -- exercise the other branch too.
    const auto today_ps =
        get_mapping(ControllerType::PS_STYLE_DRAGONRISE, "mupen64plus_next_libretro");
    const auto resolved_ps = resolve_port_mappings(
        pads, ControllerType::PS_STYLE_DRAGONRISE, store, "mupen64plus_next_libretro");
    REQUIRE(resolved_ps.p1.name == today_ps.name);
    REQUIRE(resolved_ps.p1.r_x_plus == today_ps.r_x_plus);
}

TEST_CASE("one pad connected mirrors player 1 onto player 2 exactly",
          "[retroarch][port_resolution]") {
    const std::map<std::string, PhysicalProfile> store;

    SECTION("a known/whitelisted pad") {
        const std::vector<DetectedPad> pads = {
            {0, 0x0e6d, 0x111d, "SWITCH CO.,LTD. Controller (Dinput)"},
        };
        const auto resolved = resolve_port_mappings(pads, ControllerType::UNKNOWN, store,
                                                     "mupen64plus_next_libretro");
        // True mirror: every field equal, not just the ones a spot-check
        // might catch.
        REQUIRE(resolved.p2.name == resolved.p1.name);
        REQUIRE(resolved.p2.b_btn == resolved.p1.b_btn);
        REQUIRE(resolved.p2.a_btn == resolved.p1.a_btn);
        REQUIRE(resolved.p2.r_x_plus_btn == resolved.p1.r_x_plus_btn);
        REQUIRE(resolved.p2.r_y_minus_btn == resolved.p1.r_y_minus_btn);
        REQUIRE(resolved.p2.enable_hotkey_btn == resolved.p1.enable_hotkey_btn);
        REQUIRE(resolved.p2.core_option_pad_type == resolved.p1.core_option_pad_type);
    }

    SECTION("the real-hardware unrecognized pad (2563:0575) -- the common case") {
        // Confirmed on the fielded Pi: this VID/PID is not in
        // match_vid_pid's whitelist, so it rides the legacy N64 fallback
        // via resolve_mapping_for_pad. One such pad connected must still
        // mirror onto player 2, same as any other single-pad box.
        const std::vector<DetectedPad> pads = {
            {0, kUnknownVid, kUnknownPid, "SWITCH CO.,LTD. Controller (Dinput)"},
        };
        const auto resolved = resolve_port_mappings(pads, ControllerType::UNKNOWN, store,
                                                     "pcsx_rearmed_libretro");
        REQUIRE(resolved.p1.name == resolved.p2.name);
        REQUIRE(resolved.p1.b_btn == resolved.p2.b_btn);
        REQUIRE(resolved.p1.core_option_pad_type == resolved.p2.core_option_pad_type);
        // And it should equal what resolve_mapping_for_pad alone produces
        // for this VID/PID -- i.e. the legacy fallback, not some special
        // "unknown pad" mapping.
        const auto expected =
            resolve_mapping_for_pad(kUnknownVid, kUnknownPid, store, "pcsx_rearmed_libretro");
        REQUIRE(resolved.p1.name == expected.name);
    }
}

TEST_CASE("two different pads resolve independently per port",
          "[retroarch][port_resolution]") {
    const std::map<std::string, PhysicalProfile> store;
    // Port 0: the N64 adapter. Port 1: the PS-style DragonRise pad. A real
    // mismatched pair -- exactly the scenario this task exists to fix.
    const std::vector<DetectedPad> pads = {
        {0, 0x0e6d, 0x111d, "SWITCH CO.,LTD. Controller (Dinput)"},
        {1, 0x0079, 0x0006, "DragonRise Inc. Generic USB Joystick"},
    };
    const auto resolved =
        resolve_port_mappings(pads, ControllerType::UNKNOWN, store, "mupen64plus_next_libretro");

    const auto expected_p1 = get_mapping(ControllerType::N64_ADAPTER, "mupen64plus_next_libretro");
    const auto expected_p2 =
        get_mapping(ControllerType::PS_STYLE_DRAGONRISE, "mupen64plus_next_libretro");

    REQUIRE(resolved.p1.name == expected_p1.name);
    REQUIRE(resolved.p1.b_btn == expected_p1.b_btn);
    REQUIRE(resolved.p1.r_x_plus_btn == expected_p1.r_x_plus_btn);  // digital C cluster

    REQUIRE(resolved.p2.name == expected_p2.name);
    REQUIRE(resolved.p2.b_btn == expected_p2.b_btn);
    REQUIRE(resolved.p2.r_x_plus == expected_p2.r_x_plus);  // real right stick

    // The two mappings must genuinely differ -- this is the whole point of
    // the task: a mismatched pair now each gets its own correct mapping
    // instead of both being forced onto player 1's.
    REQUIRE(resolved.p1.r_x_plus_btn != resolved.p2.r_x_plus_btn);
    REQUIRE_FALSE(resolved.p1.r_x_plus_btn.empty());
    REQUIRE(resolved.p1.r_x_plus.empty());
    REQUIRE_FALSE(resolved.p2.r_x_plus.empty());
}

TEST_CASE("a captured profile resolves independently per port too",
          "[retroarch][port_resolution]") {
    // A rewired/clone pad captured under the DragonRise's VID/PID should
    // win on whichever port it's plugged into, without affecting the other
    // port's resolution.
    std::map<std::string, PhysicalProfile> store;
    PhysicalProfile clone = builtin_dragonrise_profile();
    clone.controls[LogicalControl::CROSS].token = "77";  // rewired clone pad
    store[vidpid_key(0x0079, 0x0006)] = clone;

    const std::vector<DetectedPad> pads = {
        {0, 0x0079, 0x0006, "Clone pad"},          // port 0: the captured clone
        {1, 0x1234, 0x5678, "Totally unknown pad"},  // port 1: nothing on file
    };
    const auto resolved =
        resolve_port_mappings(pads, ControllerType::UNKNOWN, store, "snes9x2010_libretro");

    REQUIRE(resolved.p1.b_btn == "77");  // captured profile's token, not the builtin's
    // Port 1 has no captured profile and no builtin match -> legacy N64
    // fallback, same as resolve_mapping_for_pad(0x1234, 0x5678, ...) alone.
    const auto expected_p2 =
        resolve_mapping_for_pad(0x1234, 0x5678, store, "snes9x2010_libretro");
    REQUIRE(resolved.p2.name == expected_p2.name);
    REQUIRE(resolved.p2.b_btn != "77");
}

TEST_CASE("a saved 2563:0575 wizard profile drives kiosk, PS1, Dreamcast, and N64",
          "[retroarch][wizard_profile][cross_layer]") {
    using K = PhysicalBinding::Kind;
    const auto btn = [](uint16_t code, const char* token) {
        return PhysicalBinding{K::BUTTON, code, 0, token};
    };
    const auto hat = [](uint16_t code, int direction, const char* token) {
        return PhysicalBinding{K::HAT, code, direction, token};
    };
    const auto axis = [](uint16_t code, int direction, const char* token) {
        return PhysicalBinding{K::AXIS, code, direction, token};
    };

    PhysicalProfile captured;
    captured.name = "SWITCH CO.,LTD. Controller (Dinput)";
    captured.style = ControllerStyle::N64_STYLE;
    captured.vid = kUnknownVid;
    captured.pid = kUnknownPid;
    captured.controls = {
        {LogicalControl::N64_A, btn(305, "1")},
        {LogicalControl::N64_B, btn(306, "2")},
        {LogicalControl::N64_C_DOWN, btn(304, "0")},
        {LogicalControl::N64_C_LEFT, btn(307, "3")},
        {LogicalControl::N64_L, btn(308, "4")},
        {LogicalControl::N64_R, btn(309, "5")},
        {LogicalControl::N64_Z, btn(310, "6")},
        {LogicalControl::N64_C_RIGHT, btn(312, "8")},
        {LogicalControl::N64_C_UP, btn(313, "9")},
        {LogicalControl::N64_START, btn(316, "12")},
        {LogicalControl::N64_DPAD_UP, hat(0x11, -1, "h0up")},
        {LogicalControl::N64_DPAD_DOWN, hat(0x11, +1, "h0down")},
        {LogicalControl::N64_DPAD_LEFT, hat(0x10, -1, "h0left")},
        {LogicalControl::N64_DPAD_RIGHT, hat(0x10, +1, "h0right")},
        {LogicalControl::N64_STICK_UP, axis(1, -1, "-1")},
        {LogicalControl::N64_STICK_DOWN, axis(1, +1, "+1")},
        {LogicalControl::N64_STICK_LEFT, axis(0, -1, "-0")},
        {LogicalControl::N64_STICK_RIGHT, axis(0, +1, "+0")},
    };

    const std::map<std::string, PhysicalProfile> saved = {
        {vidpid_key(kUnknownVid, kUnknownPid), captured},
    };
    const auto store = profiles_from_json(profiles_to_json(saved));
    REQUIRE(store.size() == 1);

    const auto& profile = store.at(vidpid_key(kUnknownVid, kUnknownPid));
    const auto overlay = menu_overlay_from_profile(profile);
    REQUIRE(overlay.buttons.at(305) == platform::InputAction::SELECT);
    REQUIRE(overlay.buttons.at(306) == platform::InputAction::SETTINGS_MENU);
    REQUIRE(overlay.buttons.at(316) == platform::InputAction::SELECT);
    REQUIRE(overlay.buttons.at(310) == platform::InputAction::PLAY_PAUSE);
    REQUIRE(overlay.buttons.at(309) == platform::InputAction::NEXT);
    REQUIRE(overlay.buttons.at(308) == platform::InputAction::PREV);
    REQUIRE(overlay.nav_x_abs == 0);  // ABS_X

    const auto ps1 =
        resolve_mapping_for_pad(kUnknownVid, kUnknownPid, store, "pcsx_rearmed_libretro");
    REQUIRE(ps1.b_btn == "1");
    REQUIRE(ps1.y_btn == "2");
    REQUIRE(ps1.a_btn == "0");
    REQUIRE(ps1.x_btn == "3");
    REQUIRE(ps1.l_btn == "4");
    REQUIRE(ps1.r_btn == "5");
    REQUIRE(ps1.l2_btn == "6");
    REQUIRE(ps1.r2_btn == "8");
    REQUIRE(ps1.select_btn == "9");
    REQUIRE(ps1.start_btn == "12");

    const auto dreamcast =
        resolve_mapping_for_pad(kUnknownVid, kUnknownPid, store, "flycast_libretro");
    REQUIRE(dreamcast.b_btn == "1");
    REQUIRE(dreamcast.y_btn == "2");
    REQUIRE(dreamcast.a_btn == "0");
    REQUIRE(dreamcast.x_btn == "3");
    REQUIRE(dreamcast.l2_btn == "4");
    REQUIRE(dreamcast.r2_btn == "5");
    REQUIRE(dreamcast.start_btn == "12");
    REQUIRE(dreamcast.enable_hotkey_btn == "6");
    REQUIRE(dreamcast.menu_toggle_btn == "12");

    const auto n64 = resolve_mapping_for_pad(
        kUnknownVid, kUnknownPid, store, "mupen64plus_next_libretro");
    REQUIRE(n64.b_btn == "1");
    REQUIRE(n64.a_btn == "2");
    REQUIRE(n64.l_btn == "4");
    REQUIRE(n64.r_btn == "5");
    REQUIRE(n64.l2_btn == "6");
    REQUIRE(n64.start_btn == "12");
    REQUIRE(n64.r_y_minus_btn == "9");
    REQUIRE(n64.r_y_plus_btn == "0");
    REQUIRE(n64.r_x_minus_btn == "3");
    REQUIRE(n64.r_x_plus_btn == "8");
    REQUIRE(n64.enable_hotkey_btn == "6");
    REQUIRE(n64.menu_toggle_btn == "12");

    for (const auto* mapping : {&ps1, &dreamcast, &n64}) {
        REQUIRE(mapping->up_btn == "h0up");
        REQUIRE(mapping->down_btn == "h0down");
        REQUIRE(mapping->left_btn == "h0left");
        REQUIRE(mapping->right_btn == "h0right");
        REQUIRE(mapping->l_x_plus == "+0");
        REQUIRE(mapping->l_x_minus == "-0");
        REQUIRE(mapping->l_y_plus == "+1");
        REQUIRE(mapping->l_y_minus == "-1");
    }
}

TEST_CASE("extra pads beyond player 2 are ignored", "[retroarch][port_resolution]") {
    const std::map<std::string, PhysicalProfile> store;
    const std::vector<DetectedPad> pads = {
        {0, 0x0e6d, 0x111d, "N64 adapter"},
        {1, 0x0079, 0x0006, "DragonRise"},
        {2, 0x1234, 0x5678, "A third pad nobody asked for"},
    };
    const auto resolved =
        resolve_port_mappings(pads, ControllerType::UNKNOWN, store, "mupen64plus_next_libretro");
    const auto expected_p1 = get_mapping(ControllerType::N64_ADAPTER, "mupen64plus_next_libretro");
    const auto expected_p2 =
        get_mapping(ControllerType::PS_STYLE_DRAGONRISE, "mupen64plus_next_libretro");
    REQUIRE(resolved.p1.name == expected_p1.name);
    REQUIRE(resolved.p2.name == expected_p2.name);
}
