#include "media_browser/library/watch_store.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <climits>
#include <ctime>
#include <utility>

namespace media_browser::library {

namespace {

// The kind column's text values. Movie is the default MediaKind, so the
// fallthrough matches the enum's own bias.
const char* kind_text(bool tv) {
    return tv ? "tv" : "movie";
}

// The watched ratchet lives in the DO UPDATE: an unqualified `watched`
// refers to the EXISTING row, so MAX(watched, excluded.watched) can raise
// the flag but never lower it — a rewatch never un-watches.
constexpr const char* kUpsertSql =
    "INSERT INTO watch_state(kind, tmdb_id, season, episode, position_s, "
    "duration_s, watched, updated_at) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
    "ON CONFLICT(kind, tmdb_id, season, episode) DO UPDATE SET "
    "position_s=excluded.position_s, duration_s=excluded.duration_s, "
    "watched=MAX(watched, excluded.watched), updated_at=excluded.updated_at;";

// mark_watched touches ONLY the flag + timestamp on an existing row: the
// stored position/duration survive (the spec's "a watched row keeps its
// position"). A fresh row gets the column defaults (0 / 0).
constexpr const char* kMarkWatchedSql =
    "INSERT INTO watch_state(kind, tmdb_id, season, episode, watched, "
    "updated_at) VALUES(?1, ?2, ?3, ?4, 1, ?5) "
    "ON CONFLICT(kind, tmdb_id, season, episode) DO UPDATE SET "
    "watched=1, updated_at=excluded.updated_at;";

// The whole table, once, at open(). The CHECK constraint already limits
// kind to these two values; the filter keeps a hand-edited row out of the
// mirror exactly as the old per-kind queries kept it out of their results.
constexpr const char* kLoadSql =
    "SELECT kind, tmdb_id, season, episode, position_s, duration_s, watched "
    "FROM watch_state WHERE kind IN ('movie','tv');";

}  // namespace

WatchStore::~WatchStore() {
    close();
}

void WatchStore::warn_once(const std::string& why) {
    if (warned_.exchange(true)) return;
    spdlog::warn("[media_browser] watch store degraded ({}) — watch state "
                 "will not persist this session", why);
}

void WatchStore::finalize_statements() {
    sqlite3_stmt** stmts[] = {&upsert_stmt_, &mark_watched_stmt_};
    for (auto** s : stmts) {
        if (*s) {
            sqlite3_finalize(*s);
            *s = nullptr;
        }
    }
}

bool WatchStore::load_mirror() {
    rows_.clear();
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_.handle(), kLoadSql, -1, &s, nullptr) !=
        SQLITE_OK) {
        return false;
    }
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        const unsigned char* kind = sqlite3_column_text(s, 0);
        Key key;
        key.tv = kind != nullptr &&
                 std::string(reinterpret_cast<const char*>(kind)) == "tv";
        key.tmdb_id = sqlite3_column_int(s, 1);
        key.season = sqlite3_column_int(s, 2);
        key.episode = sqlite3_column_int(s, 3);
        Mirrored row;
        row.position_s = sqlite3_column_double(s, 4);
        row.duration_s = sqlite3_column_double(s, 5);
        row.watched = sqlite3_column_int(s, 6) != 0;
        rows_[key] = row;
    }
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

bool WatchStore::open(const std::string& db_path) {
    close();

    if (!db_.open(db_path)) {
        warn_once("open failed: " + db_path);
        return false;
    }
    if (!db_.run_migrations()) {
        warn_once("migrations failed: " + db_path);
        db_.close();
        return false;
    }

    struct StmtSpec {
        sqlite3_stmt** slot;
        const char* sql;
    };
    const StmtSpec specs[] = {
        {&upsert_stmt_, kUpsertSql},
        {&mark_watched_stmt_, kMarkWatchedSql},
    };
    for (const auto& spec : specs) {
        if (sqlite3_prepare_v2(db_.handle(), spec.sql, -1, spec.slot,
                               nullptr) != SQLITE_OK) {
            warn_once(std::string("prepare failed: ") +
                      sqlite3_errmsg(db_.handle()));
            finalize_statements();
            db_.close();
            return false;
        }
    }

    if (!load_mirror()) {
        warn_once(std::string("load failed: ") + sqlite3_errmsg(db_.handle()));
        rows_.clear();
        finalize_statements();
        db_.close();
        return false;
    }

    ok_ = true;
    return true;
}

void WatchStore::close() {
    // The writer owns the connection until it is idle. Nothing can queue
    // more while we wait: writes come only from this (the main) thread.
    worker_.flush();
    finalize_statements();
    db_.close();
    rows_.clear();
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_.clear();
    }
    ok_ = false;
}

bool WatchStore::flush_until(std::chrono::steady_clock::time_point deadline) {
    // Every enqueue submits a job that takes ALL of pending_, so an idle
    // worker means nothing is left to write.
    return worker_.flush_until(deadline);
}

void WatchStore::set_before_write_for_tests(std::function<void()> hook) {
    std::lock_guard<std::mutex> lk(mu_);
    before_write_ = std::move(hook);
}

void WatchStore::enqueue(const Key& key, const Pending& op) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto [it, inserted] = pending_.try_emplace(key, op);
        if (!inserted) {
            // Fold into what is already waiting for this key. Equivalent
            // to running the statements one after another: the latest
            // position replaces the earlier one, and `watched` is a
            // ratchet in SQL too.
            Pending& p = it->second;
            if (op.has_position) {
                p.has_position = true;
                p.position_s = op.position_s;
                p.duration_s = op.duration_s;
            }
            p.watched = p.watched || op.watched;
            p.updated_at = op.updated_at;
        }
    }
    // Never waits for a write in flight. A job the writer has not started
    // is replaced by this one, which does the same thing: take everything.
    worker_.submit([this] { return write_pending(); });
}

bool WatchStore::write_pending() {
    std::map<Key, Pending> batch;
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lk(mu_);
        batch.swap(pending_);
        hook = before_write_;
    }
    if (batch.empty()) return true;
    if (hook) hook();

    // One autocommit statement per key, as before the writer thread
    // existed: a statement that fails (disk full, a value SQLite rejects)
    // costs that one row, never the rest of the batch.
    bool all_ok = true;
    for (const auto& [key, op] : batch) {
        sqlite3_stmt* s = op.has_position ? upsert_stmt_ : mark_watched_stmt_;
        sqlite3_reset(s);
        sqlite3_bind_text(s, 1, kind_text(key.tv), -1, SQLITE_STATIC);
        sqlite3_bind_int(s, 2, key.tmdb_id);
        sqlite3_bind_int(s, 3, key.season);
        sqlite3_bind_int(s, 4, key.episode);
        if (op.has_position) {
            sqlite3_bind_double(s, 5, op.position_s);
            sqlite3_bind_double(s, 6, op.duration_s);
            sqlite3_bind_int(s, 7, op.watched ? 1 : 0);
            sqlite3_bind_int64(s, 8, static_cast<sqlite3_int64>(op.updated_at));
        } else {
            sqlite3_bind_int64(s, 5, static_cast<sqlite3_int64>(op.updated_at));
        }
        ++disk_writes_;
        if (sqlite3_step(s) != SQLITE_DONE) {
            all_ok = false;
            warn_once(std::string(op.has_position ? "upsert failed: "
                                                  : "mark_watched failed: ") +
                      sqlite3_errmsg(db_.handle()));
        }
        sqlite3_reset(s);
    }
    return all_ok;
}

void WatchStore::upsert_position(const MediaRef& ref, int season, int episode,
                                 double position_s, double duration_s) {
    if (!ok_) {
        warn_once("not open");
        return;
    }
    const Key key{ref.kind == MediaKind::Tv, ref.id, season, episode};
    const bool watched = ui::is_watched_position(position_s, duration_s);

    Mirrored& row = rows_[key];
    row.position_s = position_s;
    row.duration_s = duration_s;
    row.watched = row.watched || watched;

    Pending op;
    op.has_position = true;
    op.position_s = position_s;
    op.duration_s = duration_s;
    op.watched = watched;
    op.updated_at = static_cast<std::int64_t>(time(nullptr));
    enqueue(key, op);
}

void WatchStore::mark_watched(const MediaRef& ref, int season, int episode) {
    if (!ok_) {
        warn_once("not open");
        return;
    }
    const Key key{ref.kind == MediaKind::Tv, ref.id, season, episode};

    // A fresh row keeps the column defaults (0 / 0), like the SQL.
    rows_[key].watched = true;

    Pending op;
    op.watched = true;
    op.updated_at = static_cast<std::int64_t>(time(nullptr));
    enqueue(key, op);
}

ui::watch_map WatchStore::series_watch(int tmdb_id) {
    ui::watch_map out;
    if (!ok_) {
        warn_once("not open");
        return out;
    }
    // Keys order by (kind, tmdb_id, season, episode): one series' rows are
    // contiguous, starting at the smallest possible (season, episode).
    for (auto it = rows_.lower_bound(Key{true, tmdb_id, INT_MIN, INT_MIN});
         it != rows_.end() && it->first.tv && it->first.tmdb_id == tmdb_id;
         ++it) {
        ui::WatchRowLite row;
        row.position_s = it->second.position_s;
        row.duration_s = it->second.duration_s;
        row.watched = it->second.watched;
        out[ui::WatchKey{it->first.season, it->first.episode}] = row;
    }
    return out;
}

std::optional<WatchStore::Row> WatchStore::movie_watch(int tmdb_id) {
    if (!ok_) {
        warn_once("not open");
        return std::nullopt;
    }
    const auto it = rows_.find(Key{false, tmdb_id, 0, 0});
    if (it == rows_.end()) return std::nullopt;
    Row row;
    row.position_s = it->second.position_s;
    row.duration_s = it->second.duration_s;
    row.watched = it->second.watched;
    return row;
}

std::unordered_set<int> WatchStore::watched_movie_ids() {
    std::unordered_set<int> out;
    if (!ok_) {
        warn_once("not open");
        return out;
    }
    for (const auto& [key, row] : rows_) {
        if (key.tv) break;  // movies sort first
        if (row.watched) out.insert(key.tmdb_id);
    }
    return out;
}

std::unordered_set<int> WatchStore::started_movie_ids() {
    std::unordered_set<int> out;
    if (!ok_) {
        warn_once("not open");
        return out;
    }
    for (const auto& [key, row] : rows_) {
        if (key.tv) break;  // movies sort first
        out.insert(key.tmdb_id);
    }
    return out;
}

std::unordered_map<int, int> WatchStore::tv_watched_counts() {
    std::unordered_map<int, int> out;
    if (!ok_) {
        warn_once("not open");
        return out;
    }
    // Season 0 (specials) is excluded everywhere, matching
    // merge_season_rows — a watched special must not inflate a series'
    // watched count.
    for (const auto& [key, row] : rows_) {
        if (!key.tv || !row.watched || key.season == 0) continue;
        ++out[key.tmdb_id];
    }
    return out;
}

}  // namespace media_browser::library
