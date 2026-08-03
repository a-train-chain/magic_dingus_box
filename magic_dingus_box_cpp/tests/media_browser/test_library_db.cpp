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
