#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace media_browser {

// Live torrent stats from qBittorrent's /api/v2/torrents/info endpoint.
// Subset of fields — only what QueueScreen needs to override Radarr's
// stale per-torrent metrics. Hash is the lookup key (Radarr exposes the
// same value as `downloadId` on its queue items).
struct QbitTorrent {
    std::string hash;             // info_hash, lowercase hex
    std::string name;
    double      progress = 0.0;   // 0.0 - 1.0
    int64_t     size = 0;         // total bytes
    int64_t     downloaded = 0;   // total bytes received
    int         dlspeed = 0;      // bytes/sec
    int         upspeed = 0;      // bytes/sec
    int         num_seeds = 0;    // connected seeders
    int         num_leechs = 0;   // connected peers
    int         num_complete = 0; // total seeders in swarm
    int         num_incomplete = 0; // total leechers in swarm
    int         eta_seconds = 0;
    std::string state;            // "downloading", "stalledDL", "uploading", etc.
};

// Minimal qBittorrent v2 API client. Used by QueueScreen to get
// real-time progress data — Radarr's queue API caches qBit data on a
// 30-60s refresh cycle, which is the cause of the user-visible
// "frozen percentage that suddenly jumps" UX. Going direct to qBit
// gives us per-second granularity without waiting on Radarr's
// internal poll.
//
// Threading: get_torrents() is intended to be called from a single
// background worker thread (the QueueScreen refresh worker). Cookie
// state is mutex-protected so the auto-relogin path doesn't race
// against itself if the worker is canceled and respawned.
class QbittorrentClient {
public:
    struct Config {
        // Default matches the docker-compose service published port
        // (qBit's web UI is exposed via Gluetun's network namespace
        // at localhost:8080 on the Pi host).
        std::string base_url = "http://localhost:8080";

        // Default qBit credentials. Overridden via constructor or env
        // var (MDB_QBIT_USER / MDB_QBIT_PASS) for non-default setups.
        std::string username = "admin";
        std::string password = "adminadmin";

        // Hard timeout for a single HTTP call. qBit's /api/v2/torrents/
        // info typically returns in <100ms; 5s is generous, ensures
        // a stuck qBit instance doesn't pin the worker forever.
        int timeout_secs = 5;
    };

    explicit QbittorrentClient(Config cfg);
    ~QbittorrentClient();

    QbittorrentClient(const QbittorrentClient&) = delete;
    QbittorrentClient& operator=(const QbittorrentClient&) = delete;

    // Returns all torrents currently tracked by qBit. Empty on failure
    // (last_error() carries detail). Auto-logs-in lazily; if a prior
    // cookie expired, the call recovers transparently with one extra
    // round-trip.
    virtual std::vector<QbitTorrent> get_torrents();

    // Convenience: same as get_torrents() but indexed by lowercase
    // hash for O(1) lookup against Radarr's downloadId field.
    std::unordered_map<std::string, QbitTorrent> get_torrents_by_hash();

    // Single-torrent lookup by hash. Returns nullopt if not found.
    // Used by DownloadWatchdog to poll one specific download per tick
    // without re-fetching the whole torrent list.
    virtual std::optional<QbitTorrent> get_torrent(const std::string& hash);

    // Delete a torrent from qBittorrent by info_hash. When delete_files
    // is true, the partial-or-completed download files on disk are
    // also removed (qBit's "Delete torrent and files" action). Used by
    // DetailScreen::do_remove_confirm() to purge orphan torrents that
    // a movie's Radarr-side remove wouldn't catch — specifically
    // torrents that finished downloading and are now seeding (no
    // longer in Radarr's active queue, but still pinning disk +
    // upload bandwidth).
    //
    // Hash matching is case-insensitive — the caller can pass the
    // value straight from Radarr's downloadId without normalizing.
    // No-op (returns true) if no torrent with that hash exists.
    virtual bool delete_torrent(const std::string& hash, bool delete_files);

    // Pause/resume ALL torrents — used by PlaybackScreen to halt
    // disk-write contention during movie playback. The library lives
    // on the same medium qBit writes torrent pieces to (typically a
    // single USB flash drive on the Pi), so concurrent read+write
    // hammers the drive's random-IO ceiling and makes playback hitch
    // / scrubbing freeze. Pausing torrents during playback gives the
    // GStreamer reader exclusive disk access.
    //
    // Tries qBit's modern /torrents/stop (5.x) first; falls back to
    // /torrents/pause (legacy 4.x) if the modern endpoint 404s. We
    // run an `Up` qBit (5.x via linuxserver.io image) but the
    // fallback keeps this resilient against future container
    // downgrades. Returns true if AT LEAST ONE endpoint succeeded.
    //
    // Best-effort: a failure here must NOT block playback — the
    // user-visible operation is "play movie", not "manage torrents".
    // Caller logs and proceeds either way.
    virtual bool pause_all();
    virtual bool resume_all();

    // Diagnostics
    const std::string& last_error() const { return last_error_; }

protected:
    // Virtual seam for tests (mock client).
    virtual std::string http_get(const std::string& path);
    virtual std::string http_post(const std::string& path,
                                  const std::string& body);

    // Authenticate against qBit. Stores the SID cookie for subsequent
    // calls. Returns true on success.
    bool login_locked();

    Config cfg_;

private:
    std::mutex  cookie_mtx_;
    std::string sid_cookie_;     // contents of "Set-Cookie: SID=..."
    std::string last_error_;
};

}  // namespace media_browser
