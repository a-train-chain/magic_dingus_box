// WatchStore contract tests (Phase 3 Task 3, spec
// 2026-08-02-tv-playback-design.md). Temp-file DB per test in the
// test_library_db.cpp style. The collision pin matters most: TMDB movie and
// TV id spaces overlap completely, so a movie watched at tmdb_id N must
// never leak into TV aggregates for the same N (and vice versa).

#include <catch2/catch_test_macros.hpp>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "media_browser/library/library_db.h"
#include "media_browser/library/watch_store.h"
#include "media_browser/media_ref.h"

namespace fs = std::filesystem;

using media_browser::LibraryDb;
using media_browser::MediaKind;
using media_browser::MediaRef;
using media_browser::library::WatchStore;

static fs::path make_temp_db_path() {
    auto p = fs::temp_directory_path() /
        ("mdb_watch_test_" + std::to_string(std::rand()) + ".db");
    if (fs::exists(p)) fs::remove(p);
    return p;
}

// Removes the DB plus WAL/SHM sidecars — the store runs in WAL mode.
static void remove_db(const fs::path& p) {
    std::error_code ec;
    fs::remove(p, ec);
    fs::remove(fs::path(p.string() + "-wal"), ec);
    fs::remove(fs::path(p.string() + "-shm"), ec);
}

// Single-value scalar query straight through the LibraryDb handle; exec()
// discards rows, so verification goes through sqlite3 directly.
static int query_int(LibraryDb& db, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(db.handle(), sql.c_str(), -1, &stmt, nullptr) ==
            SQLITE_OK);
    int v = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

static MediaRef tv_ref(int id) { return MediaRef{MediaKind::Tv, id}; }
static MediaRef movie_ref(int id) { return MediaRef{MediaKind::Movie, id}; }

// Counts warn-level messages so the degraded-mode test can pin "exactly one
// warn" — best-effort means quiet, not silent, and not spammy either.
class CountingWarnSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::atomic<int> warn_count{0};

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        if (msg.level == spdlog::level::warn) ++warn_count;
    }
    void flush_() override {}
};

TEST_CASE("WatchStore: fresh open migrates to schema v3", "[watch_store]") {
    auto path = make_temp_db_path();
    {
        WatchStore store;
        REQUIRE(store.open(path.string()));
        REQUIRE(store.ok());
    }
    LibraryDb db;
    REQUIRE(db.open(path.string()));
    REQUIRE(db.schema_version() == 3);
    // The v3 objects exist.
    REQUIRE(query_int(db,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
        "AND name='watch_state';") == 1);
    REQUIRE(query_int(db,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' "
        "AND name='idx_watch_lookup';") == 1);
    db.close();
    remove_db(path);
}

TEST_CASE("WatchStore: v2 database upgrades to v3 preserving rows",
          "[watch_store]") {
    auto path = make_temp_db_path();
    {
        // Hand-build a v2 database: v1 + v2 SQL and their version rows,
        // exactly what a pre-TV box has on disk.
        LibraryDb db;
        REQUIRE(db.open(path.string()));
        REQUIRE(db.exec(
            "CREATE TABLE schema_version ("
            "  version INTEGER PRIMARY KEY,"
            "  name TEXT NOT NULL,"
            "  applied_at INTEGER NOT NULL"
            ");"));
        REQUIRE(db.exec(
            "CREATE TABLE titles ("
            "  id INTEGER PRIMARY KEY,"
            "  tmdb_id INTEGER NOT NULL UNIQUE,"
            "  kind TEXT NOT NULL CHECK(kind IN ('movie','tv')),"
            "  title TEXT NOT NULL,"
            "  original_title TEXT,"
            "  year INTEGER,"
            "  overview TEXT,"
            "  poster_path TEXT,"
            "  fanart_path TEXT,"
            "  runtime_minutes INTEGER,"
            "  tmdb_rating REAL,"
            "  added_at INTEGER NOT NULL,"
            "  updated_at INTEGER NOT NULL"
            ");"
            "CREATE TABLE queue ("
            "  id INTEGER PRIMARY KEY,"
            "  title_id INTEGER NOT NULL REFERENCES titles(id),"
            "  state TEXT NOT NULL,"
            "  torrent_hash TEXT,"
            "  progress REAL DEFAULT 0,"
            "  last_error TEXT,"
            "  started_at INTEGER NOT NULL,"
            "  updated_at INTEGER NOT NULL"
            ");"
            "CREATE TABLE history ("
            "  id INTEGER PRIMARY KEY,"
            "  title_id INTEGER REFERENCES titles(id),"
            "  event TEXT NOT NULL,"
            "  release_name TEXT,"
            "  detail TEXT,"
            "  occurred_at INTEGER NOT NULL"
            ");"
            "CREATE INDEX idx_queue_state ON queue(state);"
            "CREATE INDEX idx_history_title ON history(title_id);"));
        REQUIRE(db.exec(
            "INSERT INTO schema_version(version, name, applied_at) VALUES "
            "(1, 'schema_version_table', 0), (2, 'phase1_core_tables', 0);"));
        REQUIRE(db.exec(
            "INSERT INTO titles(tmdb_id, kind, title, year, added_at, "
            "updated_at) VALUES (603, 'movie', 'The Matrix', 1999, 0, 0);"));
        REQUIRE(db.schema_version() == 2);
    }
    {
        WatchStore store;
        REQUIRE(store.open(path.string()));
        REQUIRE(store.ok());
    }
    LibraryDb db;
    REQUIRE(db.open(path.string()));
    REQUIRE(db.schema_version() == 3);
    // The pre-existing v2 row survived the upgrade.
    REQUIRE(query_int(db,
        "SELECT COUNT(*) FROM titles WHERE tmdb_id=603;") == 1);
    db.close();
    remove_db(path);
}

TEST_CASE("WatchStore: upsert then read back via series_watch",
          "[watch_store]") {
    auto path = make_temp_db_path();
    WatchStore store;
    REQUIRE(store.open(path.string()));

    store.upsert_position(tv_ref(1396), 1, 2, 120.0, 3600.0);
    auto map = store.series_watch(1396);
    REQUIRE(map.size() == 1);
    auto it = map.find(media_browser::ui::WatchKey{1, 2});
    REQUIRE(it != map.end());
    REQUIRE(it->second.position_s == 120.0);
    REQUIRE(it->second.duration_s == 3600.0);
    REQUIRE_FALSE(it->second.watched);

    // Upsert on the same key updates in place — no second row.
    store.upsert_position(tv_ref(1396), 1, 2, 240.0, 3600.0);
    map = store.series_watch(1396);
    REQUIRE(map.size() == 1);
    REQUIRE(map.at(media_browser::ui::WatchKey{1, 2}).position_s == 240.0);

    store.close();
    remove_db(path);
}

TEST_CASE("WatchStore: watched ratchet survives a position regress",
          "[watch_store]") {
    auto path = make_temp_db_path();
    WatchStore store;
    REQUIRE(store.open(path.string()));

    // Cross the threshold, then seek back to the start: watched must stay.
    store.upsert_position(tv_ref(1396), 1, 1, 3500.0, 3600.0);
    auto map = store.series_watch(1396);
    REQUIRE(map.at(media_browser::ui::WatchKey{1, 1}).watched);

    store.upsert_position(tv_ref(1396), 1, 1, 0.0, 3600.0);
    map = store.series_watch(1396);
    REQUIRE(map.at(media_browser::ui::WatchKey{1, 1}).watched);
    REQUIRE(map.at(media_browser::ui::WatchKey{1, 1}).position_s == 0.0);

    store.close();
    remove_db(path);
}

TEST_CASE("WatchStore: upsert_position auto-sets watched at 0.93 fraction",
          "[watch_store]") {
    auto path = make_temp_db_path();
    WatchStore store;
    REQUIRE(store.open(path.string()));

    // 0.93 >= kWatchedFraction (0.92) — the store applies
    // ui::is_watched_position, no separate mark_watched call needed.
    store.upsert_position(tv_ref(500), 2, 7, 930.0, 1000.0);
    auto map = store.series_watch(500);
    REQUIRE(map.at(media_browser::ui::WatchKey{2, 7}).watched);

    // Just below the threshold does not.
    store.upsert_position(tv_ref(500), 2, 8, 910.0, 1000.0);
    map = store.series_watch(500);
    REQUIRE_FALSE(map.at(media_browser::ui::WatchKey{2, 8}).watched);

    store.close();
    remove_db(path);
}

TEST_CASE("WatchStore: movie_watch keys on season=episode=0",
          "[watch_store]") {
    auto path = make_temp_db_path();
    WatchStore store;
    REQUIRE(store.open(path.string()));

    store.upsert_position(movie_ref(42), 0, 0, 500.0, 6000.0);
    auto row = store.movie_watch(42);
    REQUIRE(row.has_value());
    REQUIRE(row->season == 0);
    REQUIRE(row->episode == 0);
    REQUIRE(row->position_s == 500.0);
    REQUIRE(row->duration_s == 6000.0);
    REQUIRE_FALSE(row->watched);

    // No row for an id never written.
    REQUIRE_FALSE(store.movie_watch(999).has_value());

    // mark_watched sets the flag without clobbering the stored position.
    store.mark_watched(movie_ref(42), 0, 0);
    row = store.movie_watch(42);
    REQUIRE(row.has_value());
    REQUIRE(row->watched);
    REQUIRE(row->position_s == 500.0);

    store.close();
    remove_db(path);
}

TEST_CASE("WatchStore: watched_movie_ids and tv_watched_counts aggregate",
          "[watch_store]") {
    auto path = make_temp_db_path();
    WatchStore store;
    REQUIRE(store.open(path.string()));

    store.mark_watched(movie_ref(7), 0, 0);                  // watched movie
    store.upsert_position(movie_ref(8), 0, 0, 100.0, 6000.0); // in progress
    store.mark_watched(tv_ref(1399), 1, 1);
    store.mark_watched(tv_ref(1399), 1, 2);
    store.upsert_position(tv_ref(1399), 1, 3, 30.0, 3600.0);  // unwatched

    auto ids = store.watched_movie_ids();
    REQUIRE(ids.size() == 1);
    REQUIRE(ids.count(7) == 1);

    auto counts = store.tv_watched_counts();
    REQUIRE(counts.size() == 1);
    REQUIRE(counts.at(1399) == 2);

    store.close();
    remove_db(path);
}

TEST_CASE("WatchStore: movie and tv sharing a tmdb_id never cross-count",
          "[watch_store][collision]") {
    auto path = make_temp_db_path();
    WatchStore store;
    REQUIRE(store.open(path.string()));

    // tmdb_id 1396 is Breaking Bad (TV) AND an unrelated movie. Watch the
    // MOVIE to completion; the TV side has only an in-progress episode.
    store.mark_watched(movie_ref(1396), 0, 0);
    store.upsert_position(tv_ref(1396), 1, 1, 30.0, 3600.0);

    // The watched movie must NOT count toward tv 1396.
    auto counts = store.tv_watched_counts();
    REQUIRE(counts.find(1396) == counts.end());

    // And the tv row must not surface as a watched movie.
    auto ids = store.watched_movie_ids();
    REQUIRE(ids.count(1396) == 1);  // the movie, watched above
    store.mark_watched(tv_ref(1396), 1, 1);
    counts = store.tv_watched_counts();
    REQUIRE(counts.at(1396) == 1);  // the episode — not 2
    ids = store.watched_movie_ids();
    REQUIRE(ids.size() == 1);       // still just the movie

    // series_watch never returns the movie row.
    auto map = store.series_watch(1396);
    REQUIRE(map.size() == 1);
    REQUIRE(map.count(media_browser::ui::WatchKey{1, 1}) == 1);
    REQUIRE(map.count(media_browser::ui::WatchKey{0, 0}) == 0);

    // movie_watch never returns the tv row.
    auto row = store.movie_watch(1396);
    REQUIRE(row.has_value());
    REQUIRE(row->watched);

    store.close();
    remove_db(path);
}

TEST_CASE("WatchStore: degraded mode is quiet, empty, and warns exactly once",
          "[watch_store]") {
    auto sink = std::make_shared<CountingWarnSink>();
    auto logger = std::make_shared<spdlog::logger>("watch_store_test", sink);
    auto prev = spdlog::default_logger();
    spdlog::set_default_logger(logger);

    {
        WatchStore store;
        REQUIRE_FALSE(store.open("/nonexistent-dir-mdb/sub/x.db"));
        REQUIRE_FALSE(store.ok());

        // Every method is a no-op / empty — and none of them crash.
        store.upsert_position(tv_ref(1), 1, 1, 100.0, 200.0);
        store.mark_watched(tv_ref(1), 1, 1);
        REQUIRE(store.series_watch(1).empty());
        REQUIRE_FALSE(store.movie_watch(1).has_value());
        REQUIRE(store.watched_movie_ids().empty());
        REQUIRE(store.tv_watched_counts().empty());

        REQUIRE(sink->warn_count.load() == 1);
    }

    spdlog::set_default_logger(prev);
}

TEST_CASE("started_movie_ids: any row counts, kind-isolated",
          "[watch_store]") {
    auto path = make_temp_db_path();
    media_browser::library::WatchStore ws;
    REQUIRE(ws.open(path.string()));

    // In-progress (not watched) movie row -> started.
    ws.upsert_position(MediaRef{MediaKind::Movie, 603}, 0, 0, 300.0, 7200.0);
    // Watched movie row -> also started.
    ws.mark_watched(MediaRef{MediaKind::Movie, 680}, 0, 0);
    // TV row with the SAME id as an absent movie must not leak in.
    ws.upsert_position(MediaRef{MediaKind::Tv, 555}, 1, 1, 300.0, 3600.0);

    const auto started = ws.started_movie_ids();
    REQUIRE(started.count(603) == 1);
    REQUIRE(started.count(680) == 1);
    REQUIRE(started.count(555) == 0);
    // watched_movie_ids stays the strict subset.
    const auto watched = ws.watched_movie_ids();
    REQUIRE(watched.count(603) == 0);
    REQUIRE(watched.count(680) == 1);
}

// ── Writer thread + in-memory mirror ────────────────────────────────────
// Watch writes used to run on the render loop (every 30 s during playback,
// at end-of-stream, on leaving Playback), and that loop pings the systemd
// watchdog (WatchdogSec=10): a WAL append can sit behind a saturated SD
// card, and every ~1000 WAL pages SQLite checkpoints — two fsyncs — on the
// committing thread. Now writes are queued for a writer thread and reads
// come from a mirror of the table. These pin: the caller never waits,
// reads see every write at once, the queue coalesces per key, the bounded
// flush shutdown uses, and that what lands on disk is exactly what the
// mirror said.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>
#include <condition_variable>
#include <limits>

namespace {

using namespace std::chrono_literals;

// Parks the writer thread before a batch, standing in for a card stuck
// behind a writeback queue.
struct StalledWriter {
    std::mutex mu;
    std::condition_variable cv;
    bool released = false;
    bool entered = false;

    void install(WatchStore& store) {
        store.set_before_write_for_tests([this] {
            std::unique_lock<std::mutex> lk(mu);
            entered = true;
            cv.notify_all();
            cv.wait(lk, [this] { return released; });
        });
    }
    void wait_entered() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [this] { return entered; });
    }
    void release() {
        std::lock_guard<std::mutex> lk(mu);
        released = true;
        cv.notify_all();
    }
};

// Everything the UI can read, for comparing a live store with a reopened one.
struct Snapshot {
    std::vector<std::tuple<int, int, int, double, double, bool>> tv;
    std::vector<std::tuple<int, double, double, bool>> movies;
    std::vector<int> watched_movies;
    std::vector<int> started_movies;
    std::vector<std::pair<int, int>> tv_counts;
    bool operator==(const Snapshot& o) const {
        return tv == o.tv && movies == o.movies &&
               watched_movies == o.watched_movies &&
               started_movies == o.started_movies && tv_counts == o.tv_counts;
    }
};

Snapshot snapshot_of(WatchStore& s, const std::vector<int>& ids) {
    Snapshot out;
    for (int id : ids) {
        for (const auto& [k, row] : s.series_watch(id)) {
            out.tv.emplace_back(id, k.season, k.episode, row.position_s,
                                row.duration_s, row.watched);
        }
        if (auto m = s.movie_watch(id)) {
            out.movies.emplace_back(id, m->position_s, m->duration_s, m->watched);
        }
    }
    std::sort(out.tv.begin(), out.tv.end());
    for (int id : s.watched_movie_ids()) out.watched_movies.push_back(id);
    for (int id : s.started_movie_ids()) out.started_movies.push_back(id);
    for (const auto& [id, n] : s.tv_watched_counts()) out.tv_counts.emplace_back(id, n);
    std::sort(out.watched_movies.begin(), out.watched_movies.end());
    std::sort(out.started_movies.begin(), out.started_movies.end());
    std::sort(out.tv_counts.begin(), out.tv_counts.end());
    return out;
}

}  // namespace

TEST_CASE("WatchStore: a write never waits for the disk, reads see it at "
          "once, and the queue keeps only the latest per key",
          "[watch_store][io]") {
    auto path = make_temp_db_path();
    {
        StalledWriter stalled;   // declared first: must outlive the store
        WatchStore store;
        REQUIRE(store.open(path.string()));
        stalled.install(store);

        store.upsert_position(tv_ref(1396), 1, 1, 30.0, 3600.0);
        stalled.wait_entered();   // the writer is now stuck "on the card"

        // An hour of 30 s checkpoints, an EOS mark and a movie, all while
        // the card is stuck. The old synchronous store blocked the render
        // loop on each one.
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 2; i <= 120; ++i) {
            store.upsert_position(tv_ref(1396), 1, 1, 30.0 * i, 3600.0);
        }
        store.mark_watched(tv_ref(1396), 1, 2);
        store.upsert_position(movie_ref(603), 0, 0, 500.0, 7200.0);
        CHECK(std::chrono::steady_clock::now() - t0 < 500ms);

        // Read-your-writes, with nothing on disk yet.
        auto map = store.series_watch(1396);
        REQUIRE(map.size() == 2);
        CHECK(map.at(media_browser::ui::WatchKey{1, 1}).position_s == 3600.0);
        CHECK(map.at(media_browser::ui::WatchKey{1, 1}).watched);
        CHECK(map.at(media_browser::ui::WatchKey{1, 2}).watched);
        REQUIRE(store.movie_watch(603).has_value());
        CHECK(store.movie_watch(603)->position_s == 500.0);
        CHECK(store.tv_watched_counts().at(1396) == 2);
        CHECK(store.started_movie_ids().count(603) == 1);

        // Shutdown's flush is bounded: a wedged card cannot hold it.
        CHECK_FALSE(store.flush(20ms));

        stalled.release();
        REQUIRE(store.flush(10s));
        // The write in flight, then ONE statement per key that changed —
        // not 121 queued checkpoints for the card to work through.
        CHECK(store.disk_writes_for_tests() == 1 + 3);
    }
    // A fresh process sees exactly what was read back above.
    WatchStore reopened;
    REQUIRE(reopened.open(path.string()));
    auto map = reopened.series_watch(1396);
    REQUIRE(map.size() == 2);
    CHECK(map.at(media_browser::ui::WatchKey{1, 1}).position_s == 3600.0);
    CHECK(map.at(media_browser::ui::WatchKey{1, 1}).watched);
    CHECK(map.at(media_browser::ui::WatchKey{1, 2}).watched);
    CHECK(reopened.movie_watch(603)->position_s == 500.0);
    reopened.close();
    remove_db(path);
}

TEST_CASE("WatchStore: folding queued writes equals running them in order",
          "[watch_store][io]") {
    auto path = make_temp_db_path();
    const std::vector<int> ids = {7, 8, 9, 10, 11};
    Snapshot live;
    {
        StalledWriter stalled;   // declared first: must outlive the store
        WatchStore store;
        REQUIRE(store.open(path.string()));
        // A row that already exists on disk, so the ratchet and the
        // preserved position have something to act on.
        store.upsert_position(tv_ref(7), 1, 1, 950.0, 1000.0);   // watched
        store.upsert_position(tv_ref(7), 1, 2, 400.0, 1000.0);
        REQUIRE(store.flush(10s));

        stalled.install(store);
        store.upsert_position(tv_ref(99), 1, 1, 1.0, 10.0);
        stalled.wait_entered();
        // Everything below is queued behind the stuck write and folded.
        // Seek back on a watched episode: position regresses, flag stays.
        store.upsert_position(tv_ref(7), 1, 1, 5.0, 1000.0);
        // Mark, then keep playing: the flag survives the later checkpoint.
        store.mark_watched(tv_ref(7), 1, 2);
        store.upsert_position(tv_ref(7), 1, 2, 450.0, 1000.0);
        // Checkpoint, then mark: the mark keeps the position.
        store.upsert_position(tv_ref(8), 2, 3, 120.0, 1000.0);
        store.mark_watched(tv_ref(8), 2, 3);
        // Cross the threshold, then seek back before anything is written.
        store.upsert_position(tv_ref(9), 1, 1, 990.0, 1000.0);
        store.upsert_position(tv_ref(9), 1, 1, 10.0, 1000.0);
        // Mark-only on a row that does not exist: 0 / 0, watched.
        store.mark_watched(movie_ref(10), 0, 0);
        // A special (season 0) is watched but never counted.
        store.mark_watched(tv_ref(11), 0, 1);
        // A movie and a show sharing an id stay apart.
        store.upsert_position(movie_ref(7), 0, 0, 60.0, 6000.0);

        live = snapshot_of(store, ids);
        stalled.release();
        REQUIRE(store.flush(10s));
        CHECK(snapshot_of(store, ids) == live);
    }
    WatchStore reopened;
    REQUIRE(reopened.open(path.string()));
    const Snapshot disk = snapshot_of(reopened, ids);
    CHECK(disk == live);   // the disk holds exactly what the mirror served

    // And the values themselves, spelled out.
    auto s7 = reopened.series_watch(7);
    CHECK(s7.at(media_browser::ui::WatchKey{1, 1}).position_s == 5.0);
    CHECK(s7.at(media_browser::ui::WatchKey{1, 1}).watched);
    CHECK(s7.at(media_browser::ui::WatchKey{1, 2}).position_s == 450.0);
    CHECK(s7.at(media_browser::ui::WatchKey{1, 2}).watched);
    auto s8 = reopened.series_watch(8);
    CHECK(s8.at(media_browser::ui::WatchKey{2, 3}).position_s == 120.0);
    CHECK(s8.at(media_browser::ui::WatchKey{2, 3}).watched);
    auto s9 = reopened.series_watch(9);
    CHECK(s9.at(media_browser::ui::WatchKey{1, 1}).position_s == 10.0);
    CHECK(s9.at(media_browser::ui::WatchKey{1, 1}).watched);
    REQUIRE(reopened.movie_watch(10).has_value());
    CHECK(reopened.movie_watch(10)->position_s == 0.0);
    CHECK(reopened.movie_watch(10)->watched);
    CHECK(reopened.tv_watched_counts().count(11) == 0);
    CHECK(reopened.tv_watched_counts().at(7) == 2);
    CHECK(reopened.movie_watch(7)->position_s == 60.0);
    CHECK_FALSE(reopened.movie_watch(7)->watched);
    reopened.close();
    remove_db(path);
}

TEST_CASE("WatchStore: closing drains the queue (no flush needed)",
          "[watch_store][io]") {
    auto path = make_temp_db_path();
    {
        WatchStore store;
        REQUIRE(store.open(path.string()));
        store.upsert_position(movie_ref(550), 0, 0, 1234.0, 8000.0);
        store.mark_watched(tv_ref(42), 3, 4);
    }  // destructor only
    WatchStore reopened;
    REQUIRE(reopened.open(path.string()));
    REQUIRE(reopened.movie_watch(550).has_value());
    CHECK(reopened.movie_watch(550)->position_s == 1234.0);
    CHECK(reopened.series_watch(42).at(media_browser::ui::WatchKey{3, 4}).watched);
    reopened.close();
    remove_db(path);
}

TEST_CASE("WatchStore: a write the disk rejects costs that row only, and "
          "warns once",
          "[watch_store][io]") {
    auto sink = std::make_shared<CountingWarnSink>();
    auto logger = std::make_shared<spdlog::logger>("watch_store_io_test", sink);
    auto prev = spdlog::default_logger();
    spdlog::set_default_logger(logger);

    auto path = make_temp_db_path();
    {
        StalledWriter stalled;   // declared first: must outlive the store
        WatchStore store;
        REQUIRE(store.open(path.string()));
        stalled.install(store);
        store.upsert_position(tv_ref(1), 1, 1, 10.0, 100.0);
        stalled.wait_entered();
        // SQLite stores NaN as NULL, and duration_s is NOT NULL: this
        // statement fails. Twice, to pin "warns once".
        const double nan = std::numeric_limits<double>::quiet_NaN();
        store.upsert_position(tv_ref(2), 1, 1, 10.0, nan);
        store.upsert_position(tv_ref(3), 1, 1, 10.0, nan);
        // Queued in the same batch, sorted AFTER the failing rows.
        store.upsert_position(tv_ref(4), 1, 1, 40.0, 100.0);
        stalled.release();
        REQUIRE(store.flush(10s));
        CHECK(sink->warn_count.load() == 1);
        // Still usable afterwards.
        store.mark_watched(tv_ref(5), 1, 1);
        REQUIRE(store.flush(10s));
    }
    WatchStore reopened;
    REQUIRE(reopened.open(path.string()));
    CHECK(reopened.series_watch(1).size() == 1);
    CHECK(reopened.series_watch(2).empty());   // rejected
    CHECK(reopened.series_watch(3).empty());   // rejected
    CHECK(reopened.series_watch(4).at(media_browser::ui::WatchKey{1, 1}).position_s == 40.0);
    CHECK(reopened.series_watch(5).at(media_browser::ui::WatchKey{1, 1}).watched);
    reopened.close();
    remove_db(path);
    spdlog::set_default_logger(prev);
}

TEST_CASE("WatchStore: the mirror is loaded from disk, kind by kind",
          "[watch_store][io]") {
    auto path = make_temp_db_path();
    {
        // Rows written by plain SQL — what an older kiosk left behind.
        LibraryDb db;
        REQUIRE(db.open(path.string()));
        REQUIRE(db.run_migrations());
        REQUIRE(db.exec(
            "INSERT INTO watch_state(kind,tmdb_id,season,episode,position_s,"
            "duration_s,watched,updated_at) VALUES"
            "('movie',603,0,0,300,7200,0,1),"
            "('movie',680,0,0,7000,7200,1,1),"
            "('tv',603,1,1,100,3000,1,1),"
            "('tv',603,1,2,200,3000,0,1),"
            "('tv',603,0,1,0,0,1,1);"));
    }
    WatchStore store;
    REQUIRE(store.open(path.string()));
    CHECK(store.movie_watch(603)->position_s == 300.0);
    CHECK_FALSE(store.movie_watch(603)->watched);
    CHECK(store.watched_movie_ids() == std::unordered_set<int>{680});
    CHECK(store.started_movie_ids() == (std::unordered_set<int>{603, 680}));
    CHECK(store.series_watch(603).size() == 3);
    CHECK(store.series_watch(680).empty());            // a movie id, not a show
    CHECK(store.tv_watched_counts().at(603) == 1);     // the special is not counted
    CHECK(store.disk_writes_for_tests() == 0);         // reading writes nothing
    store.close();
    remove_db(path);
}
