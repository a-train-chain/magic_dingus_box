// Table tests for browse_view.h — BrowseScreen's input mapping, page drain /
// merge / load-more verdicts, enter() dispatch, library-ref collection and
// composed text, moved out of browse_screen.cpp so they run on the Mac.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <unordered_set>
#include <vector>

#include "media_browser/ui/browse_view.h"

namespace mb = media_browser;
using namespace media_browser::ui;
using C = BrowseCategory;

namespace {

mb::TmdbSearchHit hit(int id, mb::MediaKind kind = mb::MediaKind::Movie,
                      const std::string& title = "") {
    mb::TmdbSearchHit h;
    h.tmdb_id = id;
    h.kind = kind;
    h.title = title;
    return h;
}

std::vector<int> ids(const std::vector<mb::TmdbSearchHit>& v) {
    std::vector<int> out;
    for (const auto& h : v) out.push_back(h.tmdb_id);
    return out;
}

mb::MediaRef kMovie(int id) { return mb::MediaRef{mb::MediaKind::Movie, id}; }
mb::MediaRef kTv(int id) { return mb::MediaRef{mb::MediaKind::Tv, id}; }

}  // namespace

// ---------------------------------------------------------------------------
// Categories / strip
// ---------------------------------------------------------------------------

TEST_CASE("browse view: category vocabulary", "[browse_view]") {
    CHECK_FALSE(browse_is_nav_chip(C::Popular));
    CHECK_FALSE(browse_is_nav_chip(C::ForYou));
    CHECK(browse_is_nav_chip(C::Search));
    CHECK(browse_is_nav_chip(C::Settings));

    CHECK(kBrowseNumVisibleTabs == 7);
    CHECK(std::string(browse_category_label(C::TopRated)) == "Top Rated");
    CHECK(std::string(browse_category_label(C::NowPlaying)) == "Now Playing");
    CHECK(std::string(browse_category_label(C::ForYou)) == "For You");

    CHECK(browse_strip_position(C::Popular) == 0);
    CHECK(browse_strip_position(C::ForYou) == 2);
    CHECK(browse_strip_position(C::Settings) == 6);
    // Not a Marquee tab (legacy persistence): Popular's slot.
    CHECK(browse_strip_position(C::NowPlaying) == 0);
    CHECK(browse_strip_position(C::Filter) == 0);

    CHECK(browse_nav_screen(C::Library) == Screen::Library);
    CHECK(browse_nav_screen(C::Search) == Screen::Search);
    CHECK(browse_nav_screen(C::Queue) == Screen::Queue);
    CHECK(browse_nav_screen(C::Settings) == Screen::MovieSettings);
    CHECK_FALSE(browse_nav_screen(C::TopRated).has_value());
}

TEST_CASE("browse view: BTN1/BTN3 tab steps", "[browse_view]") {
    using K = BrowseTabStep::Kind;
    CHECK(browse_tab_step(0, -1).kind == K::None);   // no wrap
    CHECK(browse_tab_step(6, +1).kind == K::None);
    auto s = browse_tab_step(0, +1);
    CHECK(s.kind == K::Activate);
    CHECK(s.category == C::TopRated);
    CHECK(s.strip_pos == 1);
    s = browse_tab_step(2, -1);
    CHECK(s.kind == K::Activate);
    CHECK(s.category == C::TopRated);
    // For You → Search is a transition.
    s = browse_tab_step(2, +1);
    CHECK(s.kind == K::Navigate);
    CHECK(s.screen == Screen::Search);
    s = browse_tab_step(5, +1);
    CHECK(s.kind == K::Navigate);
    CHECK(s.screen == Screen::MovieSettings);
}

TEST_CASE("browse view: chart / overlay tabs", "[browse_view]") {
    CHECK(is_chart_category(C::Popular));
    CHECK(is_chart_category(C::TopRated));
    CHECK_FALSE(is_chart_category(C::ForYou));
    CHECK(chart_filter_tab(C::Popular) == FilterTabKind::Popular);
    CHECK(chart_filter_tab(C::TopRated) == FilterTabKind::TopRated);
    CHECK(overlay_tab_for(C::Popular) == FilterTabKind::Popular);
    CHECK(overlay_tab_for(C::TopRated) == FilterTabKind::TopRated);
    CHECK(overlay_tab_for(C::ForYou) == FilterTabKind::ForYou);
    CHECK_FALSE(overlay_tab_for(C::Filter).has_value());
    CHECK_FALSE(overlay_tab_for(C::Library).has_value());
    CHECK(chart_shuffle_max_base(C::Popular) == kShuffleMaxBasePopular);
    CHECK(chart_shuffle_max_base(C::TopRated) == kShuffleMaxBaseTopRated);
}

// ---------------------------------------------------------------------------
// Grid navigation
// ---------------------------------------------------------------------------

TEST_CASE("browse view: grid cursor movement", "[browse_view]") {
    CHECK(grid_cursor_after_rotate(3, 1, 20) == 4);
    CHECK(grid_cursor_after_rotate(0, -1, 20) == 0);
    CHECK(grid_cursor_after_rotate(19, 5, 20) == 19);

    // 20 posters, 9 cols: rows 0..2, last row holds 18, 19.
    CHECK(grid_cursor_after_vertical(4, 1, 20, 9) == 13);
    CHECK(grid_cursor_after_vertical(13, -1, 20, 9) == 4);
    CHECK(grid_cursor_after_vertical(13, 1, 20, 9) == 19);  // short row: last poster
    CHECK(grid_cursor_after_vertical(10, 1, 20, 9) == 19);  // column 1 → idx 19 exists
    CHECK(grid_cursor_after_vertical(1, 1, 20, 9) == 10);
    CHECK(grid_cursor_after_vertical(2, -3, 20, 9) == 2);   // clamp at row 0
    CHECK(grid_cursor_after_vertical(2, 9, 20, 9) == 19);

    CHECK(grid_page_first_row(0, 9) == 0);
    CHECK(grid_page_first_row(17, 9) == 0);
    CHECK(grid_page_first_row(18, 9) == 2);
    CHECK(grid_page_first_row(40, 9) == 4);

    CHECK(browse_select_destination(mb::MediaKind::Tv) == Screen::SeriesDetail);
    CHECK(browse_select_destination(mb::MediaKind::Movie) == Screen::Detail);
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

TEST_CASE("browse view: page drain verdicts", "[browse_view]") {
    using D = PageDrain;
    CHECK(decide_page_drain(false, false, true, false, 1, 1) == D::Stale);
    CHECK(decide_page_drain(false, true, false, true, 1, 1) == D::Stale);  // stale first
    CHECK(decide_page_drain(true, true, false, false, 1, 1) == D::KeepStaleGrid);
    CHECK(decide_page_drain(true, true, true, true, 1, 1) == D::KeepStaleGrid);
    CHECK(decide_page_drain(true, true, true, false, 1, 1) == D::Apply);
    // A shuffled base page that is genuinely empty.
    CHECK(decide_page_drain(true, false, true, true, 7, 7) == D::ShuffleFallback);
    CHECK(decide_page_drain(true, false, true, true, 1, 1) == D::Apply);   // base 1
    CHECK(decide_page_drain(true, false, false, true, 7, 7) == D::Apply);  // failed
    CHECK(decide_page_drain(true, false, true, true, 8, 7) == D::Apply);   // not base
}

TEST_CASE("browse view: page merge dedupes and hides owned movies only",
          "[browse_view]") {
    std::vector<mb::TmdbSearchHit> grid{hit(99)};
    std::unordered_set<mb::MediaRef> loaded{kMovie(99)};
    const std::unordered_set<mb::MediaRef> library{kMovie(2), kTv(3)};

    std::vector<mb::TmdbSearchHit> page{hit(1), hit(2), hit(1),
                                        hit(3, mb::MediaKind::Tv),
                                        hit(2, mb::MediaKind::Tv)};
    auto c = merge_page_hits(grid, loaded, library, page, /*replace=*/true);
    // Replace: the old grid and seen-set are gone.
    CHECK(ids(grid) == std::vector<int>{1, 3, 2});
    CHECK(c.added == 3);
    CHECK(c.dups == 1);
    CHECK(c.owned == 1);  // movie 2 hidden; owned TV 3 stays (badged)
    CHECK(loaded.count(kMovie(2)) == 1);  // hidden id still marked seen
    CHECK(loaded.count(kMovie(99)) == 0);

    // Append: a later page cannot resurrect hidden movie 2 or repeat 1.
    std::vector<mb::TmdbSearchHit> page2{hit(2), hit(1), hit(4)};
    c = merge_page_hits(grid, loaded, library, page2, /*replace=*/false);
    CHECK(ids(grid) == std::vector<int>{1, 3, 2, 4});
    CHECK(c.added == 1);
    CHECK(c.dups == 2);
    CHECK(c.owned == 0);
}

TEST_CASE("browse view: chart timestamp rule and retro-hide", "[browse_view]") {
    CHECK(chart_stamp_after_base_page(true, false) == ChartStamp::Fresh);
    CHECK(chart_stamp_after_base_page(false, false) == ChartStamp::Clear);
    CHECK(chart_stamp_after_base_page(false, true) == ChartStamp::Clear);
    CHECK(chart_stamp_after_base_page(true, true) == ChartStamp::Keep);

    std::vector<mb::TmdbSearchHit> grid{hit(1), hit(2), hit(2, mb::MediaKind::Tv),
                                        hit(3)};
    const std::unordered_set<mb::MediaRef> library{kMovie(2), kTv(2), kMovie(3)};
    CHECK(erase_owned_movies(grid, library) == 2);
    CHECK(grid.size() == 2);
    CHECK(grid[1].kind == mb::MediaKind::Tv);
    CHECK(erase_owned_movies(grid, library) == 0);
}

TEST_CASE("browse view: load-more", "[browse_view]") {
    LoadMoreInputs in;
    in.next_page = 2;
    in.window_base = 1;
    in.grid_count = 20;
    in.cursor = 0;
    CHECK(decide_load_more(in) == LoadMore::PrefetchSecond);

    auto t = in;
    t.fetching_more = true;
    CHECK(decide_load_more(t) == LoadMore::None);
    t = in;
    t.loading = true;
    CHECK(decide_load_more(t) == LoadMore::None);
    t = in;
    t.more_available = false;
    CHECK(decide_load_more(t) == LoadMore::None);
    t = in;
    t.nav_chip = true;
    CHECK(decide_load_more(t) == LoadMore::None);
    t = in;
    t.for_you = true;
    CHECK(decide_load_more(t) == LoadMore::None);

    // Page 3, cursor not yet on the last loaded row.
    t = in;
    t.next_page = 3;
    t.grid_count = 40;  // rows 0..4
    t.cursor = 20;      // row 2
    CHECK(decide_load_more(t) == LoadMore::None);
    t.cursor = 36;      // row 4 = last
    CHECK(decide_load_more(t) == LoadMore::ScrollDriven);
    // Past the base-relative window (base 7 → pages 7..11).
    t.window_base = 7;
    t.next_page = 12;
    CHECK(decide_load_more(t) == LoadMore::None);
    t.next_page = 11;
    CHECK(decide_load_more(t) == LoadMore::ScrollDriven);
    // Shuffled window: the second page is base + 1.
    t.next_page = 8;
    t.cursor = 0;
    CHECK(decide_load_more(t) == LoadMore::PrefetchSecond);
    // Empty grid, not the second page: nothing to be near the end of.
    t.next_page = 9;
    t.grid_count = 0;
    CHECK(decide_load_more(t) == LoadMore::None);
}

TEST_CASE("browse view: enter() dispatch", "[browse_view]") {
    using E = BrowseEnter;
    CHECK(decide_browse_enter(false, C::Popular, true, true, true) == E::LoadCategory);
    CHECK(decide_browse_enter(false, C::ForYou, true, false, true) == E::LoadCategory);
    CHECK(decide_browse_enter(true, C::ForYou, true, false, false) == E::ForYouRevalidate);
    CHECK(decide_browse_enter(true, C::ForYou, true, true, false) == E::ForYouActivate);
    CHECK(decide_browse_enter(true, C::ForYou, false, true, false) == E::ForYouActivate);
    CHECK(decide_browse_enter(true, C::ForYou, false, false, true) == E::None);
    CHECK(decide_browse_enter(true, C::Popular, false, false, true) == E::ChartRevalidate);
    CHECK(decide_browse_enter(true, C::TopRated, true, true, false) == E::None);
    CHECK(decide_browse_enter(true, C::Filter, false, false, true) == E::None);
    CHECK(decide_browse_enter(true, C::Library, false, false, true) == E::None);
}

// ---------------------------------------------------------------------------
// Library refs
// ---------------------------------------------------------------------------

TEST_CASE("browse view: library ref collection", "[browse_view]") {
    std::vector<mb::Movie> lib(3);
    lib[0].tmdb_id = 10; lib[0].radarr_id = 1;
    lib[1].tmdb_id = 0;  lib[1].radarr_id = 2;   // no tmdb id: skipped
    lib[2].tmdb_id = 30; lib[2].radarr_id = 3;
    std::vector<mb::QueueItem> queue(3);
    queue[0].movie_id = 3;
    queue[1].movie_id = 2;   // its movie has no tmdb id
    queue[2].movie_id = 77;  // not in the library
    const auto refs = mb::ui::collect_movie_library_refs(lib, queue);
    CHECK(refs.movie_refs ==
          std::unordered_set<mb::MediaRef>{kMovie(10), kMovie(30)});
    CHECK(refs.downloading_refs == std::unordered_set<mb::MediaRef>{kMovie(30)});

    std::vector<mb::Series> shows(2);
    shows[0].tmdb_id = 1396;
    shows[1].tmdb_id = 0;
    CHECK(collect_tv_library_refs(shows) == std::unordered_set<mb::MediaRef>{kTv(1396)});

    const std::unordered_set<mb::MediaRef> mixed{kMovie(1), kTv(2)};
    CHECK(has_ref_of_kind(mixed, mb::MediaKind::Tv));
    CHECK(has_ref_of_kind(mixed, mb::MediaKind::Movie));
    CHECK_FALSE(has_ref_of_kind({kMovie(1)}, mb::MediaKind::Tv));
    CHECK_FALSE(has_ref_of_kind({}, mb::MediaKind::Movie));
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

TEST_CASE("browse view: service warning line", "[browse_view]") {
    using S = BrowseGridState;
    const std::string dash = "\xE2\x80\x94";
    CHECK(browse_service_warning(false, false, S::Grid, false, true) == nullptr);
    CHECK(browse_service_warning(true, true, S::Grid, false, true) == nullptr);
    CHECK(browse_service_warning(true, false, S::LibraryUnavailable, true, true) ==
          nullptr);
    CHECK(std::string(browse_service_warning(true, false, S::Grid, false, false)) ==
          "Radarr offline " + dash + " in-library hiding may be stale");
    CHECK(std::string(browse_service_warning(true, false, S::Loading, true, true)) ==
          "Sonarr offline " + dash + " in-library hiding may be stale");
    CHECK(std::string(browse_service_warning(true, false, S::Grid, true, false)) ==
          "TV library not set up " + dash + " in-library hiding unavailable");
}

TEST_CASE("browse view: grid-state tone", "[browse_view]") {
    using S = BrowseGridState;
    CHECK(browse_grid_state_tone(S::LibraryUnavailable) == MbTone::Highlight2);
    CHECK(browse_grid_state_tone(S::RecommendationsFailed) == MbTone::Highlight2);
    CHECK(browse_grid_state_tone(S::NoApiKey) == MbTone::Highlight2);
    CHECK(browse_grid_state_tone(S::Grid) == MbTone::Dim);
    CHECK(browse_grid_state_tone(S::Loading) == MbTone::Dim);
    CHECK(browse_grid_state_tone(S::EmptyLibrary) == MbTone::Dim);
    CHECK(browse_grid_state_tone(S::EmptyCategory) == MbTone::Dim);
}

TEST_CASE("browse view: footer hints", "[browse_view]") {
    auto h = browse_footer_hints(true, false);
    REQUIRE(h.size() == 6);
    CHECK(h[0].action == "Tab \xE2\x86\x90");
    CHECK(h[3].action == "Filters");
    CHECK(h[5].action == "Detail");
    CHECK(browse_footer_hints(false, true)[3].action == "Mode/Shuffle");
    CHECK(browse_footer_hints(false, false)[3].action == "\xE2\x80\x94");
}

TEST_CASE("browse view: poster title split", "[browse_view]") {
    // 10 px per byte.
    auto measure = [](const std::string& s) { return static_cast<float>(s.size() * 10); };
    auto s = split_poster_title("Heat", 100.0f, measure);
    CHECK(s.fits);

    // "The Lord of the Rings": longest word prefix within 100 px (10 bytes).
    s = split_poster_title("The Lord of the Rings", 100.0f, measure);
    CHECK_FALSE(s.fits);
    REQUIRE(s.split != std::string::npos);
    CHECK(std::string("The Lord of the Rings").substr(0, s.split) == "The Lord");

    // No word boundary fits: line 1 is the truncated whole title.
    s = split_poster_title("Supercalifragilistic Movie", 100.0f, measure);
    CHECK_FALSE(s.fits);
    CHECK(s.split == std::string::npos);
    s = split_poster_title("Unbreakablelongword", 100.0f, measure);
    CHECK(s.split == std::string::npos);

    // A leading space is never a split point (the scan starts after byte 0).
    s = split_poster_title(" Leading space title", 100.0f, measure);
    REQUIRE(s.split != std::string::npos);
    CHECK(s.split == 8);
}
