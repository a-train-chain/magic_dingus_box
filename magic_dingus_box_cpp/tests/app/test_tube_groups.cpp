#include <catch2/catch_test_macros.hpp>
#include <array>
#include <string_view>
#include <utility>
#include "tube/tube_groups.h"
using namespace tube;

TEST_CASE("tube groups: every row is in exactly one group", "[tube]") {
    std::array<int, kEffectCount> seen{};
    int masks = 0;
    for (const auto& g : kGroups) { masks += g.has_mask_row; for (auto e : g.rows) ++seen[index_of(e)]; }
    for (int n : seen) REQUIRE(n == 1);
    REQUIRE(masks == 1);
}
TEST_CASE("tube groups: Sound Factory's ten, in its order", "[tube]") {
    const std::array<std::pair<std::string_view, std::size_t>, 10> want{{
        {"PICTURE", 5}, {"SCANLINES", 4}, {"MASK", 2}, {"LIGHT", 3}, {"GLASS", 5},
        {"SIGNAL", 4}, {"SUPPLY", 4}, {"CHARACTER", 6}, {"SYNC", 7}, {"COLOUR PURITY", 4}}};
    for (std::size_t i = 0; i < 10; ++i) {
        REQUIRE(kGroups[i].label == want[i].first);
        REQUIRE(kGroups[i].rows.size() == want[i].second);
    }
    REQUIRE(kGroups[2].has_mask_row);
}
TEST_CASE("tube groups: labels and steps", "[tube]") {
    REQUIRE(row_info(Effect::hum).label == "HUM BAR");
    REQUIRE(row_info(Effect::white).label == "COLOUR TEMP");
    REQUIRE(row_info(Effect::flagging).label == "TOP EDGE BEND");
    REQUIRE(row_info(Effect::maskSize).hint1 == "how wide one colour triad is");
    REQUIRE(row_info(Effect::rim).hint2 == "where the tube meets its surround");
    REQUIRE(kMaskRowInfo.label == "MASK");
    for (auto e : kEffects) REQUIRE_FALSE(row_info(e).label.empty());
    REQUIRE(row_step(Effect::maskSize) == 10);
    REQUIRE(row_step(Effect::hum) == 5);
    REQUIRE(step_row(Effect::hum, 20, +1) == 25);
    REQUIRE(step_row(Effect::hum, 22, +1) == 25);   // off-grid value (set from the web) snaps
    REQUIRE(step_row(Effect::hum, 22, -1) == 20);
    REQUIRE(step_row(Effect::hum, 100, +1) == 100);
    REQUIRE(step_row(Effect::hum, 0, -1) == 0);
    REQUIRE(step_row(Effect::lineSize, 10, +1) == 20);
}
