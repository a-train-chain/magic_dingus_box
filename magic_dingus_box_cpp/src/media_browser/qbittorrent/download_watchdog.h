#pragma once

// DownloadWatchdog — surfaces stall events when Radarr's auto-pick or
// the chosen torrent isn't making progress.
//
// Two consumers:
//   1. Add to Library / Pick a source flow registers the movie's tmdb_id
//      via watch().
//   2. Main loop calls tick() once per ~10s; if the watchdog detects
//      stall conditions it returns StallEvent records that surface as
//      a non-blocking modal prompting the user to pick a different
//      release.
//
// Test surface: evaluate() is a pure function over Inputs so the
// stall-condition logic is testable without live qBit or Radarr.

#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/radarr/radarr_client.h"

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace media_browser {

class DownloadWatchdog {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct WatchedDownload {
        int         tmdb_id = 0;
        std::string title;
        TimePoint   started_at;
        std::string hash;       // qBit torrent hash; may be empty if not yet known
    };

    enum class Reason { ZeroProgress, RadarrFailed };

    struct StallEvent {
        int         tmdb_id = 0;
        std::string title;
        Reason      reason  = Reason::ZeroProgress;
    };

    // Pure-functional input bundle. Tests build these directly; the live
    // tick() builds them from qBit + Radarr.
    struct Inputs {
        TimePoint                                  now;
        std::vector<WatchedDownload>               watched;
        std::vector<QbitTorrent>                   qbit_torrents;
        std::vector<RadarrClient::HistoryEvent>    recent_history;
        std::map<int, TimePoint>                   snoozed_until;
    };

    static std::vector<StallEvent> evaluate(const Inputs& in);

    // Live-instance API (wraps real services).
    DownloadWatchdog(RadarrClient& radarr, QbittorrentClient& qbit);

    void watch(int tmdb_id, std::string title, std::string qbit_hash = "");
    void unwatch(int tmdb_id);
    void snooze(int tmdb_id, std::chrono::seconds duration);

    // Called from the main loop once per second. Internally rate-limits
    // service polls to ~once per 10s; returns any new stall events to
    // surface to the user.
    std::vector<StallEvent> tick();

private:
    RadarrClient&        radarr_;
    QbittorrentClient&   qbit_;
    std::vector<WatchedDownload>  watched_;
    std::map<int, TimePoint>      snoozed_until_;
    TimePoint                     last_poll_{};
};

}  // namespace media_browser
