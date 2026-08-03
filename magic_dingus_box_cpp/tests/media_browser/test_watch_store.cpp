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
