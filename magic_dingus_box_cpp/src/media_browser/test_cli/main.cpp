// test_media_browser — standalone CLI for exercising the media browser
// subsystems end-to-end on real hardware. Not linked into the kiosk app.
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "media_browser/library/library_db.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/torrent/torrent_session.h"

namespace fs = std::filesystem;

namespace {

struct Config {
    std::string db_path = "data/media_browser.db";
    std::string download_dir = "data/downloads/incomplete";
    std::string complete_dir = "data/library";
    std::string tmdb_api_key;
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
        "\n"
        "Environment:\n"
        "  MDB_TMDB_API_KEY   TMDB v3 API key (or ~/.config/magic_dingus_box/tmdb_api_key).\n"
        "  MDB_DB_PATH        Override DB path (default: data/media_browser.db).\n"
        "  MDB_DOWNLOAD_DIR   Override incomplete dir (default: data/downloads/incomplete).\n"
        "  MDB_COMPLETE_DIR   Override complete dir (default: data/library).\n");
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

    spdlog::error("Unknown command: {}", cmd);
    print_help();
    return 2;
}

// --- db commands (implemented in Task 8) -----------------------------
namespace {
int cmd_db_init(const Config&) { spdlog::error("not yet implemented"); return 3; }
int cmd_db_status(const Config&) { spdlog::error("not yet implemented"); return 3; }

// --- tmdb commands (implemented in Task 9) ---------------------------
int cmd_tmdb_search(const Config&, const std::string&) { spdlog::error("not yet implemented"); return 3; }
int cmd_tmdb_get(const Config&, int) { spdlog::error("not yet implemented"); return 3; }

// --- torrent commands (implemented in Task 10) -----------------------
int cmd_torrent_add_magnet(const Config&, const std::string&) { spdlog::error("not yet implemented"); return 3; }
int cmd_torrent_add_file(const Config&, const std::string&) { spdlog::error("not yet implemented"); return 3; }
int cmd_torrent_status(const Config&) { spdlog::error("not yet implemented"); return 3; }
int cmd_torrent_wait(const Config&, const std::string&, int) { spdlog::error("not yet implemented"); return 3; }
int cmd_torrent_remove(const Config&, const std::string&) { spdlog::error("not yet implemented"); return 3; }
}  // namespace
