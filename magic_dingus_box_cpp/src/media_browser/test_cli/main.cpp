// test_media_browser — standalone CLI for exercising the media browser
// subsystems end-to-end on real hardware. Not linked into the kiosk app.
#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "media_browser/library/library_db.h"
#include "media_browser/radarr/radarr_client.h"
#include "media_browser/sonarr/sonarr_client.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/torrent/torrent_session.h"

namespace fs = std::filesystem;

namespace {

struct Config {
    std::string db_path = "data/media_browser.db";
    std::string download_dir = "data/downloads/incomplete";
    std::string complete_dir = "data/library";
    std::string tmdb_api_key;
    std::string radarr_api_key;
    std::string radarr_base_url = "http://localhost:7878";
    std::string sonarr_api_key;
    std::string sonarr_base_url = "http://localhost:8989";
};

std::string read_file_trimmed(const fs::path& p) {
    if (!fs::exists(p)) return {};
    std::ifstream f(p);
    std::string line;
    std::getline(f, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
        line.pop_back();
    return line;
}

Config load_config() {
    Config c;
    if (const char* k = std::getenv("MDB_TMDB_API_KEY")) {
        c.tmdb_api_key = k;
    } else {
        const char* home = std::getenv("HOME");
        if (home) {
            fs::path p = fs::path(home) / ".config/magic_dingus_box/tmdb_api_key";
            c.tmdb_api_key = read_file_trimmed(p);
        }
    }
    if (const char* d = std::getenv("MDB_DB_PATH")) c.db_path = d;
    if (const char* d = std::getenv("MDB_DOWNLOAD_DIR")) c.download_dir = d;
    if (const char* d = std::getenv("MDB_COMPLETE_DIR")) c.complete_dir = d;
    if (const char* k = std::getenv("MDB_RADARR_API_KEY")) c.radarr_api_key = k;
    if (const char* u = std::getenv("MDB_RADARR_BASE_URL")) c.radarr_base_url = u;
    if (const char* k = std::getenv("MDB_SONARR_API_KEY")) c.sonarr_api_key = k;
    if (const char* u = std::getenv("MDB_SONARR_BASE_URL")) c.sonarr_base_url = u;
    return c;
}

void print_help() {
    std::fprintf(stderr,
        "test_media_browser - Media Browser Phase 1 CLI\n"
        "\n"
        "Usage: test_media_browser <command> [args...]\n"
        "\n"
        "Commands:\n"
        "  help                         Show this help.\n"
        "  db-init                      Create/upgrade the SQLite database.\n"
        "  db-status                    Show DB schema version and row counts.\n"
        "  tmdb-search <query>          Search TMDB for a movie title.\n"
        "  tmdb-get <tmdb_id>           Fetch full movie detail.\n"
        "  torrent-add-magnet <uri>     Add a magnet URI to the session.\n"
        "  torrent-add-file <path>      Add a .torrent file to the session.\n"
        "  torrent-status               Show status of all active torrents.\n"
        "  torrent-wait <hash> [secs]   Block until torrent completes (default 600s).\n"
        "  torrent-remove <hash>        Remove torrent (keeps files).\n"
        "  radarr-status                Ping Radarr, show version + reachability.\n"
        "  radarr-search <query>        Radarr /movie/lookup.\n"
        "  radarr-library               Show all movies in library.\n"
        "  radarr-queue                 Show active download queue.\n"
        "  radarr-add <tmdb_id>         Add movie to library (triggers search).\n"
        "  radarr-profiles              List quality profiles.\n"
        "  sonarr-status                Ping Sonarr, show version + reachability.\n"
        "  sonarr-lookup <tmdb_id>      Sonarr /series/lookup?term=tmdb:<id>.\n"
        "  sonarr-library               Show all series in library (per-season state).\n"
        "  sonarr-queue                 Show the per-episode download queue.\n"
        "\n"
        "Environment:\n"
        "  MDB_TMDB_API_KEY   TMDB v3 API key (or ~/.config/magic_dingus_box/tmdb_api_key).\n"
        "  MDB_DB_PATH        Override DB path (default: data/media_browser.db).\n"
        "  MDB_DOWNLOAD_DIR   Override incomplete dir (default: data/downloads/incomplete).\n"
        "  MDB_COMPLETE_DIR   Override complete dir (default: data/library).\n"
        "  MDB_RADARR_API_KEY Radarr v3 API key (required for radarr-* commands).\n"
        "  MDB_RADARR_BASE_URL Radarr base URL (default: http://localhost:7878).\n"
        "  MDB_SONARR_API_KEY Sonarr v4 API key (required for sonarr-* commands).\n"
        "  MDB_SONARR_BASE_URL Sonarr base URL (default: http://localhost:8989).\n");
}

// Forward declarations for dispatch (implemented in Tasks 8-10).
int cmd_db_init(const Config& c);
int cmd_db_status(const Config& c);
int cmd_tmdb_search(const Config& c, const std::string& query);
int cmd_tmdb_get(const Config& c, int tmdb_id);
int cmd_torrent_add_magnet(const Config& c, const std::string& uri);
int cmd_torrent_add_file(const Config& c, const std::string& path);
int cmd_torrent_status(const Config& c);
int cmd_torrent_wait(const Config& c, const std::string& hash, int secs);
int cmd_torrent_remove(const Config& c, const std::string& hash);
int cmd_radarr_status(const Config& c);
int cmd_radarr_search(const Config& c, const std::string& query);
int cmd_radarr_library(const Config& c);
int cmd_radarr_queue(const Config& c);
int cmd_radarr_add(const Config& c, int tmdb_id);
int cmd_radarr_profiles(const Config& c);
int cmd_sonarr_status(const Config& c);
int cmd_sonarr_lookup(const Config& c, int tmdb_id);
int cmd_sonarr_library(const Config& c);
int cmd_sonarr_queue(const Config& c);

}  // namespace

int main(int argc, char** argv) {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%^%l%$] %v");

    if (argc < 2) {
        print_help();
        return 2;
    }

    Config cfg = load_config();
    std::string cmd = argv[1];

    if (cmd == "help" || cmd == "-h" || cmd == "--help") {
        print_help();
        return 0;
    }
    if (cmd == "db-init") return cmd_db_init(cfg);
    if (cmd == "db-status") return cmd_db_status(cfg);
    if (cmd == "tmdb-search") {
        if (argc < 3) { print_help(); return 2; }
        return cmd_tmdb_search(cfg, argv[2]);
    }
    if (cmd == "tmdb-get") {
        if (argc < 3) { print_help(); return 2; }
        return cmd_tmdb_get(cfg, std::atoi(argv[2]));
    }
    if (cmd == "torrent-add-magnet") {
        if (argc < 3) { print_help(); return 2; }
        return cmd_torrent_add_magnet(cfg, argv[2]);
    }
    if (cmd == "torrent-add-file") {
        if (argc < 3) { print_help(); return 2; }
        return cmd_torrent_add_file(cfg, argv[2]);
    }
    if (cmd == "torrent-status") return cmd_torrent_status(cfg);
    if (cmd == "torrent-wait") {
        if (argc < 3) { print_help(); return 2; }
        int secs = (argc >= 4) ? std::atoi(argv[3]) : 600;
        return cmd_torrent_wait(cfg, argv[2], secs);
    }
    if (cmd == "torrent-remove") {
        if (argc < 3) { print_help(); return 2; }
        return cmd_torrent_remove(cfg, argv[2]);
    }
    if (cmd == "radarr-status") return cmd_radarr_status(cfg);
    if (cmd == "radarr-search") {
        if (argc < 3) { print_help(); return 2; }
        return cmd_radarr_search(cfg, argv[2]);
    }
    if (cmd == "radarr-library") return cmd_radarr_library(cfg);
    if (cmd == "radarr-queue") return cmd_radarr_queue(cfg);
    if (cmd == "radarr-add") {
        if (argc < 3) { print_help(); return 2; }
        return cmd_radarr_add(cfg, std::atoi(argv[2]));
    }
    if (cmd == "radarr-profiles") return cmd_radarr_profiles(cfg);
    if (cmd == "sonarr-status") return cmd_sonarr_status(cfg);
    if (cmd == "sonarr-lookup") {
        if (argc < 3) { print_help(); return 2; }
        return cmd_sonarr_lookup(cfg, std::atoi(argv[2]));
    }
    if (cmd == "sonarr-library") return cmd_sonarr_library(cfg);
    if (cmd == "sonarr-queue") return cmd_sonarr_queue(cfg);

    spdlog::error("Unknown command: {}", cmd);
    print_help();
    return 2;
}

namespace {

// --- db commands -----------------------------------------------------

int cmd_db_init(const Config& c) {
    auto parent = fs::path(c.db_path).parent_path();
    if (!parent.empty()) fs::create_directories(parent);
    media_browser::LibraryDb db;
    if (!db.open(c.db_path)) {
        spdlog::error("failed to open {}", c.db_path);
        return 1;
    }
    if (!db.run_migrations()) {
        spdlog::error("migrations failed");
        return 1;
    }
    spdlog::info("DB ready at {} (schema v{})", c.db_path, db.schema_version());
    return 0;
}

int cmd_db_status(const Config& c) {
    media_browser::LibraryDb db;
    if (!db.open(c.db_path)) {
        spdlog::error("failed to open {}", c.db_path);
        return 1;
    }
    spdlog::info("DB: {}", c.db_path);
    spdlog::info("  schema version: {}", db.schema_version());

    sqlite3* h = db.handle();
    auto count = [&](const char* sql) -> int {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(h, sql, -1, &st, nullptr) != SQLITE_OK) return -1;
        int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
        sqlite3_finalize(st);
        return n;
    };
    spdlog::info("  titles: {}", count("SELECT COUNT(*) FROM titles;"));
    spdlog::info("  queue entries: {}", count("SELECT COUNT(*) FROM queue;"));
    spdlog::info("  history rows: {}", count("SELECT COUNT(*) FROM history;"));
    return 0;
}

// --- tmdb commands ---------------------------------------------------

int cmd_tmdb_search(const Config& c, const std::string& query) {
    if (c.tmdb_api_key.empty()) {
        spdlog::error("no TMDB API key - set MDB_TMDB_API_KEY or write "
                      "~/.config/magic_dingus_box/tmdb_api_key");
        return 1;
    }
    media_browser::TmdbClient client(c.tmdb_api_key);
    auto hits = client.search_movie(query);
    if (hits.empty()) {
        spdlog::warn("no results (or error: {})", client.last_error());
        return 0;
    }
    spdlog::info("{} results for \"{}\":", hits.size(), query);
    for (const auto& h : hits) {
        spdlog::info("  [{:>7}] {} ({})  rating={:.1f}",
                     h.tmdb_id, h.title, h.year, h.rating);
    }
    return 0;
}

int cmd_tmdb_get(const Config& c, int tmdb_id) {
    if (c.tmdb_api_key.empty()) {
        spdlog::error("no TMDB API key - set MDB_TMDB_API_KEY or write "
                      "~/.config/magic_dingus_box/tmdb_api_key");
        return 1;
    }
    media_browser::TmdbClient client(c.tmdb_api_key);
    auto detail = client.get_movie(tmdb_id);
    if (!detail) {
        spdlog::error("fetch failed: {}", client.last_error());
        return 1;
    }
    spdlog::info("{} ({})", detail->title, detail->year);
    spdlog::info("  tmdb_id:  {}", detail->tmdb_id);
    spdlog::info("  runtime:  {} min", detail->runtime_minutes);
    spdlog::info("  rating:   {:.1f}", detail->rating);
    spdlog::info("  poster:   {}", detail->poster_path);
    spdlog::info("  backdrop: {}", detail->backdrop_path);
    spdlog::info("  overview: {}", detail->overview);
    return 0;
}

// --- torrent commands ------------------------------------------------

media_browser::TorrentSession::Config make_torrent_config(const Config& c) {
    media_browser::TorrentSession::Config tc;
    tc.download_dir = c.download_dir;
    tc.complete_dir = c.complete_dir;
    return tc;
}

void print_status(const media_browser::TorrentStatus& s) {
    spdlog::info("  [{}] {}", s.info_hash.substr(0, 8), s.name);
    spdlog::info("    state={} progress={:.1f}% peers={} seeds={}",
                 s.state, s.progress * 100.0, s.num_peers, s.num_seeds);
    spdlog::info("    down={}KB/s up={}KB/s",
                 s.download_rate_bps / 1024, s.upload_rate_bps / 1024);
    if (s.has_error) spdlog::error("    ERROR: {}", s.error_message);
}

// Re-adds any .torrent files left in the download dir so status/wait
// commands see prior-run torrents. libtorrent writes .torrent resume data
// alongside downloads; this gives us per-invocation session continuity
// without a persistent daemon (Phase 3 orchestrator concern).
void reattach_existing_torrents(media_browser::TorrentSession& ses, const Config& c) {
    if (!fs::exists(c.download_dir)) return;
    for (const auto& entry : fs::directory_iterator(c.download_dir)) {
        if (entry.path().extension() == ".torrent") {
            ses.add_torrent_file(entry.path().string());
        }
    }
}

int cmd_torrent_add_magnet(const Config& c, const std::string& uri) {
    media_browser::TorrentSession ses(make_torrent_config(c));
    auto hash = ses.add_magnet(uri);
    if (hash.empty()) return 1;
    spdlog::info("added torrent: {}", hash);
    spdlog::info("(CLI is stateless per-invocation; run torrent-wait for progress)");
    // Pump for 3 seconds so the session fetches metadata before exit.
    for (int i = 0; i < 3; ++i) {
        ses.pump_alerts();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}

int cmd_torrent_add_file(const Config& c, const std::string& path) {
    media_browser::TorrentSession ses(make_torrent_config(c));
    auto hash = ses.add_torrent_file(path);
    if (hash.empty()) return 1;
    spdlog::info("added torrent: {}", hash);
    for (int i = 0; i < 3; ++i) {
        ses.pump_alerts();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}

int cmd_torrent_status(const Config& c) {
    media_browser::TorrentSession ses(make_torrent_config(c));
    reattach_existing_torrents(ses, c);
    ses.pump_alerts();
    auto list = ses.list();
    if (list.empty()) {
        spdlog::info("no active torrents");
        return 0;
    }
    for (const auto& s : list) print_status(s);
    return 0;
}

int cmd_torrent_wait(const Config& c, const std::string& hash, int secs) {
    media_browser::TorrentSession ses(make_torrent_config(c));
    reattach_existing_torrents(ses, c);
    spdlog::info("waiting for {} (timeout {}s)...", hash, secs);
    auto started = std::chrono::steady_clock::now();
    std::thread progress_thread([&]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            media_browser::TorrentStatus s;
            if (ses.get_status(hash, s)) {
                spdlog::info("  progress: {:.1f}% ({} KB/s, {} peers)",
                    s.progress * 100.0, s.download_rate_bps / 1024, s.num_peers);
                if (s.is_finished || s.has_error) return;
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - started).count();
            if (elapsed > secs) return;
        }
    });
    bool ok = ses.wait_for_completion(hash, secs);
    progress_thread.join();
    if (ok) {
        spdlog::info("DONE");
        return 0;
    }
    spdlog::error("did not complete in time");
    return 1;
}

int cmd_torrent_remove(const Config& c, const std::string& hash) {
    media_browser::TorrentSession ses(make_torrent_config(c));
    reattach_existing_torrents(ses, c);
    bool ok = ses.remove(hash, /*delete_files=*/false);
    return ok ? 0 : 1;
}

// --- radarr commands -------------------------------------------------

media_browser::RadarrClient::Config make_radarr_config(const Config& c) {
    media_browser::RadarrClient::Config rc;
    rc.base_url = c.radarr_base_url;
    rc.api_key = c.radarr_api_key;
    return rc;
}

int cmd_radarr_status(const Config& c) {
    if (c.radarr_api_key.empty()) {
        spdlog::error("no Radarr API key - set MDB_RADARR_API_KEY");
        return 1;
    }
    media_browser::RadarrClient r(make_radarr_config(c));
    auto status = r.get_status();
    if (!status) {
        spdlog::error("fetch failed: {}", r.last_error());
        return 1;
    }
    spdlog::info("Radarr: {} (reachable: true)", status->version);
    return 0;
}

int cmd_radarr_search(const Config& c, const std::string& query) {
    if (c.radarr_api_key.empty()) {
        spdlog::error("no Radarr API key - set MDB_RADARR_API_KEY");
        return 1;
    }
    media_browser::RadarrClient r(make_radarr_config(c));
    auto hits = r.lookup(query);
    spdlog::info("{} results for \"{}\":", hits.size(), query);
    for (const auto& h : hits) {
        spdlog::info("  [{:>7}] {} ({}) rating={:.1f}",
                     h.tmdb_id, h.title, h.year, h.rating);
    }
    return 0;
}

int cmd_radarr_library(const Config& c) {
    if (c.radarr_api_key.empty()) {
        spdlog::error("no Radarr API key - set MDB_RADARR_API_KEY");
        return 1;
    }
    media_browser::RadarrClient r(make_radarr_config(c));
    auto lib = r.get_library();
    spdlog::info("Library: {} movies", lib.size());
    for (const auto& m : lib) {
        spdlog::info("  [{:>5}] {} ({})  have_file={}  quality={}",
                     m.radarr_id, m.title, m.year, m.has_file, m.file_quality);
    }
    return 0;
}

int cmd_radarr_queue(const Config& c) {
    if (c.radarr_api_key.empty()) {
        spdlog::error("no Radarr API key - set MDB_RADARR_API_KEY");
        return 1;
    }
    media_browser::RadarrClient r(make_radarr_config(c));
    auto q = r.get_queue();
    spdlog::info("Queue: {} items", q.size());
    for (const auto& it : q) {
        spdlog::info("  [{:>5}] {} {:.1f}% state={}", it.id, it.title,
                     it.progress * 100.0, it.state);
    }
    return 0;
}

int cmd_radarr_add(const Config& c, int tmdb_id) {
    if (c.radarr_api_key.empty()) {
        spdlog::error("no Radarr API key - set MDB_RADARR_API_KEY");
        return 1;
    }
    media_browser::RadarrClient r(make_radarr_config(c));
    auto profiles = r.get_quality_profiles();
    if (profiles.empty()) {
        spdlog::error("no quality profiles in Radarr — set up in web UI first");
        return 1;
    }
    // Prefer Radarr's built-in "HD-1080p" profile (id 4 on a fresh install).
    // Fall back to a custom "1080p Standard" if someone seeded one,
    // then to the first profile as a last resort.
    int qp = profiles[0].id;
    bool found = false;
    for (const auto& p : profiles) {
        if (p.name == "HD-1080p") { qp = p.id; found = true; break; }
    }
    if (!found) {
        for (const auto& p : profiles) {
            if (p.name == "1080p Standard") { qp = p.id; break; }
        }
    }
    bool ok = r.add_movie(tmdb_id, qp, /*monitor=*/true);
    if (ok) spdlog::info("added tmdb_id={} with quality profile {}", tmdb_id, qp);
    else spdlog::error("failed: {}", r.last_error());
    return ok ? 0 : 1;
}

int cmd_radarr_profiles(const Config& c) {
    if (c.radarr_api_key.empty()) {
        spdlog::error("no Radarr API key - set MDB_RADARR_API_KEY");
        return 1;
    }
    media_browser::RadarrClient r(make_radarr_config(c));
    auto p = r.get_quality_profiles();
    for (const auto& q : p) {
        spdlog::info("  [{:>3}] {}", q.id, q.name);
    }
    return 0;
}

// --- sonarr commands -------------------------------------------------

media_browser::SonarrClient::Config make_sonarr_config(const Config& c) {
    media_browser::SonarrClient::Config sc;
    sc.base_url = c.sonarr_base_url;
    sc.api_key = c.sonarr_api_key;
    return sc;
}

int cmd_sonarr_status(const Config& c) {
    if (c.sonarr_api_key.empty()) {
        spdlog::error("no Sonarr API key - set MDB_SONARR_API_KEY");
        return 1;
    }
    media_browser::SonarrClient s(make_sonarr_config(c));
    auto status = s.get_status();
    if (!status) {
        spdlog::error("fetch failed: {}", s.last_error());
        return 1;
    }
    spdlog::info("Sonarr: {} (reachable: true)", status->version);
    auto profiles = s.get_quality_profiles();
    for (const auto& p : profiles) {
        spdlog::info("  profile [{:>3}] {}", p.id, p.name);
    }
    for (const auto& r : s.get_root_folders()) {
        spdlog::info("  root    [{:>3}] {}  free={} GB",
                     r.id, r.path, r.free_space_bytes / 1'000'000'000);
    }
    return 0;
}

int cmd_sonarr_lookup(const Config& c, int tmdb_id) {
    if (c.sonarr_api_key.empty()) {
        spdlog::error("no Sonarr API key - set MDB_SONARR_API_KEY");
        return 1;
    }
    media_browser::SonarrClient s(make_sonarr_config(c));
    auto hits = s.lookup_by_tmdb(tmdb_id);
    if (hits.empty()) {
        spdlog::error("no results for tmdb:{} ({})", tmdb_id, s.last_error());
        return 1;
    }
    spdlog::info("{} result(s) for tmdb:{}:", hits.size(), tmdb_id);
    for (const auto& h : hits) {
        spdlog::info("  {} ({})  tvdb={} tmdb={} runtime={}min status={} seasons={}",
                     h.title, h.year, h.tvdb_id, h.tmdb_id,
                     h.runtime_minutes, h.status, h.seasons.size());
        for (const auto& season : h.seasons) {
            spdlog::info("     S{:02d} monitored={} episodes={}",
                         season.season_number, season.monitored,
                         season.episode_count);
        }
    }
    return 0;
}

int cmd_sonarr_library(const Config& c) {
    if (c.sonarr_api_key.empty()) {
        spdlog::error("no Sonarr API key - set MDB_SONARR_API_KEY");
        return 1;
    }
    media_browser::SonarrClient s(make_sonarr_config(c));
    auto checked = s.get_library_checked();
    if (!checked) {
        spdlog::error("library fetch failed: {}", s.last_error());
        return 1;
    }
    spdlog::info("Library: {} series", checked->size());
    for (const auto& series : *checked) {
        spdlog::info("  [{:>5}] {} ({})  files={} size={} GB  path={}",
                     series.sonarr_id, series.title, series.year,
                     series.episode_file_count,
                     series.size_on_disk_bytes / 1'000'000'000, series.path);
        for (const auto& season : series.seasons) {
            spdlog::info("     S{:02d} monitored={} files={}/{}",
                         season.season_number, season.monitored,
                         season.episode_file_count, season.episode_count);
        }
    }
    return 0;
}

int cmd_sonarr_queue(const Config& c) {
    if (c.sonarr_api_key.empty()) {
        spdlog::error("no Sonarr API key - set MDB_SONARR_API_KEY");
        return 1;
    }
    media_browser::SonarrClient s(make_sonarr_config(c));
    auto q = s.get_queue();
    // Per-episode records, deliberately ungrouped — a season pack shows N
    // rows sharing one downloadId. Phase 2c collapses them for display.
    spdlog::info("Queue: {} episode record(s)", q.size());
    for (const auto& it : q) {
        spdlog::info("  [{:>5}] S{:02d}E{:02d} {} {:.1f}% state={} dl={}",
                     it.id, it.season_number, it.episode.episode_number,
                     it.episode.title.empty() ? it.title : it.episode.title,
                     it.progress * 100.0, it.state, it.download_id);
    }
    return 0;
}

}  // namespace
