#pragma once

#include <unordered_set>
#include <vector>

#include "media_browser/tmdb_client.h"

// Pure merge/rank for the For You tab (spec 1c step 4). Renderer-free so
// test_media_browser_unit can assert on it.

namespace media_browser::ui {

inline constexpr int kForYouCap = 100;

// Merge per-seed TMDB recommendation lists into one ranked grid:
//   score  = number of DISTINCT seeds recommending the title
//   tie 1  = minimum index the title holds across all seed lists
//   tie 2  = ascending tmdb_id
// Drops exclude-set members (the library) and non-positive ids; duplicate
// rows within one seed list count once. Result capped at exactly `cap`.
// Inputs arrive already family-safe-trimmed by TmdbClient's list parser.
std::vector<TmdbSearchHit> merge_recommendations(
    const std::vector<std::vector<TmdbSearchHit>>& per_seed,
    const std::unordered_set<int>& exclude,
    int cap = kForYouCap);

}  // namespace media_browser::ui
