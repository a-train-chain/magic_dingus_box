#pragma once

#include <string>

namespace media_browser {

class SonarrClient;
class QbittorrentClient;

// SeriesDetail's "Delete Season N…" sequence (CLAUDE.md "Confirm delete
// Season N flow (per-season orphan-proof cleanup)"), extracted from the
// screen's mutation worker so the abort rule, the AutoRedownloadGuard
// lifetime and the toast-disclosure rules are unit-tested rather than only
// compiled into the kiosk. Same arrangement as movie_remove.h. BLOCKING
// (a dozen+ sequential HTTP calls, plus up to ~15 s of guard-restore
// retries) — run it on a worker, never the render thread.
//
//   (a) Unmonitor the season, then its EPISODES (the two flags are
//       independent; SeasonSearch skips unmonitored episodes).
//   (b) Read the season's history — authoritative; nullopt aborts.
//   --  Arm the AutoRedownloadGuard (Sonarr's global autoRedownloadFailed
//       off) and hold it across (c)-(f). Not armed = abort.
//   (c) Cancel this season's live queue rows with blocklist=true.
//   (d) Mark the season's GRABBED history records failed (IMPORTED only
//       as a fallback when there is no grab record at all).
//   (e) Purge the season's torrents from qBittorrent with their data —
//       warn-and-continue.
//   (f) Delete the season's episode files from a FRESH listing — the only
//       step that cannot be undone, always last.
//   --  Restore the guard (BEFORE the caller takes any lock — it can spend
//       ~15 s retrying an HTTP PUT).
//
// Abort rule: any read or mutation in (a)-(d), or the (f) listing, that
// cannot get an authoritative answer stops the sequence before (f)'s delete
// runs. The guard is restored on EVERY exit path — normal, abort, and a
// throw out of either client (the optional<AutoRedownloadGuard> lives on
// this function's stack).
//
// qbit may be null (no qBittorrent on this box): stage (e) is skipped,
// exactly the whole-series remove's contract.

enum class SeasonDeleteStage {
    None,          // completed — nothing aborted
    Unmonitor,     // (a) season flag, episode listing, or episode flags
    History,       // (b)
    ArmGuard,      // couldn't switch Sonarr's auto-redownload off
    CancelQueue,   // (c) queue read or a cancel
    MarkFailed,    // (d)
    DeleteFiles,   // (f) fresh listing or the delete itself
};

struct SeasonDeleteInputs {
    // Series title as shown on screen; the toast's leading "<title>: ".
    std::string title;
    // How many files this season is KNOWN to have (the merged season row's
    // episode_file_count, read on the render thread that owns rows_). Stage
    // (f) needs it to tell "Sonarr listed no files" (a contradiction: the
    // delete row would not exist) apart from a genuinely file-less,
    // download-only season (legitimate; 0 here).
    int expected_files = 0;
};

struct SeasonDeleteOutcome {
    // Echoed from the call so compose_season_delete_toast is self-contained.
    std::string title;
    int season = 0;

    // True only when (f)'s delete succeeded.
    bool removed = false;
    // Where the run stopped (None when removed), and the short reason
    // ("Sonarr history unavailable") the abort toast leads with.
    SeasonDeleteStage abort_stage = SeasonDeleteStage::None;
    std::string abort_reason;

    // (c) Queue rows Sonarr confirmed cancelled. EXACT — a queue DELETE only
    // succeeds on a row that was really there — and every one has already
    // taken its partial download with it (removeFromClient=true).
    int cancelled = 0;
    // (d) History ids handed to mark_history_failed, and how many Sonarr
    // accepted. records_to_blocklist == 0 means neither a grabbed nor an
    // imported record existed: nothing was blocklisted, and the same copy
    // can be the answer to the next search.
    int records_to_blocklist = 0;
    int marked_failed = 0;
    // (e) torrents_purged is a GATE, never a number: qBit's delete_torrent
    // returns TRUE for a hash it doesn't have (no-op), so a torrent the
    // user already removed by hand still counts. Only ever test it > 0.
    int torrents_purged = 0;
    int torrents_left = 0;  // qBit refused; needs manual cleanup
    // (f) Episode-file ids handed to a successful delete_episode_files.
    int files_deleted = 0;

    // Guard state. armed: suppression was in force for (c)-(f).
    // restore_failed: the restore exhausted its retries — Sonarr's
    // auto-redownload is STILL OFF and the toast must say so (nothing else
    // in the kiosk surfaces this flag).
    bool guard_armed = false;
    bool redownload_restore_failed = false;
};

SeasonDeleteOutcome run_delete_season(SonarrClient& sonarr,
                                      QbittorrentClient* qbit,
                                      int series_id, int season,
                                      const SeasonDeleteInputs& in);

// The one place either verdict is worded. Pure — reads only the outcome.
//   abort:   "<title>: <reason>[ — N in-flight download(s) were already
//            cancelled…][; any torrents … removed] — season NOT deleted;
//            retry is safe[ (WARNING: … still switched OFF …)]"
//   success: "<title>: Season N removed — pick Season N in the list to
//            download it again[ (no release found to blocklist …)]
//            [ (a torrent needs manual cleanup …)][ (WARNING: …)]"
std::string compose_season_delete_toast(const SeasonDeleteOutcome& out);

}  // namespace media_browser
