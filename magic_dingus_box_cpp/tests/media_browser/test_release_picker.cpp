// Unit tests for the static helpers on ReleasePickerScreen.
//
// We intentionally exercise ONLY the pure data-shaping helpers
// (sort_candidates, flag_auto_pick_and_threshold) — the screen's render()
// and handle_input() touch the Renderer / Toast / RadarrClient and are
// out of scope for the unit-test binary, which links neither the GL
// renderer nor the toast subsystem.

#include <catch2/catch_test_macros.hpp>
#include "media_browser/ui/release_picker_screen.h"

namespace mbu = media_browser::ui;

namespace {
mbu::ReleasePickerScreen::ReleaseCandidate mk(int seeders, int score,
                                              const std::string& title) {
    mbu::ReleasePickerScreen::ReleaseCandidate c;
    c.title = title;
    c.seeders = seeders;
    c.score = score;
    return c;
}
}  // namespace

TEST_CASE("sort_candidates orders by seeders desc, then score desc",
          "[picker][sort]") {
    std::vector<mbu::ReleasePickerScreen::ReleaseCandidate> rows = {
        mk(50, 80, "low-seed"),
        mk(200, 80, "high-seed"),
        mk(50, 100, "low-seed-better-score"),
    };
    mbu::ReleasePickerScreen::sort_candidates(rows);
    REQUIRE(rows[0].title == "high-seed");
    REQUIRE(rows[1].title == "low-seed-better-score");
    REQUIRE(rows[2].title == "low-seed");
}

TEST_CASE("flag_auto_pick_and_threshold marks Radarr's choice + below-threshold rows",
          "[picker][flags]") {
    std::vector<mbu::ReleasePickerScreen::ReleaseCandidate> rows = {
        mk(200, 80, "best"),
        mk(180, 80, "second"),
        mk(50, -300, "rejected"),  // below the -200 minFormatScore floor
    };
    mbu::ReleasePickerScreen::flag_auto_pick_and_threshold(
        rows, /*min_format_score=*/-200);
    REQUIRE(rows[0].would_auto_pick == true);
    REQUIRE(rows[1].would_auto_pick == false);
    REQUIRE(rows[2].below_threshold == true);
    REQUIRE(rows[0].below_threshold == false);
}

TEST_CASE("flag_auto_pick handles all-below-threshold case (no auto-pick)",
          "[picker][flags]") {
    std::vector<mbu::ReleasePickerScreen::ReleaseCandidate> rows = {
        mk(200, -300, "all-rejected-1"),
        mk(50,  -250, "all-rejected-2"),
    };
    mbu::ReleasePickerScreen::flag_auto_pick_and_threshold(rows, -200);
    REQUIRE(rows[0].would_auto_pick == false);
    REQUIRE(rows[1].would_auto_pick == false);
    REQUIRE(rows[0].below_threshold == true);
    REQUIRE(rows[1].below_threshold == true);
}
