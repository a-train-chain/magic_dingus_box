#pragma once

#include <string>
#include <vector>
#include <optional>
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

    // Profiles
    virtual std::vector<QualityProfile> get_quality_profiles();
    virtual std::vector<RootFolder> get_root_folders();

    // Path translation for the movie file Radarr reports. Returns the host
    // path that GStreamer can open, given a container-internal path.
    // Unrecognized paths pass through unchanged with a warn-level log.
    std::string resolve_host_path(const std::string& container_path) const;

    // Diagnostics
    const std::string& last_error() const { return last_error_; }

protected:
    // Virtual for mocking (see radarr_mock.h)
    virtual std::string http_get(const std::string& path);
    virtual std::string http_post(const std::string& path, const std::string& body);
    virtual std::string http_delete(const std::string& path);

    Config cfg_;
    std::string last_error_;
};

}  // namespace media_browser
