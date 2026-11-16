#include <catch2/catch_test_macros.hpp>
#include <array>
#include <string_view>
#include <utility>
#include "tube/tube_groups.h"
#include "tube/tube_inputs.h"
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
TEST_CASE("tube groups: a step reads only the direction's sign, and 0 moves nothing", "[tube]") {
    REQUIRE(step_row(Effect::hum, 20, 0) == 20);    // a knob's raw zero delta
    REQUIRE(step_row(Effect::hum, 120, 0) == 100);  // ...still lands inside the range
    REQUIRE(step_row(Effect::hum, -5, 0) == 0);
    REQUIRE(step_row(Effect::hum, 20, +7) == 25);   // one step, whatever the size of the delta
    REQUIRE(step_row(Effect::hum, 20, -3) == 15);
}

TEST_CASE("tube groups: mask and look words", "[tube]") {
    REQUIRE(mask_choice(Mask::magentaGreen).label == "MAGENTA-GREEN");
    REQUIRE(mask_choice(Mask::none).label == "OFF");
    REQUIRE(mask_choice(Mask::slot).hint2 == "a 1990s TV set");
    for (auto m : kMasks) { REQUIRE_FALSE(mask_choice(m).hint1.empty()); REQUIRE_FALSE(mask_choice(m).hint2.empty()); }
    REQUIRE(look_choice(Look::tv).label == "TV");
    REQUIRE(look_choice(Look::everything).label == "Everything");
    REQUIRE(look_choice(Look::tv).hint1 == "a 1990s set, slot mask");
}
TEST_CASE("tube groups: a look's hint says whether it moves", "[tube]") {
    for (auto look : kLooks) {
        TubeSetting s; select_look(s, look);
        const bool says_still = look_choice(look).hint2.rfind("nothing moves", 0) == 0;
        INFO(look_choice(look).label);
        REQUIRE(says_still == !moves_on_its_own(resolve(s)));
    }
}
TEST_CASE("tube groups: no hint quotes the panel's millimetres", "[tube]") {
    for (auto e : kEffects) {
        REQUIRE(row_info(e).hint1.find(" mm") == std::string_view::npos);
        REQUIRE(row_info(e).hint2.find(" mm") == std::string_view::npos);
    }
    REQUIRE(row_info(Effect::rollWidth).hint2 == "0: 4 % · 100: 31 % of picture width");
    REQUIRE(row_info(Effect::puritySize).hint2 == "0: 5 % · 100: 36 % of picture width");
}
