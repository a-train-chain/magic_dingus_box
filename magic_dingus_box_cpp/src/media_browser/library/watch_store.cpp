#include "media_browser/library/watch_store.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <ctime>

namespace media_browser::library {

namespace {

// The kind column's text values. Movie is the default MediaKind, so the
// fallthrough matches the enum's own bias.
const char* kind_text(MediaKind kind) {
    return kind == MediaKind::Tv ? "tv" : "movie";
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

constexpr const char* kSeriesSql =
    "SELECT season, episode, position_s, duration_s, watched "
    "FROM watch_state WHERE kind='tv' AND tmdb_id=?1;";

constexpr const char* kMovieSql =
    "SELECT season, episode, position_s, duration_s, watched "
    "FROM watch_state WHERE kind='movie' AND tmdb_id=?1 "
    "AND season=0 AND episode=0;";

constexpr const char* kMovieIdsSql =
    "SELECT tmdb_id FROM watch_state WHERE kind='movie' AND watched=1;";

// Season 0 (specials) is excluded everywhere, matching merge_season_rows —
// a watched special must not inflate a series' watched count.
constexpr const char* kTvCountsSql =
    "SELECT tmdb_id, COUNT(*) FROM watch_state "
    "WHERE kind='tv' AND watched=1 AND season<>0 GROUP BY tmdb_id;";

}  // namespace

WatchStore::~WatchStore() {
    close();
}

void WatchStore::warn_once(const std::string& why) {
    if (warned_) return;
    warned_ = true;
    spdlog::warn("[media_browser] watch store degraded ({}) — watch state "
                 "will not persist this session", why);
}

void WatchStore::finalize_statements() {
    sqlite3_stmt** stmts[] = {&upsert_stmt_, &mark_watched_stmt_,
                              &series_stmt_, &movie_stmt_,
                              &movie_ids_stmt_, &tv_counts_stmt_};
    for (auto** s : stmts) {
        if (*s) {
            sqlite3_finalize(*s);
            *s = nullptr;
        }
    }
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
        {&upsert_stmt_, kUpsertSql},   {&mark_watched_stmt_, kMarkWatchedSql},
        {&series_stmt_, kSeriesSql},   {&movie_stmt_, kMovieSql},
        {&movie_ids_stmt_, kMovieIdsSql}, {&tv_counts_stmt_, kTvCountsSql},
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

    ok_ = true;
    return true;
}

void WatchStore::close() {
    finalize_statements();
    db_.close();
    ok_ = false;
}

void WatchStore::upsert_position(const MediaRef& ref, int season, int episode,
                                 double position_s, double duration_s) {
    if (!ok_) {
        warn_once("not open");
        return;
    }
    sqlite3_stmt* s = upsert_stmt_;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, kind_text(ref.kind), -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, ref.id);
    sqlite3_bind_int(s, 3, season);
    sqlite3_bind_int(s, 4, episode);
    sqlite3_bind_double(s, 5, position_s);
    sqlite3_bind_double(s, 6, duration_s);
    sqlite3_bind_int(s, 7,
                     ui::is_watched_position(position_s, duration_s) ? 1 : 0);
    sqlite3_bind_int64(s, 8, static_cast<sqlite3_int64>(time(nullptr)));
    if (sqlite3_step(s) != SQLITE_DONE) {
        warn_once(std::string("upsert failed: ") +
                  sqlite3_errmsg(db_.handle()));
    }
}

void WatchStore::mark_watched(const MediaRef& ref, int season, int episode) {
    if (!ok_) {
        warn_once("not open");
        return;
    }
    sqlite3_stmt* s = mark_watched_stmt_;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, kind_text(ref.kind), -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, ref.id);
    sqlite3_bind_int(s, 3, season);
    sqlite3_bind_int(s, 4, episode);
    sqlite3_bind_int64(s, 5, static_cast<sqlite3_int64>(time(nullptr)));
    if (sqlite3_step(s) != SQLITE_DONE) {
        warn_once(std::string("mark_watched failed: ") +
                  sqlite3_errmsg(db_.handle()));
    }
}

ui::watch_map WatchStore::series_watch(int tmdb_id) {
    ui::watch_map out;
    if (!ok_) {
        warn_once("not open");
        return out;
    }
    sqlite3_stmt* s = series_stmt_;
    sqlite3_reset(s);
    sqlite3_bind_int(s, 1, tmdb_id);
    while (sqlite3_step(s) == SQLITE_ROW) {
        ui::WatchKey key{sqlite3_column_int(s, 0), sqlite3_column_int(s, 1)};
        ui::WatchRowLite row;
        row.position_s = sqlite3_column_double(s, 2);
        row.duration_s = sqlite3_column_double(s, 3);
        row.watched = sqlite3_column_int(s, 4) != 0;
        out[key] = row;
    }
    return out;
}

std::optional<WatchStore::Row> WatchStore::movie_watch(int tmdb_id) {
    if (!ok_) {
        warn_once("not open");
        return std::nullopt;
    }
    sqlite3_stmt* s = movie_stmt_;
    sqlite3_reset(s);
    sqlite3_bind_int(s, 1, tmdb_id);
    if (sqlite3_step(s) != SQLITE_ROW) return std::nullopt;
    Row row;
    row.season = sqlite3_column_int(s, 0);
    row.episode = sqlite3_column_int(s, 1);
    row.position_s = sqlite3_column_double(s, 2);
    row.duration_s = sqlite3_column_double(s, 3);
    row.watched = sqlite3_column_int(s, 4) != 0;
    // Reset now, not at the next call: a statement left sitting on
    // SQLITE_ROW keeps its read snapshot (and cursor) open. The other
    // readers all step through to done; match that hygiene here.
    sqlite3_reset(s);
    return row;
}

std::unordered_set<int> WatchStore::watched_movie_ids() {
    std::unordered_set<int> out;
    if (!ok_) {
        warn_once("not open");
        return out;
    }
    sqlite3_stmt* s = movie_ids_stmt_;
    sqlite3_reset(s);
    while (sqlite3_step(s) == SQLITE_ROW) {
        out.insert(sqlite3_column_int(s, 0));
    }
    return out;
}

std::unordered_map<int, int> WatchStore::tv_watched_counts() {
    std::unordered_map<int, int> out;
    if (!ok_) {
        warn_once("not open");
        return out;
    }
    sqlite3_stmt* s = tv_counts_stmt_;
    sqlite3_reset(s);
    while (sqlite3_step(s) == SQLITE_ROW) {
        out[sqlite3_column_int(s, 0)] = sqlite3_column_int(s, 1);
    }
    return out;
}

}  // namespace media_browser::library
