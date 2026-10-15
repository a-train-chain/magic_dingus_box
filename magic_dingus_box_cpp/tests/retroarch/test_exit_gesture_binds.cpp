// Every resolvable mapping must write an EXIT bind.
//
// The kiosk ships no RetroArch menu (owner decision 2026-08-03), so the
// exit gesture -- hold N64 Z / PS Select, press Start -- is the ONLY way a
// player leaves a game with the pad. Three ways it used to vanish:
//
//   1. The Controller Setup wizard did not require Z / Select, so a
//      captured profile could omit the hotkey-enable modifier. A captured
//      profile shadows the builtin for its VID/PID, so enable_hotkey_btn
//      came out "" and write_hotkey_binds() wrote NOTHING.
//   2. A modifier captured as an AXIS (an N64 clone whose Z reports ABS_Z)
//      was dropped by the kind-aware put_btn contract -> "" -> same.
//   3. (Same for the exit button itself, Start, captured as an axis.)
//
// build_mapping() now falls back to the builtin profile's token for a
// missing hotkey control and emits the *_axis form for an axis capture.

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <sstream>
#include <string>

#include "retroarch/controller_mapping.h"
#include "retroarch/controller_profile.h"
#include "retroarch/logical_controls.h"

using retroarch::build_mapping;
using retroarch::ControllerMapping;
using retroarch::ControllerStyle;
using retroarch::get_semantic_mapping;
using retroarch::PhysicalBinding;
using retroarch::PhysicalProfile;
using K = retroarch::PhysicalBinding::Kind;
using L = retroarch::LogicalControl;

namespace {

// Every core the kiosk ships, plus an unknown one (preamble-only mapping).
const char* const kCores[] = {
    "nestopia_libretro",        "snes9x2010_libretro",
    "genesis_plus_gx_libretro", "pcsx_rearmed_libretro",
    "mednafen_pce_fast_libretro", "prosystem_libretro",
    "fbneo_libretro",           "mupen64plus_next_libretro",
    "parallel_n64_libretro",    "flycast_libretro",
    "some_future_core_libretro",
};

std::string hotkeys(const ControllerMapping& m) {
    std::ostringstream o;
    retroarch::write_hotkey_binds(o, m);
    return o.str();
}

bool has(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

// The exit gesture needs BOTH the modifier and the exit button, each in
// either form.
void require_exit_bind(const std::string& out) {
    INFO(out);
    REQUIRE((has(out, "input_enable_hotkey_btn = \"") ||
             has(out, "input_enable_hotkey_axis = \"")));
    REQUIRE((has(out, "input_exit_emulator_btn = \"") ||
             has(out, "input_exit_emulator_axis = \"")));
    // Never an empty value (RetroArch reads "" as button 0).
    REQUIRE_FALSE(has(out, "= \"\""));
    // And never the menu: the menu-toggle hotkey is gone.
    REQUIRE_FALSE(has(out, "input_menu_toggle_btn"));
}

PhysicalProfile captured_from(const PhysicalProfile& builtin) {
    PhysicalProfile p = builtin;
    p.name = "Captured";
    p.vid = 0x1234;
    p.pid = 0x5678;
    return p;
}

}  // namespace

TEST_CASE("every builtin mapping writes an exit bind",
          "[retroarch][hotkeys][exit]") {
    for (const char* core : kCores) {
        INFO("core: " << core);
        require_exit_bind(hotkeys(build_mapping(
            get_semantic_mapping(ControllerStyle::N64_STYLE, core),
            retroarch::builtin_n64_adapter_profile())));
        require_exit_bind(hotkeys(build_mapping(
            get_semantic_mapping(ControllerStyle::PS_STYLE, core),
            retroarch::builtin_dragonrise_profile())));
        // The public dispatch paths, including the legacy fallback.
        for (auto t : {retroarch::ControllerType::N64_ADAPTER,
                       retroarch::ControllerType::PS_STYLE_DRAGONRISE,
                       retroarch::ControllerType::UNKNOWN}) {
            require_exit_bind(hotkeys(retroarch::get_mapping(t, core)));
        }
    }
}

TEST_CASE("a captured profile missing Z / Select falls back to the builtin modifier",
          "[retroarch][hotkeys][exit]") {
    PhysicalProfile n64 = captured_from(retroarch::builtin_n64_adapter_profile());
    n64.controls.erase(L::N64_Z);
    PhysicalProfile ps = captured_from(retroarch::builtin_dragonrise_profile());
    ps.controls.erase(L::SELECT);

    const std::string n64_z = retroarch::builtin_n64_adapter_profile().token(L::N64_Z);
    const std::string ps_sel = retroarch::builtin_dragonrise_profile().token(L::SELECT);
    REQUIRE_FALSE(n64_z.empty());
    REQUIRE_FALSE(ps_sel.empty());

    for (const char* core : kCores) {
        INFO("core: " << core);
        const auto m64 = build_mapping(
            get_semantic_mapping(ControllerStyle::N64_STYLE, core), n64);
        CHECK(m64.enable_hotkey_btn == n64_z);
        require_exit_bind(hotkeys(m64));

        const auto mps = build_mapping(
            get_semantic_mapping(ControllerStyle::PS_STYLE, core), ps);
        CHECK(mps.enable_hotkey_btn == ps_sel);
        require_exit_bind(hotkeys(mps));
    }

    // Through the real per-pad resolution path too (captured wins over
    // builtin/legacy for this VID/PID).
    std::map<std::string, PhysicalProfile> store{
        {retroarch::vidpid_key(n64.vid, n64.pid), n64}};
    require_exit_bind(hotkeys(retroarch::resolve_mapping_for_pad(
        n64.vid, n64.pid, store, "mupen64plus_next_libretro")));
}

TEST_CASE("a hotkey control captured as an AXIS emits the _axis form",
          "[retroarch][hotkeys][exit]") {
    SECTION("N64 Z on ABS_Z") {
        PhysicalProfile n64 = captured_from(retroarch::builtin_n64_adapter_profile());
        n64.controls[L::N64_Z] = PhysicalBinding{K::AXIS, 0x02, +1, "+2"};
        for (const char* core : kCores) {
            INFO("core: " << core);
            const auto m = build_mapping(
                get_semantic_mapping(ControllerStyle::N64_STYLE, core), n64);
            CHECK(m.enable_hotkey_btn.empty());   // never an axis token in _btn
            CHECK(m.enable_hotkey_axis == "+2");
            const std::string out = hotkeys(m);
            require_exit_bind(out);
            CHECK(has(out, "input_enable_hotkey_axis = \"+2\"\n"));
            CHECK_FALSE(has(out, "input_enable_hotkey_btn"));
        }
    }
    SECTION("PS Select and Start both on axes") {
        PhysicalProfile ps = captured_from(retroarch::builtin_dragonrise_profile());
        ps.controls[L::SELECT] = PhysicalBinding{K::AXIS, 0x05, -1, "-3"};
        ps.controls[L::START] = PhysicalBinding{K::AXIS, 0x05, +1, "+3"};
        const auto m = build_mapping(
            get_semantic_mapping(ControllerStyle::PS_STYLE, "snes9x2010_libretro"), ps);
        const std::string out = hotkeys(m);
        require_exit_bind(out);
        CHECK(out ==
              "input_enable_hotkey_axis = \"-3\"\n"
              "input_exit_emulator_axis = \"+3\"\n");
    }
}

TEST_CASE("no modifier at all writes no hotkey block",
          "[retroarch][hotkeys][exit]") {
    // An exit bind without its modifier would quit on a bare Start press.
    ControllerMapping m;
    m.exit_emulator_btn = "9";
    CHECK(hotkeys(m).empty());
}
