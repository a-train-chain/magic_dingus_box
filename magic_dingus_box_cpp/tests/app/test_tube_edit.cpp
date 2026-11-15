// tube::Edit - the TV menu's edit mode on one tube row: a turn steps from the
// RESOLVED value (the look's own when nothing is moved), the mask row cycles its
// five words, and leaving the edit asks for one save only when something changed.
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include "tube/tube_edit.h"
using namespace tube;

TEST_CASE("tube edit: a turn steps from the resolved value and saves once", "[tube]") {
    TubeSetting s; Edit e;                               // pvm: hum 0
    begin_edit(e, Effect::hum); REQUIRE(e.active);
    REQUIRE(edit_turn(e, s, +1)); REQUIRE(resolve(s).effects[index_of(Effect::hum)] == 5);
    REQUIRE(edit_turn(e, s, +1)); REQUIRE(resolve(s).effects[index_of(Effect::hum)] == 10);
    REQUIRE(edit_turn(e, s, -1)); REQUIRE(edit_turn(e, s, -1));
    REQUIRE(s == TubeSetting{});                         // back on the look: nothing moved
    REQUIRE(end_edit(e)); REQUIRE_FALSE(e.active);
    REQUIRE_FALSE(end_edit(e));                          // leaving twice saves once
}
TEST_CASE("tube edit: clamped turns change nothing; size rows step a pixel", "[tube]") {
    TubeSetting s; Edit e; begin_edit(e, Effect::hum);
    REQUIRE_FALSE(edit_turn(e, s, -1));
    REQUIRE_FALSE(end_edit(e));
    begin_edit(e, Effect::maskSize); edit_turn(e, s, +1);
    REQUIRE(resolve(s).effects[index_of(Effect::maskSize)] == 20);
}
TEST_CASE("tube edit: the mask row cycles its five words", "[tube]") {
    TubeSetting s; Edit e; begin_edit(e, std::nullopt);  // pvm: grille
    edit_turn(e, s, +1); REQUIRE(resolve(s).mask == Mask::magentaGreen);
    edit_turn(e, s, -1); edit_turn(e, s, -1); REQUIRE(resolve(s).mask == Mask::none);
    REQUIRE(next_mask(Mask::none, +1) == Mask::grille);
}
TEST_CASE("tube edit: knob and D-pad agree on up; an idle edit does nothing", "[tube]") {
    REQUIRE(edit_direction(+1, false) == +1); REQUIRE(edit_direction(-3, false) == -1);
    REQUIRE(edit_direction(-1, true) == +1);  REQUIRE(edit_direction(+1, true) == -1);
    REQUIRE(edit_direction(0, true) == 0);
    TubeSetting s; Edit e; REQUIRE_FALSE(edit_turn(e, s, +1)); REQUIRE(s == TubeSetting{});
}
TEST_CASE("tube edit: opening an edit over an open one keeps its pending change", "[tube]") {
    TubeSetting s; Edit e;
    begin_edit(e, Effect::hum); REQUIRE(edit_turn(e, s, +1));
    begin_edit(e, std::nullopt);                         // another row, the first unsaved
    REQUIRE(e.active); REQUIRE(e.mask);
    REQUIRE(end_edit(e));                                // the save is still asked for
    begin_edit(e, Effect::hum);                          // a fresh edit starts unchanged
    REQUIRE_FALSE(end_edit(e));
}
