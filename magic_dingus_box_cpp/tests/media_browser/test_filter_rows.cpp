#include <catch2/catch_test_macros.hpp>

#include "media_browser/ui/mb_filter_overlay.h"

using media_browser::ui::FilterRowRole;
using media_browser::ui::FilterTabKind;
using media_browser::ui::filter_row_count;
using media_browser::ui::filter_row_role;
using media_browser::ui::filter_value_index;

TEST_CASE("row model: chart tabs have 9 rows, For You has 2", "[filter_rows]") {
    CHECK(filter_row_count(FilterTabKind::Popular) == 9);
    CHECK(filter_row_count(FilterTabKind::TopRated) == 9);
    CHECK(filter_row_count(FilterTabKind::ForYou) == 2);
}

TEST_CASE("row model: chart-tab roles", "[filter_rows]") {
    CHECK(filter_row_role(FilterTabKind::Popular, 0) == FilterRowRole::Mode);
    for (int row = 1; row <= 6; ++row) {
        CHECK(filter_row_role(FilterTabKind::Popular, row) == FilterRowRole::Value);
    }
    CHECK(filter_row_role(FilterTabKind::Popular, 7) == FilterRowRole::Reset);
    CHECK(filter_row_role(FilterTabKind::Popular, 8) == FilterRowRole::Shuffle);
    CHECK(filter_row_role(FilterTabKind::TopRated, 8) == FilterRowRole::Shuffle);
}

TEST_CASE("row model: For You is MODE then SHUFFLE only", "[filter_rows]") {
    CHECK(filter_row_role(FilterTabKind::ForYou, 0) == FilterRowRole::Mode);
    CHECK(filter_row_role(FilterTabKind::ForYou, 1) == FilterRowRole::Shuffle);
    // For You persists no filter state in either mode — it must expose no
    // Value row and no RESET ALL.
    for (int row = 0; row < filter_row_count(FilterTabKind::ForYou); ++row) {
        CHECK(filter_row_role(FilterTabKind::ForYou, row) != FilterRowRole::Value);
        CHECK(filter_row_role(FilterTabKind::ForYou, row) != FilterRowRole::Reset);
    }
}

TEST_CASE("row model: value index maps rows 1-6 onto 0-5", "[filter_rows]") {
    CHECK(filter_value_index(FilterTabKind::Popular, 1) == 0);   // GENRE
    CHECK(filter_value_index(FilterTabKind::Popular, 2) == 1);   // DECADE
    CHECK(filter_value_index(FilterTabKind::Popular, 3) == 2);   // MIN RATING
    CHECK(filter_value_index(FilterTabKind::Popular, 4) == 3);   // RUNTIME
    CHECK(filter_value_index(FilterTabKind::Popular, 5) == 4);   // LANGUAGE
    CHECK(filter_value_index(FilterTabKind::Popular, 6) == 5);   // ORDER
}

TEST_CASE("row model: non-value rows have no value index", "[filter_rows]") {
    CHECK(filter_value_index(FilterTabKind::Popular, 0) == -1);
    CHECK(filter_value_index(FilterTabKind::Popular, 7) == -1);
    CHECK(filter_value_index(FilterTabKind::Popular, 8) == -1);
    CHECK(filter_value_index(FilterTabKind::ForYou, 0) == -1);
    CHECK(filter_value_index(FilterTabKind::ForYou, 1) == -1);
    // Out-of-range rows must not be reported as editable values.
    CHECK(filter_value_index(FilterTabKind::Popular, 9) == -1);
    CHECK(filter_value_index(FilterTabKind::Popular, -1) == -1);
}
