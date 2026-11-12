#pragma once

// The watch_state table (LibraryDb v3): resume positions + watched flags.
// Phase 3 spec: 2026-08-02-tv-playback-design.md.
//
// THREADING — the public API is main/render-thread-only, and none of it
// touches the SD card after open():
//
//   - READS are served from an in-memory mirror of the whole table, loaded
//     once by open(). The table is small (one row per title/episode ever
//     played) and nothing else writes it, so the mirror is the read model.
//   - WRITES update the mirror at once (so the resume point and the
//     watched counts the UI reads right after a checkpoint are current)
//     and are queued for a writer thread, coalesced per key: the latest
//     position wins and `watched` only ever ratchets up.
//
// Why: writes used to run on the render loop — every 30 s during Media
// Browser playback, at end-of-stream and on leaving Playback — and that
// loop pings the systemd watchdog (WatchdogSec=10). A WAL append is a file
// write that can sit behind a saturated SD card (see status_writer.h for
// the 2026-10-26 incident), and every ~1000 WAL pages SQLite checkpoints
// on the committing thread: two fsyncs, in the middle of a movie.
//
// The SQLite connection is used by ONE thread at a time: by open() and
// close() on the caller, and by the writer thread in between (close()
// waits for it first). That hand-off is safe in every SQLITE_THREADSAFE
// build, and nothing else in the kiosk uses SQLite.
//
// Every method is BEST-EFFORT: if open() failed (or was never called),
// methods no-op / return empty and the store logs a single spdlog warning
// for the whole session. A write that fails on disk is logged once and
// dropped; the mirror keeps it for the rest of the session. Playback must
// never block on the watch store.
//
// Storage is keyed (kind, tmdb_id, season, episode) — the MediaRef doctrine
// applied to SQLite: the TMDB movie and TV id spaces overlap completely, so
// a bare tmdb_id is never a key across kinds. Movies use season=episode=0.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "media_browser/library/library_db.h"
#include "media_browser/media_ref.h"
#include "media_browser/ui/episode_logic.h"
#include "utils/latest_wins_worker.h"

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
    // statements and loads the table into memory. Returns false on any
    // failure -> degraded mode: ok() stays false and every method below is
    // a safe no-op.
    bool open(const std::string& db_path);

    // True when open() succeeded and the store is live.
    bool ok() const { return ok_; }

    // Waits for every queued write, then finalizes statements and closes
    // the DB. Destructor calls it. Unbounded — main()'s _exit() path calls
    // flush_until() instead and never gets here.
    void close();

    // Records a playback position checkpoint. Also sets watched=1 when
    // ui::is_watched_position(position_s, duration_s) — and watched is a
    // RATCHET (MAX with the stored flag): a later rewatch or seek-to-start
    // never un-watches. Returns immediately; the disk write is queued.
    void upsert_position(const MediaRef& ref, int season, int episode,
                         double position_s, double duration_s);

    // Sets watched=1 (EOS path). Preserves any stored position/duration.
    // Returns immediately; the disk write is queued.
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

    // Blocks until every queued write has been committed (or dropped), or
    // `deadline` passes — false then. For shutdown (bounded, so a wedged
    // card cannot hold the stop) and tests. NEVER the render loop.
    bool flush_until(std::chrono::steady_clock::time_point deadline);
    bool flush(std::chrono::milliseconds timeout) {
        return flush_until(std::chrono::steady_clock::now() + timeout);
    }

    // Test seams. The hook runs on the WRITER thread before it writes a
    // batch (a test parks it there to stand in for a stalled card).
    // disk_writes_for_tests() counts the statements the writer has run.
    void set_before_write_for_tests(std::function<void()> hook);
    int disk_writes_for_tests() const { return disk_writes_.load(); }

private:
    // (kind, tmdb_id, season, episode). Ordered so one series' episodes
    // are contiguous.
    struct Key {
        bool tv = false;
        int tmdb_id = 0;
        int season = 0;
        int episode = 0;
        bool operator<(const Key& o) const {
            return std::tie(tv, tmdb_id, season, episode) <
                   std::tie(o.tv, o.tmdb_id, o.season, o.episode);
        }
    };
    // One row as the mirror holds it.
    struct Mirrored {
        double position_s = 0;
        double duration_s = 0;
        bool watched = false;
    };
    // The not-yet-written change for one key: everything queued for it
    // since the writer last took it, folded together.
    struct Pending {
        bool has_position = false;  // an upsert is included
        double position_s = 0;      // ...the LATEST one's values
        double duration_s = 0;
        bool watched = false;       // any included op set the flag
        std::int64_t updated_at = 0;
    };

    // Logs the best-effort degradation warning once per store lifetime.
    // Called from both threads.
    void warn_once(const std::string& why);
    // Finalizes all prepared statements (idempotent).
    void finalize_statements();
    // Reads the whole table into rows_. open() only.
    bool load_mirror();
    // Main thread: fold `op` into the queue and wake the writer.
    void enqueue(const Key& key, const Pending& op);
    // Writer thread: take everything queued and write it.
    bool write_pending();

    LibraryDb db_;
    bool ok_ = false;
    std::atomic<bool> warned_{false};

    // Used by the writer thread while the store is open (see THREADING).
    sqlite3_stmt* upsert_stmt_ = nullptr;
    sqlite3_stmt* mark_watched_stmt_ = nullptr;

    // The read model. Main thread only.
    std::map<Key, Mirrored> rows_;

    // Queue between the main thread and the writer. mu_ is never held
    // across a disk write.
    std::mutex mu_;
    std::map<Key, Pending> pending_;
    std::function<void()> before_write_;
    std::atomic<int> disk_writes_{0};

    // LAST member: destroyed (joined) first.
    ::utils::LatestWinsWorker worker_;
};

}  // namespace media_browser::library
