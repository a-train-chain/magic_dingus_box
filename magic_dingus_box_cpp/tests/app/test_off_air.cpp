#include <catch2/catch_test_macros.hpp>
#include "app/off_air.h"

TEST_CASE("overnight return only leaves 00 for an available channel with no switch", "[off_air]") {
    app::AppState s; s.off_air_return_id = "movies:comedy"; s.tuned_channel = 0;
    CHECK(app::off_air_return_row(s, 3, true, false) == 3);
    CHECK_FALSE(app::off_air_return_row(s, 3, false, false));
    CHECK_FALSE(app::off_air_return_row(s, 3, true, true));
    CHECK_FALSE(app::off_air_return_row(s, -1, true, false));
    CHECK_FALSE(app::off_air_return_row(s, 0, true, false));
    s.guide_gave_up = {3};
    CHECK_FALSE(app::off_air_return_row(s, 3, true, false));
    s.guide_gave_up.clear();
    s.tuned_channel = 2;
    CHECK_FALSE(app::off_air_return_row(s, 3, true, false));
    s.tuned_channel = -1;
    CHECK_FALSE(app::off_air_return_row(s, 3, true, false));
    s.tuned_channel = 0; s.off_air_return_id.clear();
    CHECK_FALSE(app::off_air_return_row(s, 3, true, false));
}
