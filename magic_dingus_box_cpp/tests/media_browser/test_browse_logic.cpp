#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

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

TEST_CASE("seed_pool: returns only the requested kind's ids", "[browse_logic]") {
    const std::unordered_set<MediaRef> refs = {
        MediaRef{MediaKind::Movie, 603}, MediaRef{MediaKind::Movie, 1396},
        MediaRef{MediaKind::Tv, 1396},   MediaRef{MediaKind::Tv, 1399},
    };
    auto movies = media_browser::ui::seed_pool(refs, MediaKind::Movie);
    std::sort(movies.begin(), movies.end());
    REQUIRE(movies.size() == 2);
    CHECK(movies[0] == 603);
    CHECK(movies[1] == 1396);

    auto shows = media_browser::ui::seed_pool(refs, MediaKind::Tv);
    std::sort(shows.begin(), shows.end());
    REQUIRE(shows.size() == 2);
    CHECK(shows[0] == 1396);
    CHECK(shows[1] == 1399);
}

TEST_CASE("seed_pool: empty when the kind is absent", "[browse_logic]") {
    const std::unordered_set<MediaRef> movies_only = {MediaRef{MediaKind::Movie, 1}};
    CHECK(media_browser::ui::seed_pool(movies_only, MediaKind::Tv).empty());
    CHECK(media_browser::ui::seed_pool({}, MediaKind::Movie).empty());
}

// --- decide_browse_grid_state ---------------------------------------------
// render() used to make this decision inline across five early returns, each
// of which had to remember to draw the modal filter overlay on its way out.
// One of them didn't, which is how pressing MODE made the panel vanish for
// the whole reload. The decision is pure; the drawing is not.

namespace {
media_browser::ui::BrowseStateInputs chart_inputs() {
    media_browser::ui::BrowseStateInputs in;
    in.is_foryou = false;
    in.grid_empty = false;
    in.loading = false;
    in.lib_refresh_done_once = true;
    in.lib_fetch_ok = true;
    in.seeds_empty = false;
    in.foryou_failed = false;
    in.has_api_key = true;
    return in;
}
}  // namespace

TEST_CASE("grid state: a populated grid wins over every service condition",
          "[browse_logic]") {
    using media_browser::ui::BrowseGridState;
    using media_browser::ui::decide_browse_grid_state;
    auto in = chart_inputs();
    in.lib_fetch_ok = false;           // library service down
    in.loading = true;                 // and a refresh in flight
    CHECK(decide_browse_grid_state(in) == BrowseGridState::Grid);
    in.is_foryou = true;               // same for For You, cache-first
    CHECK(decide_browse_grid_state(in) == BrowseGridState::Grid);
}

TEST_CASE("grid state: For You blocks on its library only when empty",
          "[browse_logic]") {
    using media_browser::ui::BrowseGridState;
    using media_browser::ui::decide_browse_grid_state;
    auto in = chart_inputs();
    in.is_foryou = true;
    in.grid_empty = true;
    in.lib_fetch_ok = false;
    CHECK(decide_browse_grid_state(in) == BrowseGridState::LibraryUnavailable);
    // A chart tab in the same state is NOT blocked — it is TMDB-sourced.
    in.is_foryou = false;
    CHECK(decide_browse_grid_state(in) == BrowseGridState::EmptyCategory);
    // ...and before the first refresh lands, nothing is claimed either way.
    in.is_foryou = true;
    in.lib_refresh_done_once = false;
    CHECK(decide_browse_grid_state(in) != BrowseGridState::LibraryUnavailable);
}

TEST_CASE("grid state: For You failure and empty-library precedence",
          "[browse_logic]") {
    using media_browser::ui::BrowseGridState;
    using media_browser::ui::decide_browse_grid_state;
    auto in = chart_inputs();
    in.is_foryou = true;
    in.foryou_failed = true;
    // foryou_failed_ is reported even with content on screen — matches the
    // shipped ordering, where that branch has no grid_empty guard.
    CHECK(decide_browse_grid_state(in) == BrowseGridState::RecommendationsFailed);

    // The only case that pins LibraryUnavailable vs. RecommendationsFailed
    // precedence: grid_empty && !lib_fetch_ok && foryou_failed all true at
    // once. Every other assertion in this file disqualifies one branch or
    // the other before both checks can fire, so swapping the two checks in
    // decide_browse_grid_state's is_foryou block still passed all of them.
    // The resolver checks LibraryUnavailable first, so it wins here.
    in.grid_empty = true;
    in.lib_fetch_ok = false;
    CHECK(decide_browse_grid_state(in) == BrowseGridState::LibraryUnavailable);
    in.grid_empty = false;
    in.lib_fetch_ok = true;

    in.foryou_failed = false;
    in.grid_empty = true;
    in.seeds_empty = true;
    CHECK(decide_browse_grid_state(in) == BrowseGridState::EmptyLibrary);
    in.loading = true;   // a sample in flight is Loading, not EmptyLibrary
    CHECK(decide_browse_grid_state(in) == BrowseGridState::Loading);
}

TEST_CASE("grid state: chart-tab loading / no-key / empty precedence",
          "[browse_logic]") {
    using media_browser::ui::BrowseGridState;
    using media_browser::ui::decide_browse_grid_state;
    auto in = chart_inputs();
    in.grid_empty = true;
    in.loading = true;
    CHECK(decide_browse_grid_state(in) == BrowseGridState::Loading);
    in.loading = false;
    in.has_api_key = false;
    CHECK(decide_browse_grid_state(in) == BrowseGridState::NoApiKey);
    in.has_api_key = true;
    CHECK(decide_browse_grid_state(in) == BrowseGridState::EmptyCategory);
}

// --- browse_grid_state_message ---------------------------------------------
// The switch in render() that used to own this text was verified by eye
// only; a future edit swapping "Sonarr"/"Radarr" (or TV/movie wording) in
// one arm would ship silently. Pin both tv_mode values for every state that
// names a service or a content kind.

TEST_CASE("browse_grid_state_message: mode-aware states name the right "
          "service/content kind",
          "[browse_logic]") {
    using media_browser::ui::browse_grid_state_message;
    using media_browser::ui::BrowseGridState;

    // sonarr_configured=true (a real SonarrClient was constructed) is the
    // baseline used everywhere below except the dedicated
    // never-configured test case.
    CHECK(std::string(browse_grid_state_message(
              BrowseGridState::LibraryUnavailable, /*tv_mode=*/false,
              /*sonarr_configured=*/true)) ==
          "Radarr service offline");
    CHECK(std::string(browse_grid_state_message(
              BrowseGridState::LibraryUnavailable, /*tv_mode=*/true,
              /*sonarr_configured=*/true)) ==
          "Sonarr service offline");

    CHECK(std::string(browse_grid_state_message(
              BrowseGridState::EmptyLibrary, /*tv_mode=*/false,
              /*sonarr_configured=*/true)) ==
          "Add movies to your library to get recommendations");
    CHECK(std::string(browse_grid_state_message(
              BrowseGridState::EmptyLibrary, /*tv_mode=*/true,
              /*sonarr_configured=*/true)) ==
          "Add TV shows to your library to get recommendations");

    CHECK(std::string(browse_grid_state_message(
              BrowseGridState::EmptyCategory, /*tv_mode=*/false,
              /*sonarr_configured=*/true)) ==
          "No movies in this category");
    CHECK(std::string(browse_grid_state_message(
              BrowseGridState::EmptyCategory, /*tv_mode=*/true,
              /*sonarr_configured=*/true)) ==
          "No shows in this category");
}

TEST_CASE("browse_grid_state_message: LibraryUnavailable in TV mode "
          "distinguishes never-configured from configured-but-unreachable",
          "[browse_logic]") {
    // Final-review Fix 1: main.cpp falls back to SonarrMockClient whenever
    // SONARR_API_KEY resolves empty, and that mock's get_library_checked()
    // permanently returns nullopt (63f9046) — so a box provisioned before
    // Sonarr existed hits LibraryUnavailable forever, not intermittently.
    // "Sonarr service offline" would accuse a service the box never had of
    // having gone down; sonarr_configured=false must say something true and
    // non-blaming instead.
    using media_browser::ui::browse_grid_state_message;
    using media_browser::ui::BrowseGridState;

    CHECK(std::string(browse_grid_state_message(
              BrowseGridState::LibraryUnavailable, /*tv_mode=*/true,
              /*sonarr_configured=*/false)) ==
          "TV library not set up on this box");

    // sonarr_configured is unused in Movies mode — Radarr is never mocked
    // in a way that reaches this path (Media Browser's feature gate hides
    // MB entirely when Radarr is unreachable), so the value must not
    // change the movie-mode string either way.
    CHECK(std::string(browse_grid_state_message(
              BrowseGridState::LibraryUnavailable, /*tv_mode=*/false,
              /*sonarr_configured=*/false)) ==
          "Radarr service offline");
    CHECK(std::string(browse_grid_state_message(
              BrowseGridState::LibraryUnavailable, /*tv_mode=*/false,
              /*sonarr_configured=*/true)) ==
          "Radarr service offline");
}

TEST_CASE("browse_grid_state_message: mode-invariant states and Grid",
          "[browse_logic]") {
    using media_browser::ui::browse_grid_state_message;
    using media_browser::ui::BrowseGridState;

    // Grid draws posters, not a message.
    CHECK(browse_grid_state_message(BrowseGridState::Grid, false, true) ==
          nullptr);
    CHECK(browse_grid_state_message(BrowseGridState::Grid, true, true) ==
          nullptr);

    // These three don't mention a service or content kind — same string in
    // both modes, regardless of sonarr_configured.
    for (bool tv : {false, true}) {
        for (bool sonarr_configured : {false, true}) {
            CHECK(std::string(browse_grid_state_message(
                      BrowseGridState::Loading, tv, sonarr_configured)) ==
                  "Loading...");
            CHECK(std::string(browse_grid_state_message(
                      BrowseGridState::RecommendationsFailed, tv,
                      sonarr_configured)) ==
                  "Couldn't load recommendations \xE2\x80\x94 try again later");
            CHECK(std::string(browse_grid_state_message(
                      BrowseGridState::NoApiKey, tv, sonarr_configured)) ==
                  "No TMDB key \xE2\x80\x94 add one in the Content Manager, "
                  "Media Browser tab");
        }
    }
}
