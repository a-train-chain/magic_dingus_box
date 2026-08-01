#pragma once

#include <unordered_set>
#include <vector>

#include "media_browser/media_ref.h"
#include "media_browser/tmdb_client.h"

// Pure merge/rank for the For You tab (spec 1c step 4). Renderer-free so
// test_media_browser_unit can assert on it.

namespace media_browser::ui {

inline constexpr int kForYouCap = 100;

// Merge per-seed TMDB recommendation lists into one ranked grid:
//   score  = number of DISTINCT seeds recommending the title
//   tie 1  = minimum index the title holds across all seed lists
//   tie 2  = ascending tmdb_id
//   tie 3  = kind (Movie before Tv) — keeps the ordering TOTAL, since a
//            movie and a show can legitimately share a tmdb id
// Drops exclude-set members (the library, as MediaRefs) and non-positive
// ids; duplicate rows within one seed list count once. Result capped at
// exactly `cap`. Inputs arrive already family-safe-trimmed by TmdbClient's
// list parsers.
//
// Keyed on MediaRef, never on a bare tmdb id: TMDB's movie and TV id spaces
// overlap, so an int key would collapse an unrelated movie and show into
// one entry and would let a TV library entry hide a movie of the same id.
std::vector<TmdbSearchHit> merge_recommendations(
    const std::vector<std::vector<TmdbSearchHit>>& per_seed,
    const std::unordered_set<MediaRef>& exclude,
    int cap = kForYouCap);

}  // namespace media_browser::ui
