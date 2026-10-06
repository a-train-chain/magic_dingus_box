#pragma once

// QueueScreen's decisions, Renderer-free and I/O-free so
// test_media_browser_unit can assert on them
// (tests/media_browser/test_queue_view.cpp): the refresh worker's pure
// passes (library-cache staleness, poster patching, the awaiting-release
// list, the qBittorrent live overlay, import-state reclassification, TV
// progress derivation), the cancel arm/confirm table, and every string and
// tone the page composes (row sub-line, percentage, refresh indicator,
// warning lines, empty state, awaiting section, footer). queue_groups.h
// keeps the TV collapse/enrich passes; QueueScreen keeps the clients, the
// threads and the drawing calls.
//
// Everything here was moved out of queue_screen.cpp verbatim in behaviour:
// same strings, same precedence, same arithmetic.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/radarr/radarr_types.h"
#include "media_browser/sonarr/sonarr_types.h"
#include "media_browser/ui/mb_chrome.h"
#include "media_browser/ui/mb_tone.h"
#include "media_browser/ui/mb_ui_utils.h"
#include "media_browser/ui/queue_groups.h"

namespace media_browser::ui {

// One TV download row: the collapsed group plus the live telemetry the qBit
// overlay (or the size fallback) fills. (QueueScreen::TvQueueRow aliases
// this.)
struct TvQueueRow {
    TvQueueGroup group;      // size/sizeleft/status updated by the overlay
    double progress = 0.0;
    int download_rate_bps = 0;
    int peers = 0;
    int seeds = 0;
    int eta_seconds = 0;
};

// ---------- Formatting ----------

// Human-readable rate: "1.2 MB/s", "480 KB/s", "0 B/s".
inline std::string queue_format_rate(int bps) {
    if (bps <= 0) return "0 B/s";
    double v = static_cast<double>(bps);
    const char* unit = "B/s";
    if (v >= 1024.0 * 1024.0) { v /= (1024.0 * 1024.0); unit = "MB/s"; }
    else if (v >= 1024.0)      { v /= 1024.0;            unit = "KB/s"; }
    char buf[32];
    if (v >= 100.0) snprintf(buf, sizeof(buf), "%.0f %s", v, unit);
    else            snprintf(buf, sizeof(buf), "%.1f %s", v, unit);
    return buf;
}

// ETA as "1h 23m", "12m 05s", "45s", or "--" when unknown.
inline std::string queue_format_eta(int eta_seconds) {
    if (eta_seconds <= 0) return "--";
    int s = eta_seconds;
    char buf[32];
    if (s >= 3600) {
        int h = s / 3600;
        int m = (s % 3600) / 60;
        snprintf(buf, sizeof(buf), "%dh %02dm", h, m);
    } else if (s >= 60) {
        int m = s / 60;
        int r = s % 60;
        snprintf(buf, sizeof(buf), "%dm %02ds", m, r);
    } else {
        snprintf(buf, sizeof(buf), "%ds", s);
    }
    return buf;
}

// "downloading" -> "Downloading"; empty -> "Unknown".
inline std::string queue_titlecase_state(const std::string& s) {
    if (s.empty()) return "Unknown";
    std::string out = s;
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

// Progress-bar fill tone by state: failures red, healthy green, everything
// indeterminate or idle (queued/paused/importing/unknown) gold.
inline MbTone queue_progress_tone(const std::string& state) {
    if (state == "failed" || state == "warning" || state == "stalled") {
        return MbTone::Highlight2;
    }
    if (state == "downloading" || state == "completed") {
        return MbTone::Highlight1;
    }
    return MbTone::Accent;
}

// "Breaking Bad — Season 1 (10 eps)". series_title arrives PRE-QUALIFIED
// (enrich_tv_groups bakes " — Season N" in; the fallback release name
// carries its own tokens), so only the episode count is added here.
inline std::string tv_row_title(const TvQueueGroup& g) {
    std::string base = g.series_title.empty() ? std::string("Untitled")
                                              : g.series_title;
    std::ostringstream ss;
    ss << base << " (" << g.episode_count << " ep"
       << (g.episode_count == 1 ? "" : "s") << ")";
    return ss.str();
}

// ---------- Refresh worker passes ----------

// Translate qBit's state vocabulary to the Radarr-style single-word names
// the renderer expects; an unmapped qBit state keeps `current`.
inline std::string arr_state_from_qbit(const std::string& qb,
                                       const std::string& current) {
    if (qb == "downloading") return "downloading";
    if (qb == "stalledDL")   return "stalled";
    if (qb == "metaDL" || qb == "queuedDL" || qb == "checkingDL"
        || qb == "allocating") {
        return "queued";
    }
    if (qb == "uploading" || qb == "pausedUP" || qb == "stalledUP"
        || qb == "queuedUP" || qb == "checkingUP" || qb == "forcedUP") {
        return "completed";
    }
    if (qb == "error" || qb == "missingFiles") return "failed";
    if (qb == "pausedDL") return "paused";
    return current;
}

// Both arrs emit the torrent hash in the tracker's casing (uppercase hex in
// practice); qBit normalizes to lowercase.
inline std::string lowercase_hash(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

// The 30 s movie-library snapshot is refetched when it aged out, or when a
// queue row names a movie the snapshot does not know (a just-added movie
// shows its poster and awaiting state right away).
inline bool movie_lib_cache_stale(bool aged_out, const std::vector<Movie>& cache,
                                  const std::vector<QueueItem>& queue) {
    if (aged_out) return true;
    std::unordered_set<int> cached_ids;
    cached_ids.reserve(cache.size());
    for (const auto& m : cache) cached_ids.insert(m.radarr_id);
    for (const auto& q : queue) {
        if (cached_ids.count(q.movie_id) == 0) return true;
    }
    return false;
}

// The TV mirror. series_id 0 can never resolve — treating it as "missing"
// would force a refetch every 1.5 s tick.
inline bool tv_lib_cache_stale(bool aged_out, const std::vector<Series>& cache,
                               const std::vector<TvQueueGroup>& groups) {
    if (aged_out) return true;
    std::unordered_set<int> cached_ids;
    cached_ids.reserve(cache.size());
    for (const auto& s : cache) cached_ids.insert(s.sonarr_id);
    for (const auto& g : groups) {
        if (g.series_id <= 0) continue;
        if (cached_ids.count(g.series_id) == 0) return true;
    }
    return false;
}

// sonarr_id -> {title, poster} for enrich_tv_groups.
inline std::unordered_map<int, SeriesRef> series_refs_by_id(const std::vector<Series>& library) {
    std::unordered_map<int, SeriesRef> by_id;
    by_id.reserve(library.size());
    for (const auto& s : library) {
        by_id.emplace(s.sonarr_id, SeriesRef{s.title, s.poster_url});
    }
    return by_id;
}

// One TvQueueRow per group, carrying Sonarr's per-row ETA for the row id
// the group kept (used only when the qBit overlay cannot supply a live one).
inline std::vector<TvQueueRow> tv_rows_from_groups(std::vector<TvQueueGroup>&& groups,
                                                   const std::vector<SonarrQueueItem>& rows) {
    std::unordered_map<int, int> eta_by_row_id;
    eta_by_row_id.reserve(rows.size());
    for (const auto& q : rows) eta_by_row_id.emplace(q.id, q.eta_seconds);
    std::vector<TvQueueRow> out;
    for (auto& g : groups) {
        TvQueueRow row;
        auto eta_it = eta_by_row_id.find(g.first_queue_id);
        row.eta_seconds = (eta_it == eta_by_row_id.end()) ? 0 : eta_it->second;
        row.group = std::move(g);
        out.push_back(std::move(row));
    }
    return out;
}

// Fill poster_url on queue rows that came back without one (Radarr's /queue
// carries no images) from the library snapshot, and return the set of movie
// ids with a queue row.
inline std::unordered_set<int> patch_queue_posters(std::vector<QueueItem>& queue,
                                                   const std::vector<Movie>& library) {
    std::unordered_map<int, std::string> id_to_poster;
    id_to_poster.reserve(library.size());
    for (const auto& m : library) {
        if (!m.poster_url.empty()) id_to_poster.emplace(m.radarr_id, m.poster_url);
    }
    std::unordered_set<int> active_movie_ids;
    for (auto& q : queue) {
        active_movie_ids.insert(q.movie_id);
        if (q.poster_url.empty()) {
            auto it = id_to_poster.find(q.movie_id);
            if (it != id_to_poster.end()) q.poster_url = it->second;
        }
    }
    return active_movie_ids;
}

// "Awaiting release": monitored library movies with no file that are not
// already in the active queue (copies — the snapshot is a cache).
inline std::vector<Movie> awaiting_release_movies(const std::vector<Movie>& library,
                                                  const std::unordered_set<int>& active_movie_ids) {
    std::vector<Movie> out;
    for (const auto& m : library) {
        if (!m.monitored) continue;
        if (m.has_file) continue;
        if (active_movie_ids.count(m.radarr_id) > 0) continue;
        out.push_back(m);
    }
    return out;
}

// qBit wired but answering with no torrents while there are rows that should
// be in it: unreachable or auth-desynced — the bars below are stale.
inline bool qbit_overlay_failed(bool qbit_map_empty, bool movie_queue_empty,
                                bool tv_empty) {
    return qbit_map_empty && !(movie_queue_empty && tv_empty);
}

// The live-data overlay: per-second telemetry straight from qBit, matched by
// lowercased hash, over Radarr's/Sonarr's 30-60 s cached snapshot. Identity
// fields (title, ids, poster) are kept.
inline void apply_qbit_overlay(std::vector<QueueItem>& queue, std::vector<TvQueueRow>& tv,
                               const std::unordered_map<std::string, QbitTorrent>& qbit_map) {
    for (auto& q : queue) {
        if (q.download_id.empty()) continue;
        auto it = qbit_map.find(lowercase_hash(q.download_id));
        if (it == qbit_map.end()) continue;
        const auto& qt = it->second;
        q.progress           = qt.progress;
        q.download_rate_bps  = qt.dlspeed;
        q.upload_rate_bps    = qt.upspeed;
        q.peers              = qt.num_leechs;
        q.seeds              = qt.num_seeds;
        q.size_bytes         = qt.size;
        q.sizeleft_bytes     = qt.size - qt.downloaded;
        q.eta_seconds        = qt.eta_seconds;
        q.state = arr_state_from_qbit(qt.state, q.state);
    }
    // Sonarr's downloadId IS the torrent hash, so a season pack's group
    // resolves to exactly the torrent qBit is moving — ONE live bar.
    for (auto& t : tv) {
        if (t.group.download_id.empty()) continue;
        auto it = qbit_map.find(lowercase_hash(t.group.download_id));
        if (it == qbit_map.end()) continue;
        const auto& qt = it->second;
        t.progress                = qt.progress;
        t.download_rate_bps       = qt.dlspeed;
        t.peers                   = qt.num_leechs;
        t.seeds                   = qt.num_seeds;
        t.eta_seconds             = qt.eta_seconds;
        t.group.size_bytes        = qt.size;
        t.group.sizeleft_bytes    = qt.size - qt.downloaded;
        t.group.status = arr_state_from_qbit(qt.state, t.group.status);
    }
}

// Progress for a TV row the overlay did not reach, from the group's MAXed
// (whole-pack) size/sizeleft. Rows that already have progress, or no size,
// are left alone.
inline void derive_tv_progress(std::vector<TvQueueRow>& tv) {
    for (auto& t : tv) {
        if (t.progress > 0.0) continue;
        if (t.group.size_bytes <= 0) continue;
        const int64_t left = std::max<int64_t>(0, t.group.sizeleft_bytes);
        t.progress = static_cast<double>(t.group.size_bytes - left)
                   / static_cast<double>(t.group.size_bytes);
    }
}

// A "completed" download still being imported reads "importing" (amber);
// one that cannot be imported reads "warning" (LibraryScreen's BAD RELEASE
// semantics). Path-independent: runs after, and regardless of, the overlay.
inline void reclassify_import_state(std::string& state, const std::string& tracked) {
    if (state != "completed") return;
    if (tracked == "importing" || tracked == "importPending") {
        state = "importing";
    } else if (tracked == "importBlocked" || tracked == "importFailed") {
        state = "warning";
    }
}

// ---------- Cursor / cancel ----------

inline int clamp_queue_cursor(int cursor, int row_count) {
    if (cursor >= row_count) cursor = std::max(0, row_count - 1);
    if (cursor < 0) cursor = 0;
    return cursor;
}

// Is the row a pending cancel was armed on still there? Searched in ITS
// section only — Radarr's and Sonarr's queue ids are independent sequences.
inline bool cancel_target_present(bool is_tv, int queue_id,
                                  const std::vector<QueueItem>& queue,
                                  const std::vector<TvQueueRow>& tv) {
    if (is_tv) {
        for (const auto& t : tv) {
            if (t.group.first_queue_id == queue_id) return true;
        }
        return false;
    }
    for (const auto& q : queue) {
        if (q.id == queue_id) return true;
    }
    return false;
}

// The armed cancel targets exactly this row (section-qualified).
inline bool cancel_armed_for(bool pending, bool pending_is_tv, int pending_id,
                             bool row_is_tv, int row_id) {
    return pending && pending_is_tv == row_is_tv && pending_id == row_id;
}

// SELECT on a row: the second press on the SAME armed row confirms; any
// other press (re-)arms this row.
enum class QueueSelect { Arm, Confirm };

inline QueueSelect decide_queue_select(bool pending, bool pending_is_tv, int pending_id,
                                       bool focused_is_tv, int focused_id) {
    return cancel_armed_for(pending, pending_is_tv, pending_id, focused_is_tv, focused_id)
        ? QueueSelect::Confirm
        : QueueSelect::Arm;
}

inline std::string cancel_failed_toast(const std::string& title) {
    return "Couldn't cancel " +
           (title.empty() ? std::string("the download") : title) +
           " \xE2\x80\x94 try again";
}

// Keep the cursor inside the visible window of `visible_rows`.
inline int queue_scroll_row(int cursor, int scroll_row, int visible_rows) {
    if (cursor < scroll_row) scroll_row = cursor;
    if (cursor >= scroll_row + visible_rows) scroll_row = cursor - visible_rows + 1;
    return scroll_row;
}

// ---------- Rows ----------

// One drawable queue row, projected from either a Radarr movie queue item or
// a grouped Sonarr TV download — the row painter reads ONLY this, so both
// sections share one code path.
struct QueueRowView {
    int id = 0;              // cancel key WITHIN its section (see is_tv)
    bool is_tv = false;
    std::string poster_url;  // empty draws the deterministic-tint placeholder
    std::string title;
    std::string state;
    double  progress = 0.0;
    int64_t size_bytes = 0;
    int64_t sizeleft_bytes = 0;
    int download_rate_bps = 0;
    int peers = 0;
    int seeds = 0;
    int eta_seconds = 0;
};

// Movie rows first (unchanged order), TV downloads after: one list, one
// cursor, one painter.
inline std::vector<QueueRowView> build_queue_rows(const std::vector<QueueItem>& queue,
                                                  const std::vector<TvQueueRow>& tv) {
    std::vector<QueueRowView> rows;
    rows.reserve(queue.size() + tv.size());
    for (const auto& q : queue) {
        QueueRowView v;
        v.id                = q.id;
        v.poster_url        = q.poster_url;
        v.title             = q.title;
        v.state             = q.state;
        v.progress          = q.progress;
        v.size_bytes        = q.size_bytes;
        v.sizeleft_bytes    = q.sizeleft_bytes;
        v.download_rate_bps = q.download_rate_bps;
        v.peers             = q.peers;
        v.seeds             = q.seeds;
        v.eta_seconds       = q.eta_seconds;
        rows.push_back(std::move(v));
    }
    for (const auto& t : tv) {
        QueueRowView v;
        v.id                = t.group.first_queue_id;
        v.is_tv             = true;
        v.poster_url        = t.group.poster_url;
        v.title             = tv_row_title(t.group);
        v.state             = t.group.status;
        v.progress          = t.progress;
        v.size_bytes        = t.group.size_bytes;
        v.sizeleft_bytes    = t.group.sizeleft_bytes;
        v.download_rate_bps = t.download_rate_bps;
        v.peers             = t.peers;
        v.seeds             = t.seeds;
        v.eta_seconds       = t.eta_seconds;
        rows.push_back(std::move(v));
    }
    return rows;
}

// A downloading row with bytes moving gets the pulsing activity dot.
inline bool queue_row_active(const QueueRowView& q) {
    return q.state == "downloading" && q.download_rate_bps > 0;
}

// "TV  •  Downloading  •  1.2 GB / 4.0 GB  •  1.2 MB/s  •  ETA 12m 05s
//  •  18 peers / 3 seeds" — most important first so truncation eats the
// least critical. "0 B" downloaded means "size known, nothing yet".
inline std::string queue_row_sub_line(const QueueRowView& q) {
    std::ostringstream ss;
    if (q.is_tv) ss << "TV  \xE2\x80\xA2  ";
    if (q.state == "importing") {
        ss << "Importing\xE2\x80\xA6";
    } else {
        ss << queue_titlecase_state(q.state);
    }
    if (q.size_bytes > 0) {
        int64_t left  = std::max<int64_t>(0, q.sizeleft_bytes);
        int64_t down  = std::max<int64_t>(0, q.size_bytes - left);
        ss << "  \xE2\x80\xA2  "
           << (down > 0 ? format_bytes(down) : std::string("0 B"))
           << " / " << format_bytes(q.size_bytes);
    }
    if (q.download_rate_bps > 0) {
        ss << "  \xE2\x80\xA2  " << queue_format_rate(q.download_rate_bps);
    }
    if (q.eta_seconds > 0) {
        ss << "  \xE2\x80\xA2  ETA " << queue_format_eta(q.eta_seconds);
    }
    if (q.peers > 0 || q.seeds > 0) {
        ss << "  \xE2\x80\xA2  " << q.peers
           << " peer" << (q.peers == 1 ? "" : "s")
           << " / " << q.seeds
           << " seed" << (q.seeds == 1 ? "" : "s");
    }
    return ss.str();
}

// The progress percentage: "100%" from 99.95, "0%" under 0.05, else one
// decimal ("3.8%") — whole numbers made multi-GB torrents appear to jump.
// `pct` is the clamped 0..1 fraction.
inline std::string queue_percent_text(double pct) {
    char pct_buf[16];
    double pct100 = pct * 100.0;
    if (pct100 >= 99.95) {
        snprintf(pct_buf, sizeof(pct_buf), "100%%");
    } else if (pct100 < 0.05) {
        snprintf(pct_buf, sizeof(pct_buf), "0%%");
    } else {
        snprintf(pct_buf, sizeof(pct_buf), "%.1f%%", pct100);
    }
    return pct_buf;
}

// ---------- Header lines ----------

// "1 monitored", else "N monitored — D downloading, A awaiting". A TV pack
// counts once (one entry per DOWNLOAD).
inline std::string queue_count_line(int downloading, size_t awaiting) {
    std::ostringstream cs;
    const int total = downloading + static_cast<int>(awaiting);
    if (total == 1) {
        cs << "1 monitored";
    } else {
        cs << total << " monitored \xE2\x80\x94 " << downloading
           << " downloading, " << awaiting << " awaiting";
    }
    return cs.str();
}

// The refresh indicator, color-coded by age so a stuck refresh is obvious:
// refreshing (green), > 45 s STALE (red), > 15 s slow (gold), else dim.
struct QueueLineView {
    std::string text;
    MbTone tone = MbTone::Dim;
    float alpha = 0.85f;
};

inline QueueLineView queue_refresh_indicator(bool refreshing, long long secs_since) {
    QueueLineView v;
    if (refreshing) {
        v.text = "refreshing...";
        v.tone = MbTone::Highlight1;
        v.alpha = 0.95f;
        return v;
    }
    char buf[64];
    if (secs_since > 45) {
        snprintf(buf, sizeof(buf), "STALE \xE2\x80\x94 last update %llds ago", secs_since);
        v.tone = MbTone::Highlight2;
        v.alpha = 0.95f;
    } else if (secs_since > 15) {
        snprintf(buf, sizeof(buf), "slow \xE2\x80\x94 updated %llds ago", secs_since);
        v.tone = MbTone::Accent;
        v.alpha = 0.95f;
    } else {
        snprintf(buf, sizeof(buf), "updated %llds ago", secs_since);
    }
    v.text = buf;
    return v;
}

inline std::string radarr_offline_line(const std::string& error) {
    return "Radarr offline \xE2\x80\x94 " + error;
}

// Only while TV rows are on screen — with none retained, the centered empty
// state carries the same message instead.
inline bool show_sonarr_offline_line(bool tv_unreachable, bool tv_empty) {
    return tv_unreachable && !tv_empty;
}

inline const char* sonarr_offline_line() {
    return "Sonarr offline \xE2\x80\x94 TV downloads may be out of date";
}

inline bool show_qbit_overlay_line(bool overlay_failed, bool movie_queue_empty,
                                   bool tv_empty) {
    return overlay_failed && !(movie_queue_empty && tv_empty);
}

inline const char* qbit_overlay_line() {
    return "Live data unavailable \xE2\x80\x94 progress shown is "
           "the last cached snapshot from Radarr/Sonarr";
}

// The empty queue's instruction line. When Sonarr did not answer, "add a
// movie" would be false reassurance — we do not know whether a season pack
// is running — so the warning takes its place.
inline QueueLineView queue_empty_hint(bool tv_unreachable) {
    if (tv_unreachable) return {sonarr_offline_line(), MbTone::Accent, 0.95f};
    return {"Add a movie from Browse or Search to start a download.", MbTone::Dim, 0.8f};
}

// ---------- Awaiting release ----------

inline bool awaiting_searching(const Movie& m, const ActiveSearches& s) {
    return s.global_search_running || s.movie_ids.count(m.radarr_id) > 0;
}

inline int count_searching(const std::vector<Movie>& awaiting, const ActiveSearches& s) {
    int n = 0;
    for (const auto& m : awaiting) {
        if (awaiting_searching(m, s)) ++n;
    }
    return n;
}

// The section sub-line: a live search leads (green) so a just-added movie
// doesn't read as "nothing happening for 30 minutes".
inline QueueLineView awaiting_sub_line(int searching_now) {
    if (searching_now > 0) {
        return {"Searching indexers now for " + std::to_string(searching_now) +
                    (searching_now == 1 ? " title" : " titles") +
                    " \xE2\x80\xA2  auto-downloads the moment a good "
                    "seeded release appears",
                MbTone::Highlight1, 0.9f};
    }
    return {"Radarr re-checks indexers every ~30 minutes and "
            "auto-downloads when a good seeded release appears "
            "\xE2\x80\x94 no action needed.",
            MbTone::Dim, 0.9f};
}

// "Title (Year)  •  Searching indexers now…" / "…  •  Monitored, awaiting
// release".
inline std::string awaiting_row_label(const Movie& m, bool searching) {
    std::ostringstream label;
    label << m.title;
    if (m.year > 0) label << " (" << m.year << ")";
    label << "  \xE2\x80\xA2  "
          << (searching ? "Searching indexers now\xE2\x80\xA6"
                        : "Monitored, awaiting release");
    return label.str();
}

// The footer. (Identical whether or not a cancel is armed — the armed state
// shows on the row itself.)
inline std::vector<chrome::Hint> queue_footer_hints() {
    return {
        {chrome::HintIcon::Btn1Yellow,  "Tab \xE2\x86\x90"},
        {chrome::HintIcon::Btn2Red,     "Exit"},
        {chrome::HintIcon::Btn3Green,   "Tab \xE2\x86\x92"},
        {chrome::HintIcon::Btn4Black,   "\xE2\x80\x94"},
        {chrome::HintIcon::RotaryNav,   "Browse"},
        {chrome::HintIcon::RotaryPress, "\xE2\x80\x94"},
    };
}

}  // namespace media_browser::ui
