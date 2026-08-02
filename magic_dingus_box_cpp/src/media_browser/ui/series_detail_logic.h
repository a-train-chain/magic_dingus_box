#pragma once

// Pure decision helpers for the TV series detail screen (spec:
// 2026-07-31-marquee-personalization-and-tv-design.md, Phase 2 "Series
// detail screen" + "Download granularity" + "Disk safety"). Header-only
// and Renderer-free so test_media_browser_unit can assert on them — same
// rationale as browse_logic.h / mb_ui_utils / library_view.
//
// The one structural rule everything here serves: the season list ALWAYS
// builds from TMDB's seasons as the base, with Sonarr statistics overlaid
// when present. AddSeriesResult with settled==false carries an EMPTY
// seasons vector by contract (sonarr_client.h) — a UI that keyed its rows
// on Sonarr seasons would render "0 seasons" for every unsettled add.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "media_browser/sonarr/sonarr_types.h"
#include "media_browser/tmdb_client.h"

namespace media_browser::ui {

// ---------- Season rows ----------

enum class SeasonState { None, Downloading, Partial, Complete };

struct SeasonRow {
    int season_number = 0;
    int episode_count = 0;       // Sonarr statistics when overlaid, else TMDB
    int episode_file_count = 0;  // Sonarr only; 0 pre-add
    bool monitored = false;      // Sonarr only; false pre-add
    SeasonState state = SeasonState::None;
};

// Downloading wins over everything (live queue activity is the freshest
// signal). Complete requires a KNOWN episode count — files present against
// an unknown total is Partial, never Complete.
inline SeasonState decide_season_state(int episode_count, int episode_file_count,
                                       bool downloading) {
    if (downloading) return SeasonState::Downloading;
    if (episode_count > 0 && episode_file_count >= episode_count)
        return SeasonState::Complete;
    if (episode_file_count > 0) return SeasonState::Partial;
    return SeasonState::None;
}

// TMDB seasons are the base; a Sonarr Series (nullable — pre-add, or a
// failed resolve) overlays monitored + statistics by season_number, and
// contributes seasons TMDB lacks. Season 0 (Specials) is excluded from
// BOTH sources: it is not part of "whole series" intent and would skew
// the disk estimate. Output is sorted by season_number.
inline std::vector<SeasonRow> merge_season_rows(
        const std::vector<TmdbTvSeason>& tmdb_seasons,
        const Series* sonarr,
        const std::unordered_set<int>& downloading_seasons) {
    std::vector<SeasonRow> rows;
    rows.reserve(tmdb_seasons.size());
    for (const auto& ts : tmdb_seasons) {
        if (ts.season_number == 0) continue;
        SeasonRow r;
        r.season_number = ts.season_number;
        r.episode_count = ts.episode_count;
        rows.push_back(r);
    }
    if (sonarr != nullptr) {
        for (const auto& ss : sonarr->seasons) {
            if (ss.season_number == 0) continue;
            auto it = std::find_if(rows.begin(), rows.end(),
                                   [&ss](const SeasonRow& r) {
                                       return r.season_number == ss.season_number;
                                   });
            if (it == rows.end()) {
                SeasonRow r;
                r.season_number = ss.season_number;
                rows.push_back(r);
                it = rows.end() - 1;
            }
            it->monitored = ss.monitored;
            it->episode_file_count = ss.episode_file_count;
            if (ss.episode_count > 0) it->episode_count = ss.episode_count;
        }
    }
    std::sort(rows.begin(), rows.end(),
              [](const SeasonRow& a, const SeasonRow& b) {
                  return a.season_number < b.season_number;
              });
    for (auto& r : rows) {
        r.state = decide_season_state(
            r.episode_count, r.episode_file_count,
            downloading_seasons.count(r.season_number) > 0);
    }
    return rows;
}

// "Download next season" target: the first unmonitored season in ascending
// order (specials never appear in rows). nullopt = everything monitored,
// the action hides.
inline std::optional<int> next_unmonitored_season(const std::vector<SeasonRow>& rows) {
    for (const auto& r : rows) {
        if (!r.monitored) return r.season_number;
    }
    return std::nullopt;
}

// ---------- Disk safety ----------

// Estimate covers episodes NOT yet on disk: sum(max(0, count - files)) ×
// per-episode runtime × preferred MB/min. Defensive fallbacks are load-
// bearing: runtime<=0 or rate<=0 would make the estimate 0 and silently
// ALLOW a whole-series add straight past the blocking preflight — 45 min
// and 70 MB/min are the fixture-typical values.
//
// runtime<=0 is ALSO the ordinary pre-add case (Series::runtime_minutes
// only exists once Sonarr has the record), so a pre-add estimate is an
// ASSUMPTION, not a measurement. Every label built from this value must
// say so — see the "(est)" suffix in the whole-series confirm.
inline int64_t estimate_remaining_bytes(const std::vector<SeasonRow>& rows,
                                        int runtime_minutes, double mb_per_min) {
    const int rt = runtime_minutes > 0 ? runtime_minutes : 45;
    const double rate = mb_per_min > 0.0 ? mb_per_min : 70.0;
    int64_t missing = 0;
    for (const auto& r : rows) {
        missing += std::max(0, r.episode_count - r.episode_file_count);
    }
    return static_cast<int64_t>(
        std::llround(static_cast<double>(missing) * rt * rate * 1024.0 * 1024.0));
}

// Highest preferred rate in the 1080p family (what season packs actually
// land as under the retuned profiles), falling back to 720p, then to the
// fixture's 1080p preferred (70). Rows with preferred<=0 (Sonarr encodes
// "unlimited" as null) are skipped. Values come from Sonarr's live
// quality definitions — NOT hardcoded, because the operator retuned them
// once already (25/40 → 40/70, 2026-07-26).
inline double pick_preferred_mb_per_min(const std::vector<QualityDefinition>& defs) {
    double best_1080 = 0.0, best_720 = 0.0;
    for (const auto& d : defs) {
        if (d.preferred_mb_per_min <= 0.0) continue;
        if (d.title.find("1080p") != std::string::npos)
            best_1080 = std::max(best_1080, d.preferred_mb_per_min);
        else if (d.title.find("720p") != std::string::npos)
            best_720 = std::max(best_720, d.preferred_mb_per_min);
    }
    if (best_1080 > 0.0) return best_1080;
    if (best_720 > 0.0) return best_720;
    return 70.0;
}

// The codebase's FIRST blocking preflight (spec, Phase 2 "Disk safety"):
// whole-series adds Block when the estimate exceeds free space minus a
// 20 GiB floor.
//
// WarnOnly is for a FAILED reading only (nullopt — nothing answered), per
// the movie flow's philosophy: warn, never wedge. A reading OF ZERO is not
// a failure, it is the full disk — the exact case this preflight exists to
// stop — so it Blocks. With estimate > 0 and a 20 GiB floor the general
// comparison already yields Block for free==0; the explicit branch below
// makes that intent unmistakable rather than incidental.
enum class DiskVerdict { Allow, Block, WarnOnly };

inline constexpr int64_t kDiskFloorBytes = 20LL * 1024 * 1024 * 1024;

inline DiskVerdict whole_series_verdict(int64_t estimate_bytes,
                                        std::optional<int64_t> free_bytes) {
    if (!free_bytes.has_value()) return DiskVerdict::WarnOnly;
    if (*free_bytes <= 0) return DiskVerdict::Block;
    if (estimate_bytes > *free_bytes - kDiskFloorBytes) return DiskVerdict::Block;
    return DiskVerdict::Allow;
}

// ---------- Screen state ----------

enum class SeriesDetailState {
    Loading,            // either fetch still in flight
    TmdbError,          // TMDB detail failed — nothing to render
    NotConfigured,      // no SONARR_API_KEY: read-only page, no actions
    SonarrUnreachable,  // configured but not answering: read-only + banner
    NotInLibrary,       // actions: Add Season 1 / Whole series…
    InLibrary,          // actions: Download next season / Whole series… / Remove
};

struct SeriesDetailInputs {
    bool tmdb_done = false;
    bool tmdb_ok = false;
    bool sonarr_configured = false;
    bool sonarr_done = false;
    bool sonarr_ok = false;
    bool in_library = false;
};

// Precedence: TMDB first (without it there is no page at all), then the
// configured gate (an unconfigured box must read as "not set up", never
// as an outage — see 63f9046's rationale), then Sonarr's answer.
inline SeriesDetailState decide_series_detail_state(const SeriesDetailInputs& in) {
    if (!in.tmdb_done) return SeriesDetailState::Loading;
    if (!in.tmdb_ok) return SeriesDetailState::TmdbError;
    if (!in.sonarr_configured) return SeriesDetailState::NotConfigured;
    if (!in.sonarr_done) return SeriesDetailState::Loading;
    if (!in.sonarr_ok) return SeriesDetailState::SonarrUnreachable;
    return in.in_library ? SeriesDetailState::InLibrary
                         : SeriesDetailState::NotInLibrary;
}

// Copy for the non-interactive states; nullptr = render the page body.
// NotConfigured is the SAME literal BrowseScreen ships so the two screens
// never disagree about the same box. No default: — -Wswitch catches a new
// enumerator here.
inline const char* series_detail_state_message(SeriesDetailState s) {
    switch (s) {
        case SeriesDetailState::Loading:           return "Loading...";
        case SeriesDetailState::TmdbError:
            return "Couldn't load series info \xE2\x80\x94 check network";
        case SeriesDetailState::NotConfigured:
            return "TV library not set up on this box";
        case SeriesDetailState::SonarrUnreachable: return "Sonarr service offline";
        case SeriesDetailState::NotInLibrary:      return nullptr;
        case SeriesDetailState::InLibrary:         return nullptr;
    }
    return nullptr;  // unreachable; keeps -Wreturn-type quiet without a default:
}

}  // namespace media_browser::ui
