#include <catch2/catch_test_macros.hpp>
#include <string_view>
#include <vector>
#include "tube/tube_setting.h"
using namespace tube;

TEST_CASE("tube setting: a look untouched is not custom", "[tube]") {
    TubeSetting s; select_look(s, Look::tv);
    auto r = resolve(s);
    REQUIRE_FALSE(r.custom);
    REQUIRE(r.mask == Mask::slot);
    REQUIRE(r.effects == look_values(Look::tv).effects);
    REQUIRE(moved_keys(s).empty());
}
TEST_CASE("tube setting: a moved row sits on top of the look", "[tube]") {
    TubeSetting s;                                   // pvm
    REQUIRE(set_row(s, Effect::hum, 20));
    set_mask(s, Mask::dots);
    auto r = resolve(s);
    REQUIRE(r.custom);
    REQUIRE(r.effects[index_of(Effect::hum)] == 20);
    REQUIRE(r.effects[index_of(Effect::brightness)] == 55);
    REQUIRE(r.mask == Mask::dots);
    REQUIRE(moved_keys(s) == std::vector<std::string_view>{"hum", "mask"});
}
TEST_CASE("tube setting: moving a row back to the look's value un-moves it", "[tube]") {
    TubeSetting s; set_row(s, Effect::hum, 20); set_row(s, Effect::hum, 0);
    REQUIRE_FALSE(resolve(s).custom);
    REQUIRE(s == TubeSetting{});
}
TEST_CASE("tube setting: out of range is refused", "[tube]") {
    TubeSetting s;
    REQUIRE_FALSE(set_row(s, Effect::hum, 101));
    REQUIRE_FALSE(set_row(s, Effect::hum, -1));
    REQUIRE(s == TubeSetting{});
}
TEST_CASE("tube setting: picking a look clears what was moved", "[tube]") {
    TubeSetting s; set_row(s, Effect::hum, 20); set_mask(s, Mask::dots);
    select_look(s, Look::arcade);
    REQUIRE(moved_keys(s).empty());
    REQUIRE(resolve(s).mask == Mask::dots);          // arcade's own
    set_row(s, Effect::noise, 40); reset_to_look(s);
    REQUIRE(s.look == Look::arcade);
    REQUIRE_FALSE(resolve(s).custom);
}

TEST_CASE("tube setting: a row stated at the look's own value is not custom", "[tube]") {
    TubeSetting s;                                   // pvm, fields written directly
    s.rows[index_of(Effect::hum)] = look_values(Look::pvm).effects[index_of(Effect::hum)];
    s.mask = look_values(Look::pvm).mask;
    auto r = resolve(s);
    REQUIRE_FALSE(r.custom);
    REQUIRE(moved_keys(s).empty());
    REQUIRE(r.effects == look_values(Look::pvm).effects);
    REQUIRE(r.mask == look_values(Look::pvm).mask);
}
TEST_CASE("tube setting: custom agrees with moved_keys for a directly written difference", "[tube]") {
    TubeSetting s;
    s.rows[index_of(Effect::hum)] = 20;
    REQUIRE(resolve(s).custom);
    REQUIRE(moved_keys(s) == std::vector<std::string_view>{"hum"});
    TubeSetting m;
    m.mask = Mask::dots;
    REQUIRE(resolve(m).custom == !moved_keys(m).empty());
    REQUIRE(resolve(m).custom);
}
