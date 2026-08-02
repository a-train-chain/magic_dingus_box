#pragma once

#include <chrono>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "media_browser/media_ref.h"

// Pure decision helpers for BrowseScreen's TTL / shuffle / For You entry
// logic (spec: 2026-07-31-marquee-personalization-and-tv-design.md, Phase 1).
// Header-only and Renderer-free so test_media_browser_unit can assert on
// them — same rationale as mb_ui_utils / library_view.

namespace media_browser::ui {

inline constexpr int kBrowseTtlHours       = 6;
inline constexpr int kShuffleMaxBasePopular  = 26;  // window base+4 never passes page 30
inline constexpr int kShuffleMaxBaseTopRated = 21;  // window base+4 never passes page 25

// True when the grid loaded at `last` should be refreshed at `now`.
// A default-constructed time_point means "never loaded" → stale.
inline bool tmdb_grid_stale(std::chrono::steady_clock::time_point last,
                            std::chrono::steady_clock::time_point now) {
    if (last == std::chrono::steady_clock::time_point{}) return true;
    return (now - last) > std::chrono::hours(kBrowseTtlHours);
}

// Last page of the base-relative pagination window.
inline int window_last_page(int base, int max_loaded_pages = 5) {
    return base + max_loaded_pages - 1;
}

// Highest legal shuffle base for a /discover result set. total_pages <= 0
// means no cached value for this filter signature — clamp optimistically to
// the Popular ceiling and rely on the empty-page → page-1 fallback.
inline int discover_max_base(int total_pages, int max_loaded_pages = 5) {
    if (total_pages <= 0) return kShuffleMaxBasePopular;
    int mb = total_pages - max_loaded_pages + 1;
    if (mb < 1) mb = 1;
    if (mb > kShuffleMaxBasePopular) mb = kShuffleMaxBasePopular;
    return mb;
}

// Uniform draw over 1..max_base excluding current_base whenever at least two
// candidates exist; a collapsed range degrades to a plain page-1 refetch
// (spec 1b: the shuffle draw must never deadlock re-rolling).
inline int pick_shuffle_base(int current_base, int max_base, uint32_t rand_value) {
    if (max_base <= 1) return 1;
    const bool exclude = (current_base >= 1 && current_base <= max_base);
    if (!exclude) {
        return 1 + static_cast<int>(rand_value % static_cast<uint32_t>(max_base));
    }
    // Draw index over the max_base-1 non-current candidates, then skip past
    // current_base so the mapping stays uniform.
    int base = 1 + static_cast<int>(rand_value % static_cast<uint32_t>(max_base - 1));
    if (base >= current_base) ++base;
    return base;
}

// Spec 1c "entry rule (three-way)" — what to do when the For You tab
// activates. WaitForLibrary covers both "refresh in flight" and "no refresh
// ever ran" (the caller kicks one off if none is in flight).
enum class ForYouEntry { UseCache, Sample, WaitForLibrary, ServiceUnavailable, EmptyLibrary };

inline ForYouEntry decide_foryou_entry(bool has_cached_list,
                                       bool refresh_done_once,
                                       bool library_fetch_ok,
                                       bool library_empty) {
    if (has_cached_list) return ForYouEntry::UseCache;
    if (!refresh_done_once) return ForYouEntry::WaitForLibrary;
    if (!library_fetch_ok) return ForYouEntry::ServiceUnavailable;
    if (library_empty) return ForYouEntry::EmptyLibrary;
    return ForYouEntry::Sample;
}

// Replace every entry of `kind` in `dst` with the contents of `fresh`,
// leaving entries of the other kind untouched. The library cache is a
// single MediaRef set fed by two independent services (Radarr for movies,
// Sonarr for TV); a refresh where only one answered must not wipe the
// other's contribution. Runs entirely on the render thread inside one
// apply_library_pending() call, so the momentary mid-erase state is never
// observable by another reader.
// Unsupported: passing the same set as both `dst` and `fresh` (i.e.
// replace_refs_of_kind(s, k, s)) — `fresh` would be erased-from-under-you
// as `dst` is mutated, since it's the same underlying set. No caller does
// this today; noted because this helper is about to become the TV task's
// API surface.
inline void replace_refs_of_kind(std::unordered_set<MediaRef>& dst,
                                 MediaKind kind,
                                 const std::unordered_set<MediaRef>& fresh) {
    for (auto it = dst.begin(); it != dst.end();) {
        if (it->kind == kind) it = dst.erase(it);
        else ++it;
    }
    dst.insert(fresh.begin(), fresh.end());
}

// The Marquee header title, carrying the Movies/TV mode indicator.
//
// The mode marker is NOT in the tab strip: with the shipped ZenDots metrics
// the 7-chip strip is 915 px wide and starts at x=305 on the 1280-wide
// logical canvas, and draw_screen_header right-aligns it with no overflow
// guard — suffixing the three content-tab labels with " · TV" grows the
// strip to 1080 px and runs it 63 px into the title. The title has 102 px of
// slack, and "Marquee TV" consumes 57 of it, leaving +45.
//
// Re-measure with tools/measure_strip_fit.cpp before changing either string;
// the unit test below pins the literal, not the fit.
inline const char* marquee_title_for_mode(bool tv_mode) {
    return tv_mode ? "Marquee TV" : "Marquee";
}

// The bare tmdb ids of one kind, for For You's seed sample. The library cache
// mixes kinds, and TMDB's recommendation endpoints are kind-specific
// (/movie/{id}/recommendations vs /tv/{id}/recommendations) — seeding a TV
// sample with a movie id would return a plausible-looking, entirely wrong
// grid, since the ids collide rather than 404.
inline std::vector<int> seed_pool(const std::unordered_set<MediaRef>& refs,
                                  MediaKind kind) {
    std::vector<int> out;
    out.reserve(refs.size());
    for (const auto& ref : refs) {
        if (ref.kind == kind) out.push_back(ref.id);
    }
    return out;
}

// What Browse's content area should show. Resolved from flags alone so the
// decision is testable without a Renderer — and, more importantly, so render()
// has exactly ONE place that can conclude "nothing to draw". It used to make
// this call across five early `return`s, and the filter overlay is drawn after
// all of them: any return that skipped it left an OPEN modal invisible while
// handle_input kept feeding it the user's rotary presses.
enum class BrowseGridState {
    Grid,                   // draw posters
    Loading,
    LibraryUnavailable,     // For You only — its seed source is down
    RecommendationsFailed,  // For You only — every seed failed
    EmptyLibrary,           // For You only — nothing to seed from
    NoApiKey,
    EmptyCategory,
};

struct BrowseStateInputs {
    bool is_foryou = false;
    bool grid_empty = true;
    bool loading = false;
    bool lib_refresh_done_once = false;
    bool lib_fetch_ok = false;      // for the ACTIVE mode
    bool seeds_empty = true;        // for the ACTIVE mode
    bool foryou_failed = false;
    bool has_api_key = true;
};

// Ordering is the shipped ordering, preserved exactly:
//   - For You's library/failure/empty checks come first, and only the first
//     and third are guarded on an empty grid (cache-first: a loaded grid
//     survives a transient library outage);
//   - a non-empty grid then always wins;
//   - then Loading, then the no-key message, then the generic empty state.
inline BrowseGridState decide_browse_grid_state(const BrowseStateInputs& in) {
    if (in.is_foryou) {
        if (in.grid_empty && in.lib_refresh_done_once && !in.lib_fetch_ok) {
            return BrowseGridState::LibraryUnavailable;
        }
        if (in.foryou_failed) return BrowseGridState::RecommendationsFailed;
        if (!in.loading && in.grid_empty && in.lib_refresh_done_once &&
            in.seeds_empty) {
            return BrowseGridState::EmptyLibrary;
        }
    }
    if (!in.grid_empty) return BrowseGridState::Grid;
    if (in.loading) return BrowseGridState::Loading;
    if (!in.has_api_key) return BrowseGridState::NoApiKey;
    return BrowseGridState::EmptyCategory;
}

}  // namespace media_browser::ui
