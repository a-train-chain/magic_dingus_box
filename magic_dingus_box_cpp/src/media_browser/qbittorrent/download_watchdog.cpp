#include "media_browser/qbittorrent/download_watchdog.h"

#include <algorithm>

namespace media_browser {

DownloadWatchdog::DownloadWatchdog(RadarrClient& radarr, QbittorrentClient& qbit)
    : radarr_(radarr), qbit_(qbit) {}

void DownloadWatchdog::watch(int tmdb_id, int radarr_movie_id,
                             std::string title, std::string qbit_hash) {
    // Replace any existing watch for this tmdb_id (e.g., user grabs again).
    unwatch(tmdb_id);
    watched_.push_back({tmdb_id, radarr_movie_id, std::move(title),
                        std::chrono::steady_clock::now(), std::move(qbit_hash)});
}

void DownloadWatchdog::unwatch(int tmdb_id) {
    watched_.erase(std::remove_if(watched_.begin(), watched_.end(),
                                  [tmdb_id](const auto& w){ return w.tmdb_id == tmdb_id; }),
                   watched_.end());
}

void DownloadWatchdog::snooze(int tmdb_id, std::chrono::seconds duration) {
    snoozed_until_[tmdb_id] = std::chrono::steady_clock::now() + duration;
}

std::vector<DownloadWatchdog::StallEvent>
DownloadWatchdog::evaluate(const Inputs& in) {
    std::vector<StallEvent> out;
    for (const auto& w : in.watched) {
        // Snooze check.
        auto sit = in.snoozed_until.find(w.tmdb_id);
        if (sit != in.snoozed_until.end() && in.now < sit->second) continue;

        // Radarr-failed has highest priority — explicit signal beats inference.
        bool radarr_failed = false;
        for (const auto& h : in.recent_history) {
            if (h.movie_id == w.tmdb_id && h.event_type == "downloadFailed") {
                radarr_failed = true;
                break;
            }
        }
        if (radarr_failed) {
            out.push_back({w.tmdb_id, w.radarr_movie_id, w.title,
                           Reason::RadarrFailed});
            continue;
        }

        // Zero-progress check (only after the 60s grace window).
        if (in.now - w.started_at < std::chrono::seconds(60)) continue;

        const QbitTorrent* t = nullptr;
        for (const auto& qt : in.qbit_torrents) {
            if (qt.hash == w.hash) { t = &qt; break; }
        }
        // No qBit torrent at all = Radarr never handed off.
        // OR torrent exists but no progress AND no peers connected = stall.
        if (!t || (t->progress == 0.0 && t->num_seeds == 0)) {
            out.push_back({w.tmdb_id, w.radarr_movie_id, w.title,
                           Reason::ZeroProgress});
        }
    }
    return out;
}

std::vector<DownloadWatchdog::StallEvent> DownloadWatchdog::tick() {
    auto now = std::chrono::steady_clock::now();
    if (now - last_poll_ < std::chrono::seconds(10)) return {};
    last_poll_ = now;

    Inputs in;
    in.now = now;
    in.watched = watched_;
    in.snoozed_until = snoozed_until_;
    in.qbit_torrents = qbit_.get_torrents();
    for (const auto& w : watched_) {
        auto hist = radarr_.get_history(w.tmdb_id, /*page_size=*/10);
        for (auto& h : hist) in.recent_history.push_back(std::move(h));
    }
    return evaluate(in);
}

}  // namespace media_browser
