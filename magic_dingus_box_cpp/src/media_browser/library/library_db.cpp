#include "media_browser/library/library_db.h"

#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <cstdio>

namespace media_browser {

LibraryDb::LibraryDb() = default;

LibraryDb::~LibraryDb() {
    close();
}

bool LibraryDb::open(const std::string& path) {
    if (db_) close();

    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        spdlog::error("[media_browser] sqlite3_open({}) failed: {}",
                      path, sqlite3_errmsg(db_));
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    // WAL mode for durability under concurrent readers.
    if (!exec("PRAGMA journal_mode=WAL;")) return false;
    if (!exec("PRAGMA foreign_keys=ON;")) return false;
    if (!exec("PRAGMA synchronous=NORMAL;")) return false;

    return true;
}

void LibraryDb::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool LibraryDb::exec(const std::string& sql) {
    if (!db_) return false;
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        spdlog::error("[media_browser] sqlite exec failed: {} | sql={}",
                      err ? err : "unknown", sql);
        sqlite3_free(err);
        return false;
    }
    return true;
}

int LibraryDb::schema_version() {
    if (!db_) return -1;
    // Check for schema_version table's existence first.
    const char* check =
        "SELECT name FROM sqlite_master WHERE type='table' AND name='schema_version';";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, check, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    if (!exists) return 0;

    const char* q = "SELECT MAX(version) FROM schema_version;";
    if (sqlite3_prepare_v2(db_, q, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    int v = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

// --- Migration framework --------------------------------------------

namespace {
struct Migration {
    int version;
    const char* name;
    const char* sql;
};

// Phase 1 ships only the schema_version table itself. Real schema lands in Task 4.
static const Migration MIGRATIONS[] = {
    {1, "schema_version_table",
     "CREATE TABLE IF NOT EXISTS schema_version ("
     "  version INTEGER PRIMARY KEY,"
     "  name TEXT NOT NULL,"
     "  applied_at INTEGER NOT NULL"
     ");"},
};
}  // namespace

bool LibraryDb::run_migrations() {
    if (!db_) return false;

    // Bootstrap: ensure schema_version table exists before we can query it.
    if (!exec(MIGRATIONS[0].sql)) return false;

    int current = schema_version();
    for (const auto& m : MIGRATIONS) {
        if (m.version <= current) continue;
        spdlog::info("[media_browser] applying migration {}: {}", m.version, m.name);
        if (!exec(m.sql)) {
            spdlog::error("[media_browser] migration {} failed", m.version);
            return false;
        }
        char insert[256];
        snprintf(insert, sizeof(insert),
                 "INSERT OR REPLACE INTO schema_version(version, name, applied_at) "
                 "VALUES (%d, '%s', strftime('%%s','now'));",
                 m.version, m.name);
        if (!exec(insert)) return false;
    }
    return true;
}

}  // namespace media_browser
