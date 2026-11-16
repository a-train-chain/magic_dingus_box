#include <catch2/catch_test_macros.hpp>
#include <optional>
#include "tube/tube_rows.h"
using namespace tube;

TEST_CASE("tube rows: 44 effects in Sound Factory's order", "[tube]") {
    REQUIRE(kEffects.size() == 44);
    REQUIRE(effect_key(kEffects.front()) == "brightness");
    REQUIRE(index_of(Effect::breathing) == 26);
    REQUIRE(index_of(Effect::rollBand) == 27);
    REQUIRE(effect_key(kEffects.back()) == "purityPosition");
    for (std::size_t i = 0; i < kEffects.size(); ++i) {
        REQUIRE(index_of(kEffects[i]) == i);
        REQUIRE(parse_effect(effect_key(kEffects[i])) == kEffects[i]);
    }
}
TEST_CASE("tube rows: words are exact", "[tube]") {
    REQUIRE(parse_effect("Brightness") == std::nullopt);
    REQUIRE(parse_effect("") == std::nullopt);
    REQUIRE(mask_name(Mask::magentaGreen) == "magentaGreen");
    REQUIRE(parse_mask("slot") == Mask::slot);
    REQUIRE(parse_mask("Slot") == std::nullopt);
    REQUIRE(kLooks.size() == 6);
    REQUIRE(look_name(Look::everything) == "everything");
    REQUIRE(parse_look("pvm") == Look::pvm);
    REQUIRE(kLookDefault == Look::pvm);
}
TEST_CASE("tube rows: the range", "[tube]") {
    REQUIRE(is_row_value(0));
    REQUIRE(is_row_value(100));
    REQUIRE_FALSE(is_row_value(-1));
    REQUIRE_FALSE(is_row_value(101));
}
