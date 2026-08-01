#include <catch2/catch_test_macros.hpp>
#include <string>
#include <unordered_set>

#include "media_browser/media_ref.h"
#include "media_browser/ui/browse_logic.h"

using namespace media_browser::ui;
using clock_tp = std::chrono::steady_clock::time_point;
using media_browser::MediaKind;
using media_browser::MediaRef;
using media_browser::ui::replace_refs_of_kind;

TEST_CASE("tmdb_grid_stale: default timestamp is stale; 6h TTL", "[browse_logic]") {
    const clock_tp t0{std::chrono::hours(1000)};
    CHECK(tmdb_grid_stale(clock_tp{}, t0));                          // never loaded
    CHECK_FALSE(tmdb_grid_stale(t0, t0 + std::chrono::hours(5)));    // fresh
    CHECK_FALSE(tmdb_grid_stale(t0, t0 + std::chrono::hours(6)));    // boundary: exactly 6h is fresh
    CHECK(tmdb_grid_stale(t0, t0 + std::chrono::hours(7)));          // stale
}

TEST_CASE("window_last_page is base-relative", "[browse_logic]") {
    CHECK(window_last_page(1) == 5);
    CHECK(window_last_page(12) == 16);
    CHECK(window_last_page(26) == 30);
}

TEST_CASE("discover_max_base clamps to known total_pages", "[browse_logic]") {
    CHECK(discover_max_base(0) == 26);     // unknown → optimistic ceiling
    CHECK(discover_max_base(-1) == 26);
    CHECK(discover_max_base(500) == 26);   // huge → ceiling
    CHECK(discover_max_base(8) == 4);      // 8 - 5 + 1
    CHECK(discover_max_base(5) == 1);
    CHECK(discover_max_base(2) == 1);      // fewer pages than the window → base 1 only
}

TEST_CASE("pick_shuffle_base excludes current base when possible", "[browse_logic]") {
    // Collapsed range → plain refetch of page 1.
    CHECK(pick_shuffle_base(1, 1, 0u) == 1);
    CHECK(pick_shuffle_base(1, 0, 7u) == 1);
    // Current base outside range → plain uniform draw over 1..max.
    CHECK(pick_shuffle_base(0, 4, 0u) == 1);
    CHECK(pick_shuffle_base(0, 4, 3u) == 4);
    CHECK(pick_shuffle_base(99, 4, 5u) == 2);  // 5 % 4 = 1 → base 2
    // Exclusion: current=3, max=5 → candidates {1,2,4,5} in rand order.
    CHECK(pick_shuffle_base(3, 5, 0u) == 1);
    CHECK(pick_shuffle_base(3, 5, 1u) == 2);
    CHECK(pick_shuffle_base(3, 5, 2u) == 4);
    CHECK(pick_shuffle_base(3, 5, 3u) == 5);
    CHECK(pick_shuffle_base(3, 5, 4u) == 1);   // wraps
    // Exclusion with exactly 2 candidates always picks the other one.
    CHECK(pick_shuffle_base(1, 2, 0u) == 2);
    CHECK(pick_shuffle_base(2, 2, 0u) == 1);
    CHECK(pick_shuffle_base(1, 2, 41u) == 2);
}

TEST_CASE("decide_foryou_entry three-way rule", "[browse_logic]") {
    // Cached list always wins — activation never refetches (spec 1c).
    CHECK(decide_foryou_entry(true, true, true, false) == ForYouEntry::UseCache);
    CHECK(decide_foryou_entry(true, false, false, true) == ForYouEntry::UseCache);
    // No refresh completed yet → wait (caller shows Loading + kicks a refresh).
    CHECK(decide_foryou_entry(false, false, false, false) == ForYouEntry::WaitForLibrary);
    // Refresh done but library GET failed → service state, NOT the teach message.
    CHECK(decide_foryou_entry(false, true, false, true) == ForYouEntry::ServiceUnavailable);
    // Refresh done, fetch ok, genuinely empty → teach message.
    CHECK(decide_foryou_entry(false, true, true, true) == ForYouEntry::EmptyLibrary);
    // Refresh done, fetch ok, library populated → sample.
    CHECK(decide_foryou_entry(false, true, true, false) == ForYouEntry::Sample);
}

// --- replace_refs_of_kind (Phase 2c-1) -------------------------------------
// The library cache holds BOTH kinds in one set. A refresh where only one
// service answered must replace that service's kind and leave the other
// alone — otherwise a Sonarr blip would silently disable the movie
// in-library hide, and vice versa.

TEST_CASE("replace_refs_of_kind: replaces only the named kind", "[browse_logic]") {
    std::unordered_set<MediaRef> dst = {
        MediaRef{MediaKind::Movie, 1}, MediaRef{MediaKind::Movie, 2},
        MediaRef{MediaKind::Tv, 1396},
    };
    const std::unordered_set<MediaRef> fresh = {MediaRef{MediaKind::Movie, 3}};
    replace_refs_of_kind(dst, MediaKind::Movie, fresh);
    REQUIRE(dst.size() == 2);
    CHECK(dst.count(MediaRef{MediaKind::Movie, 3}) == 1);
    CHECK(dst.count(MediaRef{MediaKind::Movie, 1}) == 0);
    CHECK(dst.count(MediaRef{MediaKind::Tv, 1396}) == 1);
}

TEST_CASE("replace_refs_of_kind: an empty fresh set clears only that kind",
          "[browse_logic]") {
    std::unordered_set<MediaRef> dst = {
        MediaRef{MediaKind::Movie, 1}, MediaRef{MediaKind::Tv, 1396},
    };
    replace_refs_of_kind(dst, MediaKind::Tv, {});
    REQUIRE(dst.size() == 1);
    CHECK(dst.count(MediaRef{MediaKind::Movie, 1}) == 1);
}

TEST_CASE("replace_refs_of_kind: adding TV leaves movies intact",
          "[browse_logic]") {
    std::unordered_set<MediaRef> dst = {MediaRef{MediaKind::Movie, 1396}};
    const std::unordered_set<MediaRef> fresh = {MediaRef{MediaKind::Tv, 1396}};
    replace_refs_of_kind(dst, MediaKind::Tv, fresh);
    REQUIRE(dst.size() == 2);
    CHECK(dst.count(MediaRef{MediaKind::Movie, 1396}) == 1);
    CHECK(dst.count(MediaRef{MediaKind::Tv, 1396}) == 1);
}

TEST_CASE("marquee title carries the mode indicator", "[browse_logic]") {
    // The strip is at its width limit and draw_screen_header has no overflow
    // guard, so the mode marker lives in the TITLE, not in a chip label.
    // Measured clearance at 1280 logical px: +45 px (see the plan).
    CHECK(std::string(media_browser::ui::marquee_title_for_mode(false)) == "Marquee");
    CHECK(std::string(media_browser::ui::marquee_title_for_mode(true)) == "Marquee TV");
}
