#include "media_browser/movie_remove.h"

#include <spdlog/spdlog.h>

#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/radarr/radarr_client.h"

namespace media_browser {

MovieRemoveOutcome remove_movie_orphan_proof(RadarrClient& radarr,
                                             QbittorrentClient* qbit,
                                             int radarr_id) {
    MovieRemoveOutcome out;
    // What step 1 already did, for every abort message after it: cancels
    // use removeFromClient=true, so those downloads' data is gone even
    // though the movie itself was not removed.
    const auto cancelled_prefix = [&out]() -> std::string {
        return out.cancelled > 0
            ? "Cancelled " + std::to_string(out.cancelled) +
                  " download(s), then "
            : std::string();
    };

    // Step 1: live queue rows. CHECKED read — a failed read is NOT an
    // empty queue, and proceeding on it would leave an in-flight torrent
    // with no Radarr record to cancel it by.
    const auto queue = radarr.get_queue_checked();
    if (!queue.has_value()) {
        out.message = "Couldn't check for in-flight downloads \xE2\x80\x94 "
                      "movie NOT removed; retry is safe";
        return out;
    }
    int cancel_failed = 0;
    std::string cancel_err;
    for (const auto& q : *queue) {
        if (q.movie_id != radarr_id) continue;
        if (radarr.cancel_queue_item(q.id)) {
            ++out.cancelled;
            continue;
        }
        ++cancel_failed;
        // Captured AT the failing call: a later success would clear the
        // shared last_error() and the abort would lose its reason.
        if (cancel_err.empty()) cancel_err = radarr.last_error();
        spdlog::warn("[movie_remove] failed to cancel queue item {} for "
                     "movie {}: {}", q.id, radarr_id, cancel_err);
    }
    if (out.cancelled > 0) {
        spdlog::info("[movie_remove] cancelled {} in-flight queue item(s) "
                     "before removing movie {}", out.cancelled, radarr_id);
    }
    if (cancel_failed > 0) {
        // Orphan-torrent state is worse than a "not removed" banner; the
        // user can fix qBittorrent connectivity and retry.
        out.message = (out.cancelled > 0
                           ? cancelled_prefix() + "failed on " +
                                 std::to_string(cancel_failed) + " more"
                           : "Couldn't cancel " +
                                 std::to_string(cancel_failed) +
                                 " in-flight download(s)") +
                      " \xE2\x80\x94 movie NOT removed; retry is safe" +
                      (cancel_err.empty() ? std::string()
                                          : " (" + cancel_err + ")");
        return out;
    }

    // Step 2: every torrent Radarr's history remembers for this movie,
    // purged with data. CHECKED read for the same reason as step 1 — and
    // here the stakes are higher: a finished, seeding torrent has no queue
    // row, so the history is the ONLY record that can find it.
    if (qbit != nullptr) {
        const auto hashes = radarr.get_movie_download_hashes_checked(radarr_id);
        if (!hashes.has_value()) {
            out.message = (out.cancelled > 0
                               ? cancelled_prefix() + "couldn't check"
                               : std::string("Couldn't check")) +
                          " for seeding torrents \xE2\x80\x94 movie NOT "
                          "removed; retry is safe";
            return out;
        }
        for (const auto& h : *hashes) {
            // Warn-and-continue: qBit's delete is a no-op for a hash it no
            // longer has, and one stuck torrent must not keep the movie.
            if (qbit->delete_torrent(h, /*delete_files=*/true)) {
                ++out.purged;
            } else {
                spdlog::warn("[movie_remove] qbit delete failed for {}", h);
            }
        }
        if (!hashes->empty()) {
            spdlog::info("[movie_remove] purged {} of {} historical qBit "
                         "torrent(s) for movie {}",
                         out.purged, hashes->size(), radarr_id);
        }
    }

    // Step 3: the record and its library file — last, because it is the
    // one step a retry cannot undo.
    if (!radarr.remove_movie(radarr_id, /*delete_files=*/true)) {
        out.message = "Remove failed: " + radarr.last_error();
        return out;
    }
    out.removed = true;
    return out;
}

}  // namespace media_browser
