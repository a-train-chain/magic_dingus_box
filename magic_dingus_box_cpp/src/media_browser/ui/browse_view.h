#pragma once

// BrowseScreen's remaining decisions, Renderer-free and I/O-free so
// test_media_browser_unit can assert on them
// (tests/media_browser/test_browse_view.cpp): the category / tab-strip
// vocabulary, BTN1/BTN3/rotary/SELECT input mapping, the TMDB page-drain and
// load-more verdicts, the page merge (dedupe + in-library hide), the enter()
// TTL dispatch, the library-refresh ref collection, the service-warning
// line, the grid-state tone, the footer and the poster-title line split.
// browse_logic.h keeps the TTL / shuffle / For You / grid-state helpers it
// already held; together they leave BrowseScreen with thread plumbing and
// paint.
//
// Everything here was moved out of browse_screen.cpp verbatim in behaviour:
// same strings, same clamps, same precedence.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "media_browser/media_ref.h"
#include "media_browser/radarr/radarr_types.h"
#include "media_browser/sonarr/sonarr_types.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/ui/browse_logic.h"
#include "media_browser/ui/mb_chrome.h"
#include "media_browser/ui/mb_filter_overlay.h"
#include "media_browser/ui/mb_screen.h"
#include "media_browser/ui/mb_tone.h"

namespace media_browser::ui {

// ---------- Categories and the tab strip ----------

// Top-strip chip order. Content chips load a grid; nav chips transition.
// (BrowseScreen::Category aliases this.)
enum class BrowseCategory {
    Popular = 0,
    NowPlaying = 1,
    TopRated = 2,
    Upcoming = 3,
    Filter = 4,
    ForYou = 5,
    Search = 6,
    Library = 7,
    Queue = 8,
    Settings = 9,
};

inline constexpr int kBrowseNumContentCategories = 6;  // Popular..ForYou

inline bool browse_is_nav_chip(BrowseCategory cat) {
    return static_cast<int>(cat) >= kBrowseNumContentCategories;
}

// The Marquee strip, left-to-right — the single source of truth for both
// handle_input() and render().
inline constexpr BrowseCategory kBrowseVisibleTabs[] = {
    BrowseCategory::Popular,
    BrowseCategory::TopRated,
    BrowseCategory::ForYou,
    BrowseCategory::Search,
    BrowseCategory::Library,
    BrowseCategory::Queue,
    BrowseCategory::Settings,
};
inline constexpr int kBrowseNumVisibleTabs =
    static_cast<int>(sizeof(kBrowseVisibleTabs) / sizeof(kBrowseVisibleTabs[0]));

inline const char* browse_category_label(BrowseCategory cat) {
    switch (cat) {
        case BrowseCategory::Popular:    return "Popular";
        case BrowseCategory::NowPlaying: return "Now Playing";
        case BrowseCategory::TopRated:   return "Top Rated";
        case BrowseCategory::Upcoming:   return "Upcoming";
        case BrowseCategory::Filter:     return "Filter";
        case BrowseCategory::ForYou:     return "For You";
        case BrowseCategory::Search:     return "Search";
        case BrowseCategory::Library:    return "Library";
        case BrowseCategory::Queue:      return "Queue";
        case BrowseCategory::Settings:   return "Settings";
    }
    return "";
}

// Where the category sits in the visible strip; 0 (Popular) when it is not
// a Marquee tab (legacy persistence, NowPlaying from before v1.6.x).
inline int browse_strip_position(BrowseCategory cat) {
    for (int i = 0; i < kBrowseNumVisibleTabs; ++i) {
        if (kBrowseVisibleTabs[i] == cat) return i;
    }
    return 0;
}

// The screen a transition-only tab hands the dispatcher; nullopt for a
// content tab.
inline std::optional<Screen> browse_nav_screen(BrowseCategory cat) {
    switch (cat) {
        case BrowseCategory::Library:  return Screen::Library;
        case BrowseCategory::Search:   return Screen::Search;
        case BrowseCategory::Queue:    return Screen::Queue;
        case BrowseCategory::Settings: return Screen::MovieSettings;
        default:                       return std::nullopt;
    }
}

// BTN1 (dir -1) / BTN3 (dir +1): stop at the ends (no wrap); a transition
// tab returns its screen; a content tab becomes the active category.
struct BrowseTabStep {
    enum class Kind { None, Navigate, Activate };
    Kind kind = Kind::None;
    Screen screen = Screen::Browse;                      // Navigate
    BrowseCategory category = BrowseCategory::Popular;   // Activate
    int strip_pos = 0;                                   // Activate
};

inline BrowseTabStep browse_tab_step(int strip_pos, int dir) {
    BrowseTabStep s;
    if (dir < 0 ? strip_pos == 0 : strip_pos >= kBrowseNumVisibleTabs - 1) return s;
    const int new_pos = strip_pos + (dir < 0 ? -1 : 1);
    const BrowseCategory new_cat = kBrowseVisibleTabs[new_pos];
    if (const auto screen = browse_nav_screen(new_cat)) {
        s.kind = BrowseTabStep::Kind::Navigate;
        s.screen = *screen;
        return s;
    }
    s.kind = BrowseTabStep::Kind::Activate;
    s.category = new_cat;
    s.strip_pos = new_pos;
    return s;
}

// Popular / Top Rated — the chart tabs with a filter panel.
inline bool is_chart_category(BrowseCategory cat) {
    return cat == BrowseCategory::Popular || cat == BrowseCategory::TopRated;
}

// The filter slot a chart tab reads/writes (callers have already checked
// is_chart_category; anything else maps to TopRated, as the inline ternaries
// this replaces did).
inline FilterTabKind chart_filter_tab(BrowseCategory cat) {
    return cat == BrowseCategory::Popular ? FilterTabKind::Popular
                                          : FilterTabKind::TopRated;
}

// BTN4: which overlay a tab opens — the full filter panel on the charts, the
// SHUFFLE-only row on For You, nothing elsewhere.
inline std::optional<FilterTabKind> overlay_tab_for(BrowseCategory cat) {
    if (cat == BrowseCategory::Popular) return FilterTabKind::Popular;
    if (cat == BrowseCategory::TopRated) return FilterTabKind::TopRated;
    if (cat == BrowseCategory::ForYou) return FilterTabKind::ForYou;
    return std::nullopt;
}

// The curated-endpoint shuffle ceiling for a chart tab.
inline int chart_shuffle_max_base(BrowseCategory cat) {
    return cat == BrowseCategory::Popular ? kShuffleMaxBasePopular
                                          : kShuffleMaxBaseTopRated;
}

// ---------- Poster grid navigation ----------

// ROTATE: one cell at a time, row-major, clamped into the loaded grid.
// Callers skip an empty grid.
inline int grid_cursor_after_rotate(int cursor, int delta, int count) {
    return std::clamp(cursor + delta, 0, count - 1);
}

// ROTATE_VERTICAL: one row at a time, keeping the column; a target past the
// short last row lands on the last poster. Callers skip an empty grid.
inline int grid_cursor_after_vertical(int cursor, int delta, int count, int cols) {
    const int row = cursor / cols;
    const int col = cursor % cols;
    const int max_row = (count - 1) / cols;
    const int new_row = std::clamp(row + delta, 0, max_row);
    const int new_idx = new_row * cols + col;
    return new_idx < count ? new_idx : count - 1;
}

// Page-based scroll: the first row of the cursor's two-row page. It snaps
// when the cursor crosses a page boundary, not on every row step.
inline int grid_page_first_row(int cursor, int cols) {
    return ((cursor / cols) / 2) * 2;
}

// SELECT: the KIND picks the destination — movie and TV tmdb id spaces
// overlap completely, so the id alone must never choose it.
inline Screen browse_select_destination(MediaKind kind) {
    return kind == MediaKind::Tv ? Screen::SeriesDetail : Screen::Detail;
}

// ---------- TMDB pages ----------

// One drained page. Stale = a later generation superseded it at DRAIN time;
// KeepStaleGrid = a background revalidate failed or came back empty (the old
// grid survives, pagination freezes); ShuffleFallback = a shuffled base page
// was genuinely empty (rerun as a plain page-1 load); Apply = merge it.
enum class PageDrain { Stale, KeepStaleGrid, ShuffleFallback, Apply };

inline PageDrain decide_page_drain(bool gen_current, bool is_revalidate, bool ok,
                                   bool empty, int page, int window_base) {
    if (!gen_current) return PageDrain::Stale;
    if (is_revalidate && (!ok || empty)) return PageDrain::KeepStaleGrid;
    if (!is_revalidate && page == window_base && window_base != 1 && ok && empty)
        return PageDrain::ShuffleFallback;
    return PageDrain::Apply;
}

// MOVIE titles the owner already has are hidden from the chart grids (the
// Library tab is the way back). TV titles are NOT hidden — they wear the IN
// LIBRARY badge, because the badged chart poster is a route back to the
// series page. Kind-aware, so a movie is never hidden because a show shares
// its id.
inline bool is_hidden_owned_movie(const TmdbSearchHit& m,
                                  const std::unordered_set<MediaRef>& library_refs) {
    return m.kind == MediaKind::Movie && library_refs.count(media_ref_of(m)) > 0;
}

struct PageMergeCounts {
    size_t added = 0;
    size_t dups = 0;
    size_t owned = 0;
};

// Merge one page's hits into the grid. replace (the window-base page)
// clears the grid and the seen-set first. Exact-duplicate refs are dropped;
// owned movies are hidden but still enter `loaded` so a later append page
// cannot resurrect them. Consumes `hits`.
inline PageMergeCounts merge_page_hits(std::vector<TmdbSearchHit>& grid,
                                       std::unordered_set<MediaRef>& loaded,
                                       const std::unordered_set<MediaRef>& library_refs,
                                       std::vector<TmdbSearchHit>& hits, bool replace) {
    PageMergeCounts c;
    if (replace) {
        grid.clear();
        loaded.clear();
        grid.reserve(hits.size());
    } else {
        grid.reserve(grid.size() + hits.size());
    }
    for (auto& m : hits) {
        if (!loaded.insert(media_ref_of(m)).second) {
            ++c.dups;
        } else if (is_hidden_owned_movie(m, library_refs)) {
            ++c.owned;
        } else {
            grid.push_back(std::move(m));
            ++c.added;
        }
    }
    return c;
}

// Timestamp rule (spec 1a/1b) for a merged window-base page: fresh when it
// landed ok and non-empty; a FAILED base page clears the timestamp (so the
// next enter() revalidates instead of reading the previous grid's stamp as
// fresh); ok-but-empty leaves it alone.
enum class ChartStamp { Fresh, Clear, Keep };

inline ChartStamp chart_stamp_after_base_page(bool ok, bool grid_empty) {
    if (ok && !grid_empty) return ChartStamp::Fresh;
    if (!ok) return ChartStamp::Clear;
    return ChartStamp::Keep;
}

// Remove every hidden-owned movie from a grid (the retro-hide sweep after a
// library refresh); returns how many went.
inline size_t erase_owned_movies(std::vector<TmdbSearchHit>& grid,
                                 const std::unordered_set<MediaRef>& library_refs) {
    const size_t before = grid.size();
    grid.erase(std::remove_if(grid.begin(), grid.end(),
                              [&library_refs](const TmdbSearchHit& m) {
                                  return is_hidden_owned_movie(m, library_refs);
                              }),
               grid.end());
    return before - grid.size();
}

// When to fetch the next page of the window. Never while a fetch is in
// flight, in the initial Loading state, at end-of-list, on a nav chip or For
// You, or past the base-relative window; then either the second page right
// after the base lands (a full second screen ready) or a scroll-driven page
// once the cursor reaches the last loaded row.
enum class LoadMore { None, PrefetchSecond, ScrollDriven };

struct LoadMoreInputs {
    bool fetching_more = false;
    bool loading = false;
    bool more_available = true;
    bool nav_chip = false;
    bool for_you = false;
    int next_page = 1;
    int window_base = 1;
    int max_loaded_pages = 5;
    int grid_count = 0;
    int cursor = 0;
    int cols = 9;
};

inline LoadMore decide_load_more(const LoadMoreInputs& in) {
    if (in.fetching_more || in.loading) return LoadMore::None;
    if (!in.more_available) return LoadMore::None;
    if (in.nav_chip) return LoadMore::None;
    if (in.for_you) return LoadMore::None;
    if (in.next_page > window_last_page(in.window_base, in.max_loaded_pages))
        return LoadMore::None;
    const int rows_loaded =
        in.grid_count == 0 ? 0 : (in.grid_count + in.cols - 1) / in.cols;
    const int cursor_row = in.cursor / in.cols;
    const bool prefetch_second = (in.next_page == in.window_base + 1);
    const bool near_end = (rows_loaded > 0) && (cursor_row >= rows_loaded - 1);
    if (!prefetch_second && !near_end) return LoadMore::None;
    return prefetch_second ? LoadMore::PrefetchSecond : LoadMore::ScrollDriven;
}

// enter(): first visit loads; For You revalidates a stale cache behind the
// grid (or runs its entry rule when it never loaded); a chart grid past its
// 6 h TTL revalidates (stale-while-revalidate). Nav chips and Filter never.
enum class BrowseEnter { LoadCategory, ForYouRevalidate, ForYouActivate,
                         ChartRevalidate, None };

inline BrowseEnter decide_browse_enter(bool loaded, BrowseCategory cat,
                                       bool foryou_stale, bool foryou_empty,
                                       bool chart_stale) {
    if (!loaded) return BrowseEnter::LoadCategory;
    if (cat == BrowseCategory::ForYou) {
        if (foryou_stale && !foryou_empty) return BrowseEnter::ForYouRevalidate;
        if (foryou_empty) return BrowseEnter::ForYouActivate;
        return BrowseEnter::None;
    }
    if (!browse_is_nav_chip(cat) && cat != BrowseCategory::Filter && chart_stale)
        return BrowseEnter::ChartRevalidate;
    return BrowseEnter::None;
}

// ---------- Library refresh ----------

struct MovieLibraryRefs {
    std::unordered_set<MediaRef> movie_refs;
    std::unordered_set<MediaRef> downloading_refs;
};

// Radarr's half: every library movie with a tmdb id, and the movies the
// download queue cross-references (radarr_id → tmdb_id) for DOWNLOADING
// badges.
inline MovieLibraryRefs collect_movie_library_refs(const std::vector<Movie>& library,
                                                   const std::vector<QueueItem>& queue) {
    MovieLibraryRefs out;
    std::unordered_map<int, int> radarr_to_tmdb;
    for (const auto& m : library) {
        if (m.tmdb_id > 0) {
            out.movie_refs.insert(MediaRef{MediaKind::Movie, m.tmdb_id});
            radarr_to_tmdb[m.radarr_id] = m.tmdb_id;
        }
    }
    for (const auto& qi : queue) {
        auto it = radarr_to_tmdb.find(qi.movie_id);
        if (it != radarr_to_tmdb.end()) {
            out.downloading_refs.insert(MediaRef{MediaKind::Movie, it->second});
        }
    }
    return out;
}

// Sonarr's half: every library series with a tmdb id.
inline std::unordered_set<MediaRef> collect_tv_library_refs(const std::vector<Series>& library) {
    std::unordered_set<MediaRef> out;
    for (const auto& srs : library) {
        if (srs.tmdb_id > 0) out.insert(MediaRef{MediaKind::Tv, srs.tmdb_id});
    }
    return out;
}

// Allocation-free "is there any ref of this kind?" — seed_pool(refs,
// kind).empty() without the per-frame vector.
inline bool has_ref_of_kind(const std::unordered_set<MediaRef>& refs, MediaKind kind) {
    return std::any_of(refs.begin(), refs.end(),
                       [kind](const MediaRef& ref) { return ref.kind == kind; });
}

// ---------- Text ----------

// The non-blocking service line over a chart/For You grid when the ACTIVE
// mode's library did not answer. Suppressed under LibraryUnavailable (that
// state already names the same service for the same fact, with no posters to
// badge). A box that never had Sonarr set up is told so, not "offline".
inline const char* browse_service_warning(bool lib_refresh_done_once, bool lib_ok,
                                          BrowseGridState state, bool tv_mode,
                                          bool sonarr_configured) {
    if (!lib_refresh_done_once || lib_ok ||
        state == BrowseGridState::LibraryUnavailable) {
        return nullptr;
    }
    if (tv_mode) {
        return sonarr_configured
            ? "Sonarr offline \xE2\x80\x94 in-library hiding may be stale"
            : "TV library not set up \xE2\x80\x94 in-library hiding "
              "unavailable";
    }
    return "Radarr offline \xE2\x80\x94 in-library hiding may be stale";
}

// The color of browse_grid_state_message's line: the three failure states
// warn, the rest are dim.
inline MbTone browse_grid_state_tone(BrowseGridState state) {
    switch (state) {
        case BrowseGridState::LibraryUnavailable:
        case BrowseGridState::RecommendationsFailed:
        case BrowseGridState::NoApiKey:
            return MbTone::Highlight2;
        case BrowseGridState::Grid:
        case BrowseGridState::Loading:
        case BrowseGridState::EmptyLibrary:
        case BrowseGridState::EmptyCategory:
            break;
    }
    return MbTone::Dim;
}

inline std::vector<chrome::Hint> browse_footer_hints(bool filter_available,
                                                     bool shuffle_only) {
    return {
        {chrome::HintIcon::Btn1Yellow,  "Tab \xE2\x86\x90"},
        {chrome::HintIcon::Btn2Red,     "Exit"},
        {chrome::HintIcon::Btn3Green,   "Tab \xE2\x86\x92"},
        {chrome::HintIcon::Btn4Black,
         filter_available ? "Filters" : (shuffle_only ? "Mode/Shuffle" : "\xE2\x80\x94")},
        {chrome::HintIcon::RotaryNav,   "Browse"},
        {chrome::HintIcon::RotaryPress, "Detail"},
    };
}

// The two-line title under a poster. fits: the whole title is line 1.
// Otherwise `split` is the byte index of the space ending the longest
// word-prefix that fits (line 1 = title[0, split), line 2 = the rest after
// the space, truncated by the caller); npos = no word boundary fits, so line
// 1 is the truncated whole title. `measure(text)` returns the rendered width.
struct PosterTitleSplit {
    bool fits = false;
    size_t split = std::string::npos;
};

template <class Measure>
PosterTitleSplit split_poster_title(const std::string& title, float max_w,
                                    Measure&& measure) {
    PosterTitleSplit s;
    if (measure(title) <= max_w) {
        s.fits = true;
        return s;
    }
    size_t pos = 0;
    while (true) {
        size_t next = title.find(' ', pos + 1);
        if (next == std::string::npos) break;
        if (measure(title.substr(0, next)) > max_w) break;
        s.split = next;
        pos = next;
    }
    return s;
}

}  // namespace media_browser::ui
