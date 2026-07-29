#include <catch2/catch_test_macros.hpp>
#include <iostream>
#include <vector>
#include "mapping_snapshot_util.h"
#include "mapping_snapshot_golden.h"
#include "retroarch/controller_detector.h"

using retroarch::ControllerType;
using retroarch::get_mapping;

static const std::vector<std::string> kSnapshotCores = {
    "nestopia_libretro",          "snes9x2010_libretro",
    "genesis_plus_gx_libretro",   "pcsx_rearmed_libretro",
    "mednafen_pce_fast_libretro", "prosystem_libretro",
    "fbneo_libretro",             "mupen64plus_next_libretro",
    "parallel_n64_libretro",      "flycast_libretro",
    "totally_unknown_core",  // guards the default-construct fallthrough
};

static ControllerType pad_of(const std::string& tag) {
    if (tag == "N64") return ControllerType::N64_ADAPTER;
    if (tag == "PS") return ControllerType::PS_STYLE_DRAGONRISE;
    return ControllerType::UNKNOWN;
}

TEST_CASE("mapping output is bit-identical to the pre-refactor snapshot",
          "[mapping_snapshot]") {
    REQUIRE(mapping_golden().size() == kSnapshotCores.size() * 3);
    for (const auto& [key, golden] : mapping_golden()) {
        const auto bar = key.find('|');
        REQUIRE(bar != std::string::npos);
        INFO("snapshot key: " << key);
        REQUIRE(serialize_mapping(get_mapping(pad_of(key.substr(0, bar)),
                                              key.substr(bar + 1))) == golden);
    }
}

// Maintenance tool, not a test: prints freshly-generated golden entries to
// stdout for regenerating mapping_snapshot_golden.h after a deliberate,
// hardware-verified mapping change. It asserts nothing (SUCCEED() only) and
// is hidden from default test runs by Catch2's "[.]" tag -- run it
// explicitly with `./test_retroarch_unit "[mapping_snapshot_gen]"` and paste
// its stdout into mapping_snapshot_golden.h.
TEST_CASE("GENERATOR - print golden entries", "[.][mapping_snapshot_gen]") {
    auto dump = [](const char* pad, ControllerType t) {
        for (const auto& core : kSnapshotCores) {
            std::cout << "{\"" << pad << "|" << core << "\", R\"GOLD("
                      << serialize_mapping(get_mapping(t, core))
                      << ")GOLD\"},\n";
        }
    };
    dump("N64", ControllerType::N64_ADAPTER);
    dump("PS", ControllerType::PS_STYLE_DRAGONRISE);
    dump("UNKNOWN", ControllerType::UNKNOWN);
    SUCCEED();
}
