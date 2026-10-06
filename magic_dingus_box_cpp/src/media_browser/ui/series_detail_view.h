#pragma once

// The TV series detail screen's VIEW-side decisions: what each season and
// episode row says (and in which tone), how the two paged lists clamp, the
// footer's hints, the rotary/paging/SELECT input mapping, and the small
// lookups the screen used to inline. Header-only and Renderer-free so
// test_media_browser_unit can assert on them
// (tests/media_browser/test_series_detail_view.cpp) — the same split as
// series_detail_logic.h, which holds the page's STATE decisions (season
// merge, action row, disk verdict, deferred start). SeriesDetailScreen keeps
// only the drawing calls and the thread plumbing.
//
// Everything here was moved out of series_detail_screen.cpp verbatim in
// behaviour: same strings, same clamps, same precedence.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "media_browser/sonarr/sonarr_types.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/ui/episode_logic.h"
#include "media_browser/ui/mb_chrome.h"
#include "media_browser/ui/mb_tone.h"
#include "media_browser/ui/mb_ui_utils.h"  // join_genres
#include "media_browser/ui/series_detail_logic.h"

namespace media_browser::ui {

// ---------- Lookups ----------

// The merged row for one season, or nullptr.
inline const SeasonRow* find_season_row(const std::vector<SeasonRow>& rows,
                                        int season_number) {
    for (const auto& row : rows) {
        if (row.season_number == season_number) return &row;
    }
    return nullptr;
}

// Indices into the full-series episode vector for one season, in fetch order.
inline std::vector<int> season_episode_indices(const std::vector<EpisodeInfo>& episodes,
                                               int season) {
    std::vector<int> idxs;
    for (size_t i = 0; i < episodes.size(); ++i) {
        if (episodes[i].season_number == season) idxs.push_back(static_cast<int>(i));
    }
    return idxs;
}

// Every season row Sonarr does not monitor yet — the whole-series press-2
// PUT list, in row (ascending season) order.
inline std::vector<int> unmonitored_seasons(const std::vector<SeasonRow>& rows) {
    std::vector<int> out;
    for (const auto& row : rows) {
        if (!row.monitored) out.push_back(row.season_number);
    }
    return out;
}

// estimate_remaining_bytes for ONE season (the season chooser's "~N GB").
// A season with no row estimates as zero missing episodes.
inline int64_t season_estimate_bytes(const std::vector<SeasonRow>& rows, int season,
                                     int runtime_minutes, double mb_per_min) {
    std::vector<SeasonRow> one;
    for (const auto& r : rows)
        if (r.season_number == season) one.push_back(r);
    return estimate_remaining_bytes(one, runtime_minutes, mb_per_min);
}

// An episode's runtime, falling back to the series' per-episode figure
// (sonarr_types.h documents runtime 0 as real for specials/unknown).
inline int episode_runtime_minutes(const EpisodeInfo& ep, int series_runtime_minutes) {
    return ep.runtime_minutes > 0 ? ep.runtime_minutes : series_runtime_minutes;
}

// "Start watching" vs "Continue": true when nothing in the series' watch map
// is watched or holds a resumable position.
inline bool next_up_is_first(const watch_map& watch) {
    for (const auto& kv : watch) {
        if (kv.second.watched ||
            is_resumable_position(kv.second.position_s, kv.second.duration_s)) {
            return false;
        }
    }
    return true;
}

// Resume offset for one episode: its watch row's position when resumable
// (>= 60 s, short of the watched threshold), else 0 — the movie path's rule.
inline double episode_resume_position(const watch_map& watch, int season, int episode) {
    const auto it = watch.find(WatchKey{season, episode});
    if (it != watch.end() &&
        is_resumable_position(it->second.position_s, it->second.duration_s)) {
        return it->second.position_s;
    }
    return 0.0;
}

// ---------- Action row ----------

// The primary (two-press, season-chooser) button.
inline bool is_primary_season_action(Action a) {
    return a == Action::AddSeason || a == Action::NextSeason;
}

// True when every button in the row is Remove/ConfirmRemove (canonicalized —
// they are one button in two states): a focus the LAYOUT forced, which
// decide_action_row must not preserve. False for an empty row.
inline bool row_is_remove_only(const std::vector<ActionButton>& buttons) {
    return !buttons.empty() &&
           std::all_of(buttons.begin(), buttons.end(), [](const ActionButton& b) {
               return canonical_action(b.action) == Action::Remove;
           });
}

// Button color: destructive (Remove, its confirm, the armed whole-series
// confirm) is Warn; the whole-series button and an OPEN season chooser read
// as "in a mode" (Action); everything else is Ok.
inline chrome::ButtonKind series_button_kind(Action a, bool whole_armed,
                                             bool chooser_open) {
    if (a == Action::Remove || a == Action::ConfirmRemove ||
        (a == Action::WholeSeries && whole_armed)) {
        return chrome::ButtonKind::Warn;
    }
    if (a == Action::WholeSeries || (chooser_open && is_primary_season_action(a))) {
        return chrome::ButtonKind::Action;
    }
    return chrome::ButtonKind::Ok;
}

// ---------- Header / rows text ----------

// "2008 · 5 seasons · 62 episodes · Ended [· syncing…]". The syncing suffix
// is the honest label for an in-library record Sonarr has never refreshed:
// the rows below are TMDB's, not Sonarr's.
inline std::string series_meta_line(const TmdbTvDetail& d, bool in_library,
                                    bool series_settled) {
    std::string meta = std::to_string(d.year);
    meta += " \xC2\xB7 " + std::to_string(d.number_of_seasons) + " season" +
            (d.number_of_seasons == 1 ? "" : "s");
    meta += " \xC2\xB7 " + std::to_string(d.number_of_episodes) + " episodes";
    if (!d.status.empty()) meta += " \xC2\xB7 " + d.status;
    if (in_library && !series_settled) meta += " \xC2\xB7 syncing\xE2\x80\xA6";
    return meta;
}

struct SeasonRowView {
    std::string label;   // "Season N"
    std::string counts;  // "files/count eps"
    std::string state;   // "monitored" / "—" / "downloading" / "partial" / "complete"
    MbTone state_tone = MbTone::Dim;
};

inline SeasonRowView season_row_view(const SeasonRow& row) {
    SeasonRowView v;
    v.label = "Season " + std::to_string(row.season_number);
    v.counts = std::to_string(row.episode_file_count) + "/" +
               std::to_string(row.episode_count) + " eps";
    switch (row.state) {
        case SeasonState::None:
            v.state = row.monitored ? "monitored" : "\xE2\x80\x94";
            break;
        case SeasonState::Downloading:
            v.state = "downloading";
            v.state_tone = MbTone::Highlight2;
            break;
        case SeasonState::Partial:
            v.state = "partial";
            v.state_tone = MbTone::Accent;
            break;
        case SeasonState::Complete:
            v.state = "complete";
            v.state_tone = MbTone::Highlight1;
            break;
    }
    return v;
}

// The episode row's state glyph: ✓ watched, "▶ <hms>" resumable, ·
// unwatched-with-file, nothing for a fileless row (its dim text IS its
// state).
enum class EpisodeGlyph { None, Watched, Resume, Unwatched };

struct EpisodeGlyphView {
    EpisodeGlyph kind = EpisodeGlyph::None;
    std::string text;
    MbTone tone = MbTone::Dim;
};

inline EpisodeGlyphView episode_glyph_view(const EpisodeInfo& ep, const watch_map& watch) {
    EpisodeGlyphView g;
    if (!ep.has_file) return g;
    const auto it = watch.find(WatchKey{ep.season_number, ep.episode_number});
    const bool watched =
        it != watch.end() &&
        (it->second.watched ||
         is_watched_position(it->second.position_s, it->second.duration_s));
    const bool resumable =
        it != watch.end() && !watched &&
        is_resumable_position(it->second.position_s, it->second.duration_s);
    if (watched) {
        g.kind = EpisodeGlyph::Watched;
        g.text = "\xE2\x9C\x93";
        g.tone = MbTone::Highlight1;
    } else if (resumable) {
        g.kind = EpisodeGlyph::Resume;
        g.text = "\xE2\x96\xB6 " + format_position_hms(it->second.position_s);
        g.tone = MbTone::Accent;
    } else {
        g.kind = EpisodeGlyph::Unwatched;
        g.text = "\xC2\xB7";
        g.tone = MbTone::Dim;
    }
    return g;
}

// "E<n> · <title>[ · <runtime>m][ · downloading]" — runtime falls back to
// the series' per-episode figure and is omitted when genuinely unknown; the
// downloading suffix rides on a fileless row of a season with a live
// download.
inline std::string episode_row_text(const EpisodeInfo& ep, int series_runtime_minutes,
                                    bool season_downloading) {
    std::string text =
        "E" + std::to_string(ep.episode_number) + " \xC2\xB7 " + ep.title;
    const int rt = episode_runtime_minutes(ep, series_runtime_minutes);
    if (rt > 0) text += " \xC2\xB7 " + std::to_string(rt) + "m";
    if (!ep.has_file && season_downloading) text += " \xC2\xB7 downloading";
    return text;
}

// ---------- Paged lists ----------

// One frame's page geometry for a list of `total` navigable rows with
// `per_page` rows of space. per_page may be 0 (CRT_NATIVE's 640x480 canvas
// draws no rows): that is ONE page, no overflow, and nothing is ever
// divided by it.
struct ListPaging {
    int page_count = 1;
    int page = 0;
    int first = 0;      // first row index drawn
    int last = 0;       // one past the last row index drawn
    bool overflow = false;
    int focus = -1;     // the (snapped/clamped) focus the screen must adopt
};

// The season list: page is the screen's own (BTN1/BTN3 move it), clamped
// into range; a season ring (focus >= 0) the page does not show snaps into
// it. focus -1 (ring on the action row) passes through.
inline ListPaging season_list_paging(int total_rows, int per_page, int page,
                                     int season_focus) {
    ListPaging p;
    p.page_count =
        per_page > 0 ? std::max(1, (total_rows + per_page - 1) / per_page) : 1;
    p.page = page;
    if (p.page >= p.page_count) p.page = p.page_count - 1;
    if (p.page < 0) p.page = 0;
    p.overflow = per_page > 0 && total_rows > per_page;
    p.first = p.page * per_page;
    p.last = std::min(total_rows, p.first + per_page);
    p.focus = season_focus;
    if (p.focus >= 0 && per_page > 0 && (p.focus < p.first || p.focus >= p.last)) {
        p.focus = std::min(p.last - 1, std::max(p.first, p.focus));
    }
    return p;
}

// The episode picker: focus is clamped into [0, nav_total) first and the
// page FOLLOWS it, so SELECT can only ever fire a visible row. nav_total
// counts the trailing delete row when present.
inline ListPaging episode_list_paging(int nav_total, int per_page, int focus) {
    ListPaging p;
    p.page_count =
        per_page > 0 ? std::max(1, (nav_total + per_page - 1) / per_page) : 1;
    p.focus = focus;
    if (p.focus >= nav_total) p.focus = nav_total - 1;
    if (p.focus < 0) p.focus = 0;
    p.page = per_page > 0 ? p.focus / per_page : 0;
    if (p.page >= p.page_count) p.page = p.page_count - 1;
    p.overflow = per_page > 0 && nav_total > per_page;
    p.first = p.page * per_page;
    p.last = per_page > 0 ? std::min(nav_total, p.first + per_page) : 0;
    return p;
}

// "Seasons 1–8 of 21 · [BTN1/BTN3]".
inline std::string season_page_indicator(int first, int last, int total) {
    return "Seasons " + std::to_string(first + 1) + "\xE2\x80\x93" +
           std::to_string(last) + " of " + std::to_string(total) +
           " \xC2\xB7 [BTN1/BTN3]";
}

// "Episodes 1–8 of 10 · [BTN1/BTN3]". Paging counts the delete row; the
// INDICATOR does not — it names episodes — so both ends clamp into the
// episode range, which keeps a last page holding only the delete row from
// reading "Episodes 11-10 of 10".
inline std::string episode_page_indicator(int first, int last, int total_episodes) {
    const int ind_first = std::min(first + 1, total_episodes);
    const int ind_last = std::min(last, total_episodes);
    return "Episodes " + std::to_string(ind_first) + "\xE2\x80\x93" +
           std::to_string(ind_last) + " of " + std::to_string(total_episodes) +
           " \xC2\xB7 [BTN1/BTN3]";
}

// ---------- Trailing "Delete Season N…" row ----------

// Whether the picker shows the delete row: only inside the drill-down, only
// on frames that actually paint episode rows (loading / outage / "No
// episodes" bail before any row is drawn — an armable row nobody can see is
// the invisible-affordance bug class), and then season_delete_row_exists on
// THIS season's merged row.
inline bool season_delete_row_present(bool in_episodes_region, bool episodes_done,
                                      bool episodes_ok, int season_episode_count,
                                      int season_file_count, bool season_downloading) {
    if (!in_episodes_region) return false;
    if (!episodes_done || !episodes_ok) return false;
    if (season_episode_count <= 0) return false;
    return season_delete_row_exists(season_file_count, season_downloading);
}

inline SeasonDeleteState season_delete_row_state(bool inflight, bool armed) {
    return inflight ? SeasonDeleteState::Removing
         : armed    ? SeasonDeleteState::Armed
                    : SeasonDeleteState::Idle;
}

// ---------- Input mapping ----------

// Rotary twist on the season page: ONE navigation chain — season rows
// top-to-bottom, then the action buttons left-to-right. season_focus == -1
// means the ring is on the button row (focus). Rows count only when the
// canvas draws them (per_page > 0); a stale season ring restarts from the
// button row. Clamp, never wrap. nullopt = nothing to navigate (the press is
// inert — no confirm is cancelled either).
struct SeasonChainFocus {
    int season_focus = -1;
    int focus = 0;
    int season_page = 0;
};

inline std::optional<SeasonChainFocus> rotate_season_chain(
        int season_focus, int focus, int season_page, int row_count,
        int season_per_page, int button_count, int delta) {
    const int n_rows = season_per_page > 0 ? row_count : 0;
    const int total = n_rows + button_count;
    if (total == 0) return std::nullopt;
    int pos = (season_focus >= 0 && season_focus < n_rows) ? season_focus
                                                           : n_rows + focus;
    pos = std::clamp(pos + delta, 0, total - 1);
    SeasonChainFocus out;
    out.season_focus = season_focus;
    out.focus = focus;
    out.season_page = season_page;
    if (pos < n_rows) {
        out.season_focus = pos;
        // The list page follows the ring so SELECT always targets a visible
        // row.
        if (season_per_page > 0) out.season_page = out.season_focus / season_per_page;
    } else {
        out.season_focus = -1;
        out.focus = pos - n_rows;
    }
    return out;
}

// BTN1 (dir -1) / BTN3 (dir +1) on the season page. A ring IN the list moves
// with the page (to the page's first row) so it never sits off-screen; on the
// action row the pages browse freely underneath.
struct SeasonPageStep {
    int season_page = 0;
    int season_focus = -1;
};

inline SeasonPageStep season_page_step(int season_page, int season_page_count,
                                       int season_focus, int season_per_page,
                                       int row_count, int dir) {
    SeasonPageStep s{season_page, season_focus};
    if (dir < 0) {
        if (s.season_page > 0) {
            --s.season_page;
            if (s.season_focus >= 0 && season_per_page > 0)
                s.season_focus = s.season_page * season_per_page;
        }
    } else {
        if (s.season_page + 1 < season_page_count) {
            ++s.season_page;
            if (s.season_focus >= 0 && season_per_page > 0)
                s.season_focus =
                    std::min(s.season_page * season_per_page, row_count - 1);
        }
    }
    return s;
}

// Rotary twist inside the episode picker: clamp over the nav chain
// (episodes + the delete row). n == 0 leaves focus alone.
inline int episode_focus_after_rotate(int focus, int delta, int nav_count) {
    if (nav_count == 0) return focus;
    return std::clamp(focus + delta, 0, nav_count - 1);
}

// BTN1 / BTN3 inside the picker page by moving FOCUS a page at a time —
// page and focus never diverge. Inert with no rows or no drawn rows.
inline int episode_focus_after_page(int focus, int per_page, int nav_count, int dir) {
    if (nav_count <= 0 || per_page <= 0) return focus;
    return dir < 0 ? std::max(0, focus - per_page)
                   : std::min(nav_count - 1, focus + per_page);
}

// SELECT on a season row (the ring is in the list).
enum class SeasonRowSelect {
    RingToButtons,  // no rows are drawn (CRT_NATIVE): return the ring home
    Busy,           // a mutation is running — "Still finishing…"
    OpenPicker,     // files on disk or a live download: the episode drill-down
    StartDownload,  // nothing to see or stop: download this season
};

inline SeasonRowSelect decide_season_row_select(int season_per_page, bool mut_in_flight,
                                                const SeasonRow& row) {
    if (season_per_page <= 0) return SeasonRowSelect::RingToButtons;
    if (mut_in_flight) return SeasonRowSelect::Busy;
    return season_row_opens_picker(row) ? SeasonRowSelect::OpenPicker
                                        : SeasonRowSelect::StartDownload;
}

// SELECT inside the episode picker.
enum class EpisodeSelect {
    Ignore,             // no rows, or none drawn (CRT_NATIVE)
    Busy,               // a mutation (or this season's delete) is running
    FinishRemoveFirst,  // the whole-series Remove confirm is armed
    ArmDelete,          // delete row, press 1
    ConfirmDelete,      // delete row, press 2 inside the window
    Play,               // an episode row
};

struct EpisodeSelectInputs {
    int nav_count = 0;
    int per_page = 0;
    bool mut_in_flight = false;
    bool delete_focused = false;
    bool season_del_inflight = false;
    bool remove_pending = false;
    bool season_del_armed = false;
};

inline EpisodeSelect decide_episode_select(const EpisodeSelectInputs& in) {
    if (in.nav_count == 0) return EpisodeSelect::Ignore;
    if (in.per_page <= 0) return EpisodeSelect::Ignore;
    if (in.mut_in_flight) return EpisodeSelect::Busy;
    if (in.delete_focused) {
        // Inert while a season remove is already running or the whole-series
        // Remove confirm is armed — never two destructive confirms at once —
        // and never SILENTLY inert.
        if (in.season_del_inflight) return EpisodeSelect::Busy;
        if (in.remove_pending) return EpisodeSelect::FinishRemoveFirst;
        return in.season_del_armed ? EpisodeSelect::ConfirmDelete
                                   : EpisodeSelect::ArmDelete;
    }
    return EpisodeSelect::Play;
}

// The single-season download's render-thread guards, in order.
enum class SeasonStartGuard { Busy, NotInLibrary, Syncing, Go };

inline SeasonStartGuard decide_season_start_guard(bool mut_in_flight, bool in_library,
                                                  bool has_series_id,
                                                  bool series_settled) {
    if (mut_in_flight) return SeasonStartGuard::Busy;
    if (!in_library || !has_series_id) return SeasonStartGuard::NotInLibrary;
    if (!series_settled) return SeasonStartGuard::Syncing;
    return SeasonStartGuard::Go;
}

// ---------- Footer ----------

inline std::vector<chrome::Hint> series_footer_hints(bool in_episodes, bool overflow,
                                                     bool ep_overflow,
                                                     bool on_delete_row,
                                                     bool nothing_focusable) {
    return {
        {chrome::HintIcon::Btn1Yellow,
         in_episodes ? (ep_overflow ? "Episodes \xE2\x86\x90" : "\xE2\x80\x94")
                     : (overflow ? "Seasons \xE2\x86\x90" : "\xE2\x80\x94")},
        {chrome::HintIcon::Btn2Red, "Exit"},
        {chrome::HintIcon::Btn3Green,
         in_episodes ? (ep_overflow ? "Episodes \xE2\x86\x92" : "\xE2\x80\x94")
                     : (overflow ? "Seasons \xE2\x86\x92" : "\xE2\x80\x94")},
        {chrome::HintIcon::Btn4Black, in_episodes ? "Seasons" : "Back"},
        {chrome::HintIcon::RotaryNav,
         in_episodes ? "Choose" : (nothing_focusable ? "\xE2\x80\x94" : "Choose")},
        {chrome::HintIcon::RotaryPress,
         in_episodes ? (on_delete_row ? "Select" : "Play")
                     : (nothing_focusable ? "\xE2\x80\x94" : "Select")},
    };
}

// ---------- Season-end deferred start ----------

// The gate worker's mailbox (0 pending, 1 ready, 2 timed out) as the verdict
// decide_deferred_season_start reads.
inline DeferredGate deferred_gate_from_code(int g) {
    return g == 1 ? DeferredGate::Ready
         : g == 2 ? DeferredGate::TimedOut
                  : DeferredGate::Pending;
}

inline const char* season_update_didnt_apply_toast() {
    return "Season update didn't apply \xE2\x80\x94 try from this screen";
}

// Held past the announce delay: "<title>: starting Season N once services
// are back…".
inline std::string deferred_waiting_toast(const std::string& title, int season) {
    return title + ": starting Season " + std::to_string(season) +
           " once services are back\xE2\x80\xA6";
}

inline std::string deferred_services_down_toast(const std::string& title, int season) {
    return title + ": Sonarr didn't come back \xE2\x80\x94 Season " +
           std::to_string(season) + " not started; try from this screen";
}

// leave() with a start still held.
inline std::string deferred_not_started_toast(const std::string& title, int season) {
    return title + ": Season " + std::to_string(season) +
           " not started \xE2\x80\x94 open the show again to start it";
}

// A chosen-season (N > 1) add whose start was never run because the user
// left the page: the series IS in Sonarr now, with nothing monitored.
inline std::string added_start_dropped_toast(const std::string& start_title, int season) {
    return (start_title.empty() ? std::string("This series") : start_title) +
           ": added \xE2\x80\x94 open the show again to start Season " +
           std::to_string(season);
}

}  // namespace media_browser::ui
