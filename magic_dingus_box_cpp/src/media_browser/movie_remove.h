#pragma once

#include <string>

namespace media_browser {

class RadarrClient;
class QbittorrentClient;

// Detail's "Confirm Remove" sequence (CLAUDE.md "Confirm Remove flow"),
// extracted from DetailScreen::run_remove so its abort rule is unit-tested.
// BLOCKING (3+ sequential HTTP calls) — run it on a worker, never the
// render thread.
//
//   1. Cancel this movie's live Radarr queue rows (removeFromClient=true).
//   2. Walk Radarr's history and purge every torrent it ever grabbed for
//      the movie from qBittorrent, with data (finished+seeding torrents are
//      not in the queue, so step 1 cannot see them).
//   3. remove_movie(delete_files=true) — the ONLY step that cannot be
//      retried, so it is always last.
//
// Abort rule (the TV flow's, SeriesDetailScreen's whole-series remove):
// every READ that decides what is left to clean up must ANSWER before
// step 3 runs. The screen version used the bare get_queue() and
// get_movie_download_hashes(), which turn a failed read into "nothing to
// clean up" — and then deleted the library record, orphaning seeding
// torrents with nothing left to find them by. A failed read, or a failed
// cancel, now aborts with the record intact; retry is always safe because
// steps 1-2 are idempotent.
//
// qbit may be null (no qBittorrent client on this box): nothing can be
// purged, so the history read is skipped — the orphan risk is accepted BY
// CONSTRUCTION there, exactly as before.
struct MovieRemoveOutcome {
    bool removed = false;
    // User-facing reason when !removed. Discloses what already happened:
    // cancels use removeFromClient=true, so a later abort must not read as
    // "nothing was touched".
    std::string message;
    int cancelled = 0;  // queue rows cancelled in step 1
    int purged = 0;     // torrents qBittorrent confirmed deleted in step 2
};

MovieRemoveOutcome remove_movie_orphan_proof(RadarrClient& radarr,
                                             QbittorrentClient* qbit,
                                             int radarr_id);

}  // namespace media_browser
