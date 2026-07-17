#pragma once

#include <string>
#include <vector>
#include <optional>
#include <json/json.h>
#include "media_browser/radarr/radarr_types.h"

namespace media_browser {

class RadarrClient {
public:
    struct Config {
        std::string base_url = "http://localhost:7878";
        std::string api_key;
        // HTTP timeout for all Radarr requests. Kept short so a stalled
        // Radarr instance doesn't freeze the kiosk main render thread while
        // get_queue() / is_reachable() are in-flight.
        int timeout_secs = 5;
        // Path translation for movie files: Radarr returns container-internal
        // paths like /library/foo.mp4. The kiosk runs on the host and needs
        // /mnt/ssd/library/foo.mp4. Both prefixes are normalized to end in
        // '/' by the constructor (defense against /library2 false-matches).
        std::string container_library_prefix = "/library/";
        std::string host_library_prefix      = "/mnt/ssd/library/";
    };

    explicit RadarrClient(Config config);
    virtual ~RadarrClient();

    RadarrClient(const RadarrClient&) = delete;
    RadarrClient& operator=(const RadarrClient&) = delete;

    // Service health
    virtual bool is_reachable();
    virtual std::optional<SystemStatus> get_status();

    // Movie discovery
    virtual std::vector<MovieSearchHit> lookup(const std::string& query);
    virtual std::vector<Movie> get_library();
    virtual std::optional<Movie> get_movie(int radarr_id);

    // Library management
    virtual bool add_movie(int tmdb_id, int quality_profile_id, bool monitor = true);
    virtual bool remove_movie(int radarr_id, bool delete_files = false);
    virtual bool trigger_search(int radarr_id);

    // Queue / downloads
    virtual std::vector<QueueItem> get_queue();
    virtual bool cancel_queue_item(int queue_id);

    // Which movies Radarr is actively searching indexers for right now
    // (running MoviesSearch / MissingMoviesSearch commands). Powers the
    // "Searching indexers now…" state in the awaiting-release list.
    virtual ActiveSearches get_active_searches();

    // Grab a specific release picked by the user. The `release` JSON
    // must be an object previously returned from /api/v3/release?movieId=X
    // (or constructed with the same shape — at minimum guid + indexerId).
    virtual bool grab_release(const Json::Value& release);

    // Fetch /api/v3/release?movieId=X — the list of releases Radarr would
    // consider for an interactive search of this movie. Each entry has
    // guid, indexerId, title, seeders, size, quality, customFormatScore, etc.
    virtual std::vector<Json::Value> get_releases_for_movie(int radarr_movie_id);

    // Returns the distinct downloadId values (qBit info_hash) from
    // every "grabbed" event in this movie's Radarr history. Used by
    // DetailScreen::do_remove_confirm() to find torrents that may
    // still be seeding in qBittorrent for a movie the user is
    // deleting — the active-queue cancel path only catches downloads
    // currently in progress; finished+seeding torrents don't appear
    // there. Empty vector on error or when the movie has no history.
    // Hashes are returned in lowercase for direct comparison with
    // QbittorrentClient (which normalizes to lowercase internally).
    virtual std::vector<std::string> get_movie_download_hashes(int movie_id);

    struct HistoryEvent {
        int         id = 0;
        int         movie_id = 0;
        std::string event_type;       // grabbed, downloadFailed, downloadFolderImported, etc.
        std::string source_title;     // release title
        std::string date_iso;         // ISO 8601
    };

    // Recent history events for one movie. Returns most-recent-first.
    virtual std::vector<HistoryEvent> get_history(int radarr_movie_id, int page_size = 20);

    // Profiles
    virtual std::vector<QualityProfile> get_quality_profiles();
    virtual std::vector<RootFolder> get_root_folders();

    // Path translation for the movie file Radarr reports. Returns the host
    // path that GStreamer can open, given a container-internal path.
    // Unrecognized paths pass through unchanged with a warn-level log.
    std::string resolve_host_path(const std::string& container_path) const;

    // Trailing-slash normalization for path prefixes — exposed so main.cpp
    // can normalize env-var-supplied overrides at the same boundary the
    // constructor uses internally. Empty input passes through unchanged
    // (treated as "use default"); otherwise we ensure exactly one '/' at
    // the end. Defense against /library2/foo falsely matching /library.
    static std::string normalize_prefix(std::string s);

    // Diagnostics
    const std::string& last_error() const { return last_error_; }

protected:
    // Virtual for mocking (see radarr_mock.h)
    virtual std::string http_get(const std::string& path);
    virtual std::string http_post(const std::string& path, const std::string& body);
    virtual std::string http_delete(const std::string& path);

    // Long-timeout GET for endpoints that do synchronous work upstream
    // (notably /api/v3/release?movieId=X, which kicks off an interactive
    // search across every Prowlarr-synced indexer and can take 10-30s).
    // Other endpoints use http_get() with the default cfg_.timeout_secs
    // (5s) so a stalled Radarr doesn't freeze the UI on routine calls.
    virtual std::string http_get_long(const std::string& path, int timeout_secs);

    Config cfg_;
    std::string last_error_;
};

}  // namespace media_browser
