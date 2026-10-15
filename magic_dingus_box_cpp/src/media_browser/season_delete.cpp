#include "media_browser/season_delete.h"

#include <optional>
#include <vector>

#include <spdlog/spdlog.h>

#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/sonarr/sonarr_client.h"
#include "media_browser/ui/series_detail_logic.h"

namespace media_browser {

SeasonDeleteOutcome run_delete_season(SonarrClient& sonarr,
                                      QbittorrentClient* qbit,
                                      int series_id, int season,
                                      const SeasonDeleteInputs& in) {
    const int sid = series_id;
    SeasonDeleteOutcome out;
    out.title = in.title;
    out.season = season;

    // Engaged just before (c) and held across every destructive stage.
    // Declared HERE, above finish, so every exit path can put the flag back
    // BEFORE the caller words its toast (and before it takes any lock: the
    // restore can spend up to ~15 s retrying an HTTP PUT, and the render
    // thread blocks on the screen's mutation mutex in drain_mutation).
    // Idempotent via AutoRedownloadGuard::restore, so restoring here and
    // letting the destructor cover a throw never double-PUTs.
    std::optional<AutoRedownloadGuard> no_redownload;
    const auto restore_guard = [&]() {
        if (!no_redownload.has_value()) return;
        try {
            no_redownload->restore();
            out.redownload_restore_failed = no_redownload->restore_failed();
        } catch (const std::exception& e) {
            // A restore that threw has NOT confirmed the flag is back, so
            // warn as for a defeated one — a false alarm costs the owner a
            // look at one Sonarr setting; a missed one silently stops
            // auto-retry of failed downloads for every series. The guard's
            // destructor still gets its backstop attempt.
            spdlog::error("[SeriesDetail] auto-redownload restore threw: {}",
                          e.what());
            out.redownload_restore_failed = true;
        }
    };
    // Every abort goes through here: flag back FIRST, then record where and
    // why. The counters (cancelled / torrents_purged) are already on `out`,
    // so compose_season_delete_toast discloses what earlier stages destroyed
    // no matter which stage stopped us — two DIFFERENT stages can already
    // have destroyed data by the time any abort fires:
    //   (c) cancels carry removeFromClient=true, so every cancel that took
    //       has taken its partial download with it;
    //   (e) purges torrents WITH their downloaded copies.
    // These used to be two lambdas — a plain `fail` that named neither, and
    // a `fail_after_purge` that named only the second — and stage (d), which
    // runs AFTER (c) has already cancelled downloads, aborted through the
    // plain one saying "season NOT deleted; retry is safe" while the user's
    // partial data was gone. Composing from both counters means no stage can
    // pick the wrong message.
    const auto fail = [&](SeasonDeleteStage stage, const std::string& msg) {
        restore_guard();
        out.abort_stage = stage;
        out.abort_reason = msg;
        return out;
    };

    // A throw out of either client used to escape to spawn_mutation's
    // generic "something went wrong — try again", which named neither the
    // downloads stage (c) had already cancelled nor the torrents stage (e)
    // had already purged, and could not carry the stuck-OFF warning either
    // (the guard's destructor restored during unwinding, but its verdict
    // reached nobody). Route it through fail like every other abort; `at`
    // is the stage that was running.
    SeasonDeleteStage at = SeasonDeleteStage::Unmonitor;
    try {
        // (a) Unmonitor FIRST — season AND episodes.
        // Probe-verified (P3): the two flags are INDEPENDENT and SeasonSearch
        // skips unmonitored episodes, so a season-only unmonitor would leave the
        // season's episodes armed and make the "Download Season N" re-monitor
        // meaningless.
        //
        // This is NOT what stops the re-grab. P3 also concluded that
        // autoRedownloadFailed keys off the episode flag; hardware disproved
        // that on 2026-08-13 — Sonarr's redownload fires an EXPLICIT
        // EpisodeSearch by id, which ignores monitoring entirely, and it was
        // measured firing within 5 s with episode, season and series ALL
        // unmonitored. Suppression is the AutoRedownloadGuard below; stage (a)
        // is still required, just for the reason above rather than that one.
        if (!sonarr.set_season_monitored(sid, season, false))
            return fail(SeasonDeleteStage::Unmonitor,
                        "couldn't unmonitor Season " + std::to_string(season));
        const auto eps = sonarr.get_episodes_checked(sid);
        if (!eps.has_value())
            return fail(SeasonDeleteStage::Unmonitor, "couldn't list episodes");
        std::vector<int> season_ep_ids;
        for (const auto& e : *eps) {
            // id <= 0 is an unusable record — never PUT a guess
            // (fire_episode1_search's rule).
            if (e.season_number == season && e.id > 0)
                season_ep_ids.push_back(e.id);
        }
        // get_episodes_checked returns engaged-but-EMPTY for an unparseable body
        // or an unknown series id (documented misclassification,
        // sonarr_client.h) — and an empty id list makes set_episodes_monitored
        // short-circuit to true with NO HTTP, so the worker would sail on with
        // every episode still MONITORED. That is precisely the probe-P3 state
        // stage (a) exists to prevent: the season's episodes stay armed, so the
        // later "Download Season N" re-monitor has nothing to mean and a
        // SeasonSearch could act on them. The delete row is only reachable when
        // the picker just listed this season's episodes, so empty here
        // contradicts the row's own gate. Abort with only the SEASON flag
        // flipped — nothing destructive has run, and re-arming the season is one
        // press of "Download Season N".
        if (season_ep_ids.empty())
            return fail(SeasonDeleteStage::Unmonitor,
                        "couldn't list the season's episodes");
        if (!sonarr.set_episodes_monitored(season_ep_ids, false))
            return fail(SeasonDeleteStage::Unmonitor,
                        "couldn't unmonitor the season's episodes");

        at = SeasonDeleteStage::History;
        // (b) Season history — authoritative; nullopt aborts before anything
        // destructive.
        const auto hist = sonarr.get_season_history_checked(sid, season);
        if (!hist.has_value())
            return fail(SeasonDeleteStage::History, "Sonarr history unavailable");

        at = SeasonDeleteStage::ArmGuard;
        // SUPPRESS Sonarr's auto-redownload for the whole destructive window
        // (c)-(f). The owner's contract is "delete blocklists the release,
        // re-download is MANUAL"; stage (d)'s mark-failed breaks that on its
        // own, because Sonarr answers a DownloadFailedEvent with an EXPLICIT
        // EpisodeSearch by id that ignores every monitored flag stage (a) just
        // cleared. Observed live twice on 2026-08-13: a replacement grabbed 5 s
        // and 4 s after the delete, while the toast told the user to press a
        // button to download a season whose row already read `downloading`.
        //
        // Armed here rather than at (a): (a) and (b) cannot trigger a
        // redownload, and this flag is GLOBAL to SONARR (a separate config from
        // Radarr's — movies are unaffected) — no failed SERIES download gets an
        // automatic retry while it is held, so the window stays as short as the
        // work allows.
        //
        // Not armed = abort. Deleting the files without suppression IS the
        // shipped defect, so a guard that could not establish itself must stop
        // us here, with the season still on disk.
        no_redownload.emplace(sonarr);
        out.guard_armed = no_redownload->armed();
        if (!out.guard_armed)
            return fail(SeasonDeleteStage::ArmGuard,
                        "couldn't pause Sonarr's automatic re-download");

        at = SeasonDeleteStage::CancelQueue;
        // (c) Cancel this season's live queue rows WITH blocklist.
        // get_queue_CHECKED, not the bare wrapper: get_queue() collapses a
        // Sonarr outage to an empty vector, which reads as "nothing in flight"
        // and would walk straight into the file delete with a download still
        // running — the one answer that must abort is exactly the one the bare
        // form cannot give.
        const auto queue = sonarr.get_queue_checked();
        if (!queue.has_value())
            return fail(SeasonDeleteStage::CancelQueue,
                        "couldn't check for in-flight downloads");
        for (int qid : ui::cancel_ids_for_season(*queue, sid, season)) {
            if (sonarr.cancel_queue_item(qid, /*blocklist=*/true)) {
                ++out.cancelled;
                continue;
            }
            // The "N already cancelled, their data gone" clause is the toast
            // composer's job (it reads out.cancelled itself) — every LATER stage
            // owes the user the same disclosure, and duplicating it here is how
            // the two drifted apart in the first place.
            return fail(SeasonDeleteStage::CancelQueue,
                        "couldn't cancel an in-flight download");
        }

        at = SeasonDeleteStage::MarkFailed;
        // (d) Blocklist the release (mark-as-failed) so the same download is not
        // the answer to the next search. Safe here only because (a) unmonitored
        // the EPISODES.
        //
        // GRABBED ids are the target, not imported. Probe P2 verified
        // POST /history/failed/{id} against a GRABBED record; whether Sonarr
        // accepts an IMPORTED record's id is UNVERIFIED. imported_history_ids is
        // used only as a fallback when there is no grab record at all (a
        // manually-imported release, added without ever going through a grab).
        // Trying imported first — and aborting the loop on its first refusal, as
        // the abort-on-refusal rule below requires — would mean the one scenario
        // this fallback exists for (Sonarr rejecting an imported id) is exactly
        // the scenario where the loop never reaches the grabbed ids at all.
        // Grabbed first avoids that.
        //
        // The two vectors never share an id: parse_season_history
        // (sonarr_parsers.cpp) assigns each history record to exactly one of
        // them by eventType, and history ids are unique DB keys — no dedup
        // needed. Abort semantics unchanged: any refusal stops us before the
        // file delete.
        const std::vector<int>& to_fail = !hist->grabbed_history_ids.empty()
                                              ? hist->grabbed_history_ids
                                              : hist->imported_history_ids;
        out.records_to_blocklist = static_cast<int>(to_fail.size());
        for (int hid : to_fail) {
            if (!sonarr.mark_history_failed(hid))
                return fail(SeasonDeleteStage::MarkFailed,
                            "couldn't blocklist the downloaded release");
            ++out.marked_failed;
        }

        at = SeasonDeleteStage::PurgeTorrents;
        // (e) Purge the season's torrents. Warn-and-continue: the torrent may
        // already be gone, and the destructive file delete below is still
        // correct without it. Null qbit = the whole-series remove's contract,
        // skip.
        if (qbit != nullptr) {
            for (const auto& h : hist->download_hashes) {
                if (!qbit->delete_torrent(h, /*delete_files=*/true)) {
                    ++out.torrents_left;
                    spdlog::warn("[SeriesDetail] qbit delete failed for {}", h);
                    continue;
                }
                ++out.torrents_purged;
            }
        }
        // From here on the torrents above are gone WITH their downloaded data —
        // the abort toast says so on its own, from torrents_purged.

        at = SeasonDeleteStage::DeleteFiles;
        // (f) THE destructive step, LAST: this season's files, from a FRESH
        // authoritative listing (files can land between the picker's load and
        // now).
        const auto files = sonarr.get_episode_files_checked(sid);
        if (!files.has_value())
            return fail(SeasonDeleteStage::DeleteFiles,
                        "couldn't list episode files");
        std::vector<int> ids;
        for (const auto& f : *files) {
            if (f.season_number == season) ids.push_back(f.id);
        }
        // Same misclassification as stage (a), one step from the finish line:
        // an unparseable body reads as engaged-but-empty,
        // delete_episode_files({}) returns true with no HTTP by design, and the
        // success toast would say "Season N removed" while every file is still
        // on disk — after (e) has already destroyed the torrents and their
        // copies, so the user is told the season is gone AND has lost the
        // seeding data. The row requires files or a live download, so zero ids
        // against a known-nonzero count is a contradiction; zero against zero is
        // the legitimate download-only season and must still pass through.
        if (in.expected_files > 0 && ids.empty())
            return fail(SeasonDeleteStage::DeleteFiles,
                        "couldn't list the season's files");
        if (!sonarr.delete_episode_files(ids))
            return fail(SeasonDeleteStage::DeleteFiles,
                        "couldn't delete the season's files");
        out.files_deleted = static_cast<int>(ids.size());
        out.removed = true;
    } catch (const std::exception& e) {
        spdlog::warn("[SeriesDetail] season delete threw: {}", e.what());
        return fail(at, "something went wrong");
    }

    // Destructive work is done — put Sonarr's auto-redownload back now, so
    // the outcome carries the verdict and the caller never restores under
    // its own lock.
    restore_guard();
    return out;
}

std::string compose_season_delete_toast(const SeasonDeleteOutcome& out) {
    // Nothing else in the kiosk surfaces this Sonarr flag, so if the restore
    // lost, saying so here is the owner's only warning that their box has
    // stopped auto-retrying FAILED downloads for every SERIES, not just this
    // one (the flag is Sonarr-only; Radarr/movies are unaffected).
    const std::string warn = out.redownload_restore_failed
        ? " (WARNING: Sonarr's automatic re-download "
          "is still switched OFF \xE2\x80\x94 turn it "
          "back on in Sonarr under Settings > "
          "Download Clients)"
        : std::string();

    if (!out.removed) {
        std::string done;
        if (out.cancelled > 0) {
            done = " \xE2\x80\x94 " + std::to_string(out.cancelled) +
                   " in-flight download(s) were already "
                   "cancelled and their partial data "
                   "removed";
        }
        if (out.torrents_purged > 0) {
            done += (done.empty() ? std::string(" \xE2\x80\x94 any")
                                  : std::string("; any")) +
                    " torrents for this season and their "
                    "downloaded copies have already been "
                    "removed";
        }
        return out.title + ": " + out.abort_reason + done +
               " \xE2\x80\x94 season NOT deleted; retry is safe" + warn;
    }

    return out.title + ": Season " + std::to_string(out.season) +
           // Name the affordance the user is about to SEE. The action row's
           // "Download Season N" PROPOSES suggested_season (the season after
           // the viewer's progress), which need not be the one just deleted —
           // reaching it there takes the chooser. The season list's own row
           // targets exactly this season (selecting a season with nothing on
           // disk starts its download — see start_season_download), and the
           // season list is exactly where the screen's drain returns them.
           " removed \xE2\x80\x94 pick Season " + std::to_string(out.season) +
           " in the list to download it again" +
           // Both stage (d) and stage (e) had nothing to work with: no
           // grabbed record and no imported one means no release was
           // blocklisted and no torrent hash was known, so the same copy can
           // be the answer to the next search. Same treatment as the
           // torrents_left clause below — never imply a cleanup that did not
           // happen.
           (out.records_to_blocklist == 0
                ? " (no release found to blocklist \xE2\x80\x94 "
                  "the same copy could come back)"
                : "") +
           (out.torrents_left
                ? " (a torrent needs manual cleanup in "
                  "qBittorrent)"
                : "") +
           warn;
}

}  // namespace media_browser
