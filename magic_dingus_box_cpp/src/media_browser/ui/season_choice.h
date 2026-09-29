// Which season a TV download should target, and the chooser that lets the
// viewer change it. Pure, header-only, Mac-tested (test_season_choice.cpp).
//
// Separate from series_detail_logic.h because it needs watch_map, and
// episode_logic.h (which defines watch_map) already includes
// series_detail_logic.h — putting this there would be an include cycle.
#pragma once

#include <algorithm>
#include <optional>
#include <vector>

#include "media_browser/ui/episode_logic.h"
#include "media_browser/ui/series_detail_logic.h"

namespace media_browser::ui {

// Seasons a download can target: season >= 1 with nothing on disk and
// nothing in flight (SeasonState::None). Eligibility is deliberately NOT the
// monitored flag: a monitored season whose search found nothing is still a
// legitimate target — re-issuing it re-runs the search. Ascending.
inline std::vector<int> eligible_seasons(const std::vector<SeasonRow>& rows) {
    std::vector<int> out;
    for (const auto& r : rows) {
        if (r.season_number >= 1 && r.state == SeasonState::None)
            out.push_back(r.season_number);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The season the primary button proposes. frontier = the highest season the
// viewer has touched: any watched episode or resumable position, any file on
// disk, or a download in flight. Suggest the first eligible season past it;
// when none is past it (re-watching, or filling a gap) the lowest eligible.
inline std::optional<int> suggested_season(const std::vector<SeasonRow>& rows,
                                           const watch_map& watch) {
    const std::vector<int> eligible = eligible_seasons(rows);
    if (eligible.empty()) return std::nullopt;
    int frontier = 0;
    for (const auto& kv : watch) {
        if (kv.first.season < 1) continue;
        if (kv.second.watched ||
            is_resumable_position(kv.second.position_s, kv.second.duration_s))
            frontier = std::max(frontier, kv.first.season);
    }
    for (const auto& r : rows) {
        if (r.season_number < 1) continue;
        if (r.episode_file_count > 0 || r.state == SeasonState::Downloading)
            frontier = std::max(frontier, r.season_number);
    }
    for (int s : eligible) {
        if (s > frontier) return s;
    }
    return eligible.front();
}

}  // namespace media_browser::ui
