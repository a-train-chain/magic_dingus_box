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

// ---------- Remove ----------

// Queue rows -> the exact ids to cancel for one series, deduped by
// download_id (a season pack's siblings 404 by design after the first
// cancel). Rows with an empty download_id fall back to q.title as the
// dedupe key (documented identical across a pack's rows) rather than
// fanning out per-row.
//
// The dedupe is what makes the remove flow's abort guard trustworthy.
// Sonarr's queue is per EPISODE while cancel_queue_item acts on the WHOLE
// download, so cancelling row 1 of a 13-episode pack makes rows 2-13 404 BY
// DESIGN (sonarr_client.h). Counting those 404s as failures aborted the
// remove AFTER the torrent was already gone, leaving the series record and
// its files behind under a "NOT removed" toast that was itself a lie.
//
// A row with NEITHER a download_id nor a title has no key at all, so it is
// taken as-is — the committed loop's behaviour for an unkeyed row. Collapsing
// several of those onto one empty key would silently skip real cancels.
inline std::vector<int> cancel_ids_for_series(
        const std::vector<SonarrQueueItem>& queue, int sonarr_series_id) {
    std::vector<int> ids;
    std::unordered_set<std::string> seen;
    for (const auto& q : queue) {
        if (q.series_id != sonarr_series_id) continue;
        const std::string& key = q.download_id.empty() ? q.title : q.download_id;
        if (key.empty()) {
            ids.push_back(q.id);  // nothing to dedupe on
            continue;
        }
        if (seen.insert(key).second) ids.push_back(q.id);
    }
    return ids;
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

// ---------- Action row ----------

// The action row's vocabulary. It lives HERE rather than nested inside
// SeriesDetailScreen because the decision that BUILDS the row is pure (see
// decide_action_row below) and its table tests are Mac-side: the enum has to
// be reachable from a TU that cannot include a Renderer-bound header. Tasks
// 6-7's dispatch cases name these same enumerators.
// PlayNextUp is deliberately FIRST (Task 6's dispatch contract pins it with a
// test): it is the row's head whenever it exists, and keeping enumerator
// order aligned with on-screen order keeps the focus fallback loop honest.
enum class Action { PlayNextUp, AddSeason1, NextSeason, WholeSeries, Remove,
                    ConfirmRemove };

struct ActionButton {
    Action action;
    std::string label;
};

// Remove and ConfirmRemove are ONE button in two states: every focus
// comparison canonicalizes, so arming the confirm cannot move focus off it.
inline Action canonical_action(Action a) {
    return a == Action::ConfirmRemove ? Action::Remove : a;
}

// The whole-series button's label, armed and unarmed.
//
// Binary GB (GiB) on purpose — it is the same unit the free-space readings
// arrive in, so the two numbers in the Block toast compare like with like.
// "(est)" is the honesty: pre-add the per-episode runtime is
// estimate_remaining_bytes' 45-minute ASSUMPTION, because Sonarr has no
// record to read runtime_minutes from yet.
inline std::string whole_series_label(bool armed, int64_t estimate_bytes) {
    if (!armed) return "Whole series\xE2\x80\xA6";
    return "Confirm ~" +
           std::to_string(estimate_bytes / (1024LL * 1024 * 1024)) +
           " GB (est)";
}

// Everything the action row is decided from. All of it is render-thread
// state on the screen; none of it is Renderer-shaped, which is the point.
struct ActionRowInputs {
    SeriesDetailState state = SeriesDetailState::Loading;
    // False for the window where Sonarr holds the record but has never
    // refreshed it — EVERY season reads unmonitored there, so the add
    // controls must not be offered.
    bool series_settled = true;
    // next_unmonitored_season(rows) — nullopt when everything is monitored.
    std::optional<int> next_unmonitored;
    bool remove_pending = false;
    bool whole_armed = false;
    int64_t whole_estimate_bytes = 0;
    // Task 6: "keep watching" affordance. has_next_up is EVIDENCE-based — the
    // screen sets it only when ui::next_up (current==nullptr form) found an
    // unwatched episode WITH a file, so the button can never promise an
    // episode that cannot start. next_up_is_first = nothing watched yet
    // (no watched flag and no resumable position anywhere in the series'
    // watch map), which flips the label from "Continue" to "Start watching".
    bool has_next_up = false;
    int next_up_season = 0;
    int next_up_episode = 0;
    bool next_up_is_first = false;
    // The action under the focus ring BEFORE this rebuild, if any. Focus is
    // preserved by ACTION IDENTITY, never by index.
    std::optional<Action> prev_focus_action;
    // True when the PREVIOUS row was Remove/ConfirmRemove and nothing else —
    // i.e. prev_focus_action was not a choice the user made, it was the only
    // place the layout could put the ring. A focus the layout FORCED is not a
    // user choice, so it must not be preserved across the next rebuild.
    bool prev_row_remove_only = false;
};

struct ActionRow {
    std::vector<ActionButton> buttons;
    int focus = 0;
};

// The whole of the action row's algebra: which buttons exist, what they are
// labelled, and where focus lands.
//
// Focus is preserved by ACTION IDENTITY, never by index. A rebuild can
// insert, drop or relabel rows (an add turns "Add Season 1" into "Download
// Season 2" + "Remove"), and forcing focus to 0 meant the whole-series
// confirm was never the focused button — pressing SELECT inside the 4 s
// window fired "Add Season 1" instead. Deterministically.
//
// The no-match fallback biases AWAY from destructive actions: after an
// unsettled add the row is [Remove] alone for ~9 s, and a satisfied user
// tapping again should not find the delete button pre-focused unless it is
// genuinely the only thing on offer.
inline ActionRow decide_action_row(const ActionRowInputs& in) {
    ActionRow out;
    if (in.state == SeriesDetailState::NotInLibrary) {
        out.buttons.push_back({Action::AddSeason1, "Add Season 1"});
        out.buttons.push_back(
            {Action::WholeSeries,
             whole_series_label(in.whole_armed, in.whole_estimate_bytes)});
    } else if (in.state == SeriesDetailState::InLibrary) {
        // PlayNextUp leads whenever an episode is genuinely playable. It is
        // NOT gated on series_settled: the settled gate exists because the
        // ADD controls derive a garbage target while every season reads
        // unmonitored — playing a file already on disk has no such hazard,
        // and hiding "keep watching" for ~9 s after an add would read as a
        // regression to anyone mid-binge.
        if (in.has_next_up) {
            out.buttons.push_back(
                {Action::PlayNextUp,
                 (in.next_up_is_first ? std::string("Start watching S")
                                      : std::string("Continue S")) +
                     std::to_string(in.next_up_season) + "E" +
                     std::to_string(in.next_up_episode)});
        }
        // While the record is unsettled EVERY season reads unmonitored, so
        // next_unmonitored would answer "1" one second after we added
        // season 1 and the primary button would read "Download Season 1".
        // Offer Remove only until the poll settles it; the meta line says
        // "syncing…" so the missing controls read as pending, not broken.
        if (in.series_settled && in.next_unmonitored.has_value()) {
            out.buttons.push_back(
                {Action::NextSeason,
                 "Download Season " + std::to_string(*in.next_unmonitored)});
            out.buttons.push_back(
                {Action::WholeSeries,
                 whole_series_label(in.whole_armed, in.whole_estimate_bytes)});
        }
        out.buttons.push_back(
            in.remove_pending
                ? ActionButton{Action::ConfirmRemove, "Confirm Remove"}
                : ActionButton{Action::Remove, "Remove"});
    }
    // NOTE: there is deliberately NO "Working…" swap while a mutation is in
    // flight. Replacing the row wholesale destroyed focus identity and was
    // the root of the wrong-mutation bug; the screen dims the real row
    // instead, and SELECT is already gated on the in-flight flag.
    out.focus = 0;
    for (size_t i = 0; i < out.buttons.size(); ++i) {
        if (canonical_action(out.buttons[i].action) != Action::Remove) {
            out.focus = static_cast<int>(i);
            break;
        }
    }
    // A focus the LAYOUT forced is not a user choice. When the previous row
    // was [Remove] alone — the ~9 s unsettled-add window — the ring sat on
    // Remove because it was the only button, not because anyone chose it.
    // Preserving that identity when the poll settles and re-expands the row to
    // [NextSeason, WholeSeries, Remove] leaves the delete button focused at the
    // exact moment a waiting user taps SELECT: that arms ConfirmRemove, and a
    // second tap inside 2 s deletes the series WITH ITS FILES. Skipping the
    // identity loop hands the decision to the bias-away-from-Remove fallback
    // above, which is the correct answer for a forced focus.
    if (in.prev_focus_action.has_value() && !in.prev_row_remove_only) {
        for (size_t i = 0; i < out.buttons.size(); ++i) {
            if (canonical_action(out.buttons[i].action) ==
                canonical_action(*in.prev_focus_action)) {
                out.focus = static_cast<int>(i);
                break;
            }
        }
    }
    if (out.focus >= static_cast<int>(out.buttons.size())) out.focus = 0;
    return out;
}

}  // namespace media_browser::ui
