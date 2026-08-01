#pragma once

#include <chrono>
#include <cstdint>

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

}  // namespace media_browser::ui
