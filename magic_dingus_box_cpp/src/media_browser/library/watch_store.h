#pragma once

// Prepared-statement wrapper over the watch_state table (LibraryDb v3).
// Phase 3 spec: 2026-08-02-tv-playback-design.md.
//
// Main/render-thread-only, like the LibraryDb it wraps (documented
// non-thread-safe) — workers never touch it. Every method is BEST-EFFORT:
// if open() failed (or was never called), methods no-op / return empty and
// the store logs a single spdlog warning for the whole session. Playback
// must never block on the watch store.
//
// Storage is keyed (kind, tmdb_id, season, episode) — the MediaRef doctrine
// applied to SQLite: the TMDB movie and TV id spaces overlap completely, so
// a bare tmdb_id is never a key across kinds. Movies use season=episode=0.

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "media_browser/library/library_db.h"
#include "media_browser/media_ref.h"
#include "media_browser/ui/episode_logic.h"

struct sqlite3_stmt;

namespace media_browser::library {

class WatchStore {
public:
    // A full watch_state row (movie_watch's return). The decision logic's
    // slice of this is ui::WatchRowLite; this adds the key fields.
    struct Row {
        int season = 0;
        int episode = 0;
        double position_s = 0;
        double duration_s = 0;
        bool watched = false;
    };

    WatchStore() = default;
    ~WatchStore();

    WatchStore(const WatchStore&) = delete;
    WatchStore& operator=(const WatchStore&) = delete;

    // Opens (creating if needed) the DB, runs migrations, prepares
    // statements. Returns false on any failure -> degraded mode: ok() stays
    // false and every method below is a safe no-op.
    bool open(const std::string& db_path);

    // True when open() succeeded and the store is live.
    bool ok() const { return ok_; }

    // Finalizes statements and closes the DB. Destructor calls it.
    void close();

    // Records a playback position checkpoint. Also sets watched=1 when
    // ui::is_watched_position(position_s, duration_s) — and watched is a
    // RATCHET (MAX with the stored flag): a later rewatch or seek-to-start
    // never un-watches.
    void upsert_position(const MediaRef& ref, int season, int episode,
                         double position_s, double duration_s);

    // Sets watched=1 (EOS path). Preserves any stored position/duration.
    void mark_watched(const MediaRef& ref, int season, int episode);

    // All watch rows for a TV series, keyed (season, episode). kind='tv'
    // only — a movie sharing the tmdb_id never appears.
    ui::watch_map series_watch(int tmdb_id);

    // The movie row (kind='movie', season=episode=0), if any.
    std::optional<Row> movie_watch(int tmdb_id);

    // tmdb_ids of movies with watched=1 — the Library filter's input.
    std::unordered_set<int> watched_movie_ids();

    // Movies with ANY watch row (opened at least once, watched or not).
    // Drives the Library's NEW badge: has-file movies absent from this
    // set have never been played. Rows appear via the 30 s checkpoint or
    // the exit flush, so "opened for a few seconds and backed out" may
    // still read as new — acceptable for a freshness marker.
    std::unordered_set<int> started_movie_ids();

    // tmdb_id -> COUNT(watched=1) across a series' episodes. Season 0
    // (specials) is excluded, matching merge_season_rows. Accepted v1
    // behavior: rows are never garbage-collected, so counts can exceed the
    // CURRENT files after episodes are deleted/re-sourced in Sonarr.
    std::unordered_map<int, int> tv_watched_counts();

private:
    // Logs the best-effort degradation warning once per store lifetime.
    void warn_once(const std::string& why);
    // Finalizes all prepared statements (idempotent).
    void finalize_statements();

    LibraryDb db_;
    bool ok_ = false;
    bool warned_ = false;

    sqlite3_stmt* upsert_stmt_ = nullptr;
    sqlite3_stmt* mark_watched_stmt_ = nullptr;
    sqlite3_stmt* series_stmt_ = nullptr;
    sqlite3_stmt* movie_stmt_ = nullptr;
    sqlite3_stmt* movie_ids_stmt_ = nullptr;
    sqlite3_stmt* started_ids_stmt_ = nullptr;
    sqlite3_stmt* tv_counts_stmt_ = nullptr;
};

}  // namespace media_browser::library
