// Which season a TV download should target, and the chooser that lets the
// viewer change it. Pure, header-only, Mac-tested (test_season_choice.cpp).
//
// Separate from series_detail_logic.h because it needs watch_map, and
// episode_logic.h (which defines watch_map) already includes
// series_detail_logic.h — putting this there would be an include cycle.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
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

// The primary button's two-step flow: SELECT opens it on the suggested
// season, rotate steps through candidates, SELECT confirms, BTN4 or any focus
// move cancels. Render-thread only, like whole_armed_/remove_pending_.
struct SeasonChooser {
    bool choosing = false;
    std::vector<int> candidates;
    int index = 0;

    void open(std::vector<int> eligible, int start) {
        candidates = std::move(eligible);
        choosing = !candidates.empty();
        index = 0;
        if (!choosing) return;
        snap_to(start);
    }
    void step(int delta) {
        if (!choosing) return;
        index = std::clamp(index + delta, 0,
                           static_cast<int>(candidates.size()) - 1);
    }
    std::optional<int> confirm() {
        if (!choosing) return std::nullopt;
        const int season = candidates[static_cast<size_t>(index)];
        cancel();
        return season;
    }
    void cancel() {
        choosing = false;
        candidates.clear();
        index = 0;
    }
    void revalidate(const std::vector<int>& eligible) {
        if (!choosing) return;
        const int was = candidates[static_cast<size_t>(index)];
        candidates = eligible;
        if (candidates.empty()) { cancel(); return; }
        snap_to(was);
    }
    std::optional<int> current() const {
        if (!choosing) return std::nullopt;
        return candidates[static_cast<size_t>(index)];
    }

private:
    // First candidate >= season, else the last one.
    void snap_to(int season) {
        index = static_cast<int>(candidates.size()) - 1;
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (candidates[i] >= season) { index = static_cast<int>(i); break; }
        }
    }
};

// "‹ Season 5 · ~22 GB (est) ›". GiB, same unit as whole_series_label; the
// "(est)" suffix whenever the runtime behind the estimate was assumed.
inline std::string chooser_label(int season, int64_t estimate_bytes,
                                 bool estimated) {
    return std::string("\xE2\x80\xB9 Season ") + std::to_string(season) +
           " \xC2\xB7 ~" +
           std::to_string(estimate_bytes / (1024LL * 1024 * 1024)) + " GB" +
           (estimated ? " (est)" : "") + " \xE2\x80\xBA";
}

}  // namespace media_browser::ui
