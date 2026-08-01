#pragma once

#include <cstdint>
#include <vector>

#include "app/app_state.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/ui/mb_filter_overlay.h"

// Renderer-free filter logic for the Marquee content tabs, with a mode axis.
// Lives outside browse_screen.cpp / mb_filter_overlay.cpp precisely so it can
// be compiled into test_media_browser_unit: "TV filters never touch movie
// storage" and "the two genre id spaces stay separate" are the two rules that
// most need to be assertions rather than comments.

namespace media_browser::ui {

using MbMode     = ::app::AppState::DisplaySettings::MbMode;
using MbChartTab = ::app::AppState::DisplaySettings::MbChartTab;

// TV titles accumulate far fewer votes than films, so the movie gates (200 /
// 300, baked in to match what /movie/popular and /movie/top_rated imply)
// would empty a filtered TV grid.
inline constexpr int kTvVoteCountPopular  = 50;
inline constexpr int kTvVoteCountTopRated = 100;

// TMDB genre ids for `mode`, in bit-position order: index i is bit i of
// FilterState::genre_mask. The two catalogs are DIFFERENT id spaces (18
// movie genres, 16 TV genres) — never index one with the other's mask.
const std::vector<int>& filter_genre_ids(MbMode mode);
int filter_genre_count(MbMode mode);
// "All" for mask 0; the genre named by the lowest set bit otherwise; "?" when
// that bit is past the end of `mode`'s catalog.
const char* filter_genre_display(MbMode mode, uint32_t mask);

// Five RUNTIME row labels for `mode`, index = MbRuntime value. TV's runtime
// is PER EPISODE, so its bands are 30/45/60-minute, not feature-length.
const char* const* filter_runtime_labels(MbMode mode);

// Value shown on the overlay's MODE row.
const char* mode_row_value_label(MbMode mode);

// Per-(mode, tab) persisted filter state. ForYou keeps none in either mode:
// read returns a default FilterState, write is a no-op. Unlike the two-way
// helpers these replaced, there is no aliasing hazard — ForYou has no slot to
// alias onto.
FilterState read_filter_state(const ::app::AppState::DisplaySettings& s,
                              MbMode mode, FilterTabKind tab);
void write_filter_state(::app::AppState::DisplaySettings& s,
                        MbMode mode, FilterTabKind tab, const FilterState& fs);

// One MODE-row toggle, as a pure transaction on DisplaySettings:
//   1. `staged` (the overlay's working_ edits) is written into the OUTGOING
//      mode's slot for `tab` — those edits were made against that mode, and
//      filing them under the incoming one would corrupt filters the user
//      never touched;
//   2. s.mb_mode becomes `incoming`;
//   3. the incoming mode's persisted state for `tab` is returned, for the
//      overlay to re-stage as both working_ and its open()-time snapshot.
// ForYou is a no-op on step 1 and yields a default FilterState on step 3.
// The caller saves settings.json ONCE afterwards.
FilterState apply_mode_toggle(::app::AppState::DisplaySettings& s,
                              MbMode outgoing, MbMode incoming,
                              FilterTabKind tab, const FilterState& staged);

// True when `fs` differs from `tab`'s defaults — i.e. when the tab must route
// through /discover instead of its curated chart endpoint. Mode-independent:
// the default sort is a property of the tab, not the media kind.
bool any_filter_active(const FilterState& fs, FilterTabKind tab);

::media_browser::DiscoverFilter build_discover_filter(const FilterState& fs,
                                                      FilterTabKind tab);
::media_browser::TvDiscoverFilter build_tv_discover_filter(const FilterState& fs,
                                                           FilterTabKind tab);

}  // namespace media_browser::ui
