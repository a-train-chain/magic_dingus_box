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
