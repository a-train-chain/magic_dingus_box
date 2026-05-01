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
        // Radarr's internal movie id for the same movie — required so the
        // stall-event handler can deep-link straight into the
        // ReleasePickerScreen (which calls Radarr's /api/v3/release?
        // movieId=X endpoint, not /tmdb_id). Defaults to 0 for callers
        // that haven't been updated to forward it; the stall modal then
        // falls back to the legacy "open Detail screen" path.
        int         radarr_movie_id = 0;
        std::string title;
        TimePoint   started_at;
        std::string hash;       // qBit torrent hash; may be empty if not yet known
    };

    enum class Reason { ZeroProgress, RadarrFailed };

    struct StallEvent {
        int         tmdb_id = 0;
        // Mirrors WatchedDownload::radarr_movie_id — copied through by
        // evaluate() so the stall-modal handler in main.cpp can pick
        // the deep-link path without needing to re-query Radarr.
        int         radarr_movie_id = 0;
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

    // Register a watch on a movie download. radarr_movie_id is required
    // for the stall-modal "Pick another" deep-link (the picker calls
    // Radarr's /api/v3/release?movieId=X endpoint with this id); pass 0
    // when the radarr id isn't known yet (the stall modal will fall back
    // to opening Detail and the user can re-trigger Pick a source from
    // there). Title is used by the stall-modal headline only — a few
    // legacy callers pass an empty string.
    void watch(int tmdb_id, int radarr_movie_id, std::string title,
               std::string qbit_hash = "");
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
