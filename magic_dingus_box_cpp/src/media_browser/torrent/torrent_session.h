#pragma once

#include <string>
#include <vector>
#include <memory>

namespace libtorrent {
    struct session;
}

namespace media_browser {

struct TorrentStatus {
    std::string info_hash;      // hex
    std::string name;
    double progress = 0.0;      // 0.0 - 1.0
    int download_rate_bps = 0;
    int upload_rate_bps = 0;
    int num_peers = 0;
    int num_seeds = 0;
    std::string state;          // human-readable: "checking", "downloading", "seeding", "finished"
    bool is_finished = false;
    bool has_error = false;
    std::string error_message;
};

// Wraps a libtorrent session with a simplified API tuned for kiosk use.
// Non-thread-safe; call from a single owning thread. libtorrent internally
// uses its own worker threads for network I/O.
class TorrentSession {
public:
    struct Config {
        std::string download_dir;       // incomplete downloads
        std::string complete_dir;       // files moved here on finish
        int max_download_rate_bps = 0;  // 0 = unlimited
        int max_upload_rate_bps = 0;
        int active_downloads = 3;
    };

    explicit TorrentSession(Config config);
    ~TorrentSession();

    TorrentSession(const TorrentSession&) = delete;
    TorrentSession& operator=(const TorrentSession&) = delete;

    // Adds a magnet URI. Returns info_hash on success, empty string on failure.
    std::string add_magnet(const std::string& magnet_uri);

    // Adds a .torrent file on disk. Returns info_hash on success.
    std::string add_torrent_file(const std::string& torrent_file_path);

    // Lists all active torrents' status.
    std::vector<TorrentStatus> list();

    // Fetch a single torrent's status by info_hash (hex). Returns true if found.
    bool get_status(const std::string& info_hash, TorrentStatus& out);

    // Blocks until a torrent completes or `timeout_secs` elapses.
    // Returns true on clean completion.
    bool wait_for_completion(const std::string& info_hash, int timeout_secs);

    // Removes a torrent. If `delete_files` is true, also deletes partial files.
    bool remove(const std::string& info_hash, bool delete_files);

    // Process libtorrent alerts (call periodically; used internally by wait_*).
    void pump_alerts();

private:
    Config cfg_;
    std::unique_ptr<libtorrent::session> ses_;
};

}  // namespace media_browser
