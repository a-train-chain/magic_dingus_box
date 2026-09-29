#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <cstdlib>
#include "media_browser/library/library_db.h"

namespace fs = std::filesystem;

static fs::path make_temp_db_path() {
    auto p = fs::temp_directory_path() /
        ("mdb_test_" + std::to_string(std::rand()) + ".db");
    if (fs::exists(p)) fs::remove(p);
    return p;
}

TEST_CASE("LibraryDb: open creates the file", "[library_db]") {
    auto path = make_temp_db_path();
    {
        media_browser::LibraryDb db;
        REQUIRE(db.open(path.string()));
    }
    REQUIRE(fs::exists(path));
    fs::remove(path);
}

TEST_CASE("LibraryDb: schema_version is 0 before migrations", "[library_db]") {
    auto path = make_temp_db_path();
    media_browser::LibraryDb db;
    REQUIRE(db.open(path.string()));
    REQUIRE(db.schema_version() == 0);
    fs::remove(path);
}

TEST_CASE("LibraryDb: run_migrations creates schema_version table", "[library_db]") {
    auto path = make_temp_db_path();
    media_browser::LibraryDb db;
    REQUIRE(db.open(path.string()));
    REQUIRE(db.run_migrations());
    REQUIRE(db.schema_version() >= 1);
    fs::remove(path);
}

TEST_CASE("LibraryDb: run_migrations is idempotent", "[library_db]") {
    auto path = make_temp_db_path();
    media_browser::LibraryDb db;
    REQUIRE(db.open(path.string()));
    REQUIRE(db.run_migrations());
    int v1 = db.schema_version();
    REQUIRE(db.run_migrations());
    int v2 = db.schema_version();
    REQUIRE(v1 == v2);
    fs::remove(path);
}

TEST_CASE("LibraryDb: phase 1 schema creates titles table", "[library_db][schema]") {
    auto path = make_temp_db_path();
    media_browser::LibraryDb db;
    REQUIRE(db.open(path.string()));
    REQUIRE(db.run_migrations());
    REQUIRE(db.exec(
        "INSERT INTO titles(tmdb_id, kind, title, year, added_at, updated_at) "
        "VALUES (603, 'movie', 'The Matrix', 1999, 0, 0);"));
    fs::remove(path);
}

TEST_CASE("LibraryDb: phase 1 schema creates queue table", "[library_db][schema]") {
    auto path = make_temp_db_path();
    media_browser::LibraryDb db;
    REQUIRE(db.open(path.string()));
    REQUIRE(db.run_migrations());
    REQUIRE(db.exec(
        "INSERT INTO titles(tmdb_id, kind, title, year, added_at, updated_at) "
        "VALUES (603, 'movie', 'The Matrix', 1999, 0, 0);"));
    REQUIRE(db.exec(
        "INSERT INTO queue(title_id, state, started_at, updated_at) "
        "VALUES (1, 'searching', 0, 0);"));
    fs::remove(path);
}

TEST_CASE("LibraryDb: full migration lands at the current version (3)",
          "[library_db][schema]") {
    auto path = make_temp_db_path();
    media_browser::LibraryDb db;
    REQUIRE(db.open(path.string()));
    REQUIRE(db.run_migrations());
    // Bump this pin when appending to MIGRATIONS[] — it exists so a
    // migration that silently fails to apply can't go unnoticed.
    REQUIRE(db.schema_version() == 3);
    fs::remove(path);
}

// A DB left half-migrated by a pre-transaction build: migration 2's first
// table was committed (autocommit per statement) but the process died before
// the rest of v2 and before the version bump. Every later open used to fail
// forever on the bare "CREATE TABLE titles".
TEST_CASE("LibraryDb: a half-migrated DB (tables created, version unbumped) "
          "heals", "[library_db][schema]") {
    auto path = make_temp_db_path();
    {
        media_browser::LibraryDb db;
        REQUIRE(db.open(path.string()));
        REQUIRE(db.exec(
            "CREATE TABLE schema_version (version INTEGER PRIMARY KEY,"
            " name TEXT NOT NULL, applied_at INTEGER NOT NULL);"
            "INSERT INTO schema_version VALUES (1, 'schema_version_table', 0);"
            "CREATE TABLE titles (id INTEGER PRIMARY KEY,"
            " tmdb_id INTEGER NOT NULL UNIQUE, kind TEXT NOT NULL,"
            " title TEXT NOT NULL, original_title TEXT, year INTEGER,"
            " overview TEXT, poster_path TEXT, fanart_path TEXT,"
            " runtime_minutes INTEGER, tmdb_rating REAL,"
            " added_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);"));
        REQUIRE(db.schema_version() == 1);
    }
    media_browser::LibraryDb db;
    REQUIRE(db.open(path.string()));
    REQUIRE(db.run_migrations());
    REQUIRE(db.schema_version() == 3);
    // v3's table exists and works.
    REQUIRE(db.exec(
        "INSERT INTO watch_state(kind, tmdb_id, updated_at) "
        "VALUES ('movie', 603, 0);"));
    // Re-open is still fine.
    REQUIRE(db.run_migrations());
    fs::remove(path);
}

// Same shape one version later: v3's table landed, its index statement and
// the version bump did not.
TEST_CASE("LibraryDb: half-applied v3 (watch_state exists at version 2) heals",
          "[library_db][schema]") {
    auto path = make_temp_db_path();
    {
        media_browser::LibraryDb db;
        REQUIRE(db.open(path.string()));
        REQUIRE(db.run_migrations());
        REQUIRE(db.exec("DELETE FROM schema_version WHERE version = 3;"));
        REQUIRE(db.exec("DROP INDEX idx_watch_lookup;"));
        REQUIRE(db.schema_version() == 2);
    }
    media_browser::LibraryDb db;
    REQUIRE(db.open(path.string()));
    REQUIRE(db.run_migrations());
    REQUIRE(db.schema_version() == 3);
    fs::remove(path);
}

// A migration that fails partway must leave NOTHING behind: no version bump
// and no partially-created objects (one transaction per migration).
TEST_CASE("LibraryDb: a failing migration rolls back atomically",
          "[library_db][schema]") {
    auto path = make_temp_db_path();
    {
        media_browser::LibraryDb db;
        REQUIRE(db.open(path.string()));
        REQUIRE(db.run_migrations());
        // Rewind to v2 with an incompatible watch_state: v3's CREATE TABLE
        // IF NOT EXISTS is a no-op, then its CREATE INDEX on (kind, ...)
        // fails — mid-migration.
        REQUIRE(db.exec("DELETE FROM schema_version WHERE version = 3;"));
        REQUIRE(db.exec("DROP TABLE watch_state;"));
        REQUIRE(db.exec("CREATE TABLE watch_state (id INTEGER PRIMARY KEY);"));
    }
    media_browser::LibraryDb db;
    REQUIRE(db.open(path.string()));
    REQUIRE_FALSE(db.run_migrations());
    REQUIRE(db.schema_version() == 2);  // bump rolled back with the SQL
    // Not stuck inside an open transaction afterwards.
    REQUIRE(db.exec("BEGIN; COMMIT;"));
    fs::remove(path);
}
