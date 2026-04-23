#pragma once

#include <string>

struct sqlite3;

namespace media_browser {

// Wraps a SQLite connection for the media browser's persistent state.
// Non-thread-safe: create one per thread, or serialize access externally.
class LibraryDb {
public:
    LibraryDb();
    ~LibraryDb();

    LibraryDb(const LibraryDb&) = delete;
    LibraryDb& operator=(const LibraryDb&) = delete;

    // Opens (creating if needed) a SQLite database at `path`.
    // Enables WAL mode and foreign keys. Returns true on success.
    bool open(const std::string& path);

    // Closes the connection. Called by destructor automatically.
    void close();

    // Returns the current schema version (0 if no schema_version table).
    int schema_version();

    // Applies all pending migrations in order. Idempotent.
    bool run_migrations();

    // Executes arbitrary SQL. Intended for tests / admin use.
    // For queries, prefer scoped helpers in later tasks.
    bool exec(const std::string& sql);

    // Raw handle escape hatch for submodules that need prepared statements.
    sqlite3* handle() { return db_; }

private:
    sqlite3* db_ = nullptr;
};

}  // namespace media_browser
