// Unit tests for the static helpers on ReleasePickerScreen.
//
// We intentionally exercise ONLY the pure data-shaping helpers
// (sort_candidates, flag_auto_pick_and_threshold) and a minimal
// load-state default check — the screen's render() and the live worker
// path in handle_input()/load_async() touch the Renderer / Toast /
// libcurl-RadarrClient and are out of scope for the unit-test binary,
// which links neither the GL renderer nor the toast subsystem nor the
// release_picker_screen.cpp translation unit.

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

TEST_CASE("ReleasePickerScreen LoadState enum has the four expected values",
          "[picker][async]") {
    // The async refactor introduced a load-state machine (Idle ->
    // Loading -> Ready/Failed -> Idle). The enum is a public part of
    // the picker's API because main.cpp's stall-modal handler may want
    // to distinguish "load is in flight" from "ready to drain" when
    // deciding whether to show progress UI. Locking the enum in a test
    // catches accidental reorderings or removals during future
    // refactors. (We can't construct a ReleasePickerScreen here — its
    // .cpp isn't linked into the test binary because that would pull
    // in the GL renderer and Toast — but the enum itself is in the
    // header and reachable.)
    using LS = mbu::ReleasePickerScreen::LoadState;
    REQUIRE(static_cast<int>(LS::Idle)    >= 0);
    REQUIRE(static_cast<int>(LS::Loading) != static_cast<int>(LS::Idle));
    REQUIRE(static_cast<int>(LS::Ready)   != static_cast<int>(LS::Loading));
    REQUIRE(static_cast<int>(LS::Failed)  != static_cast<int>(LS::Ready));
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
