#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "media_browser/sonarr/sonarr_types.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/ui/mb_screen.h"
#include "media_browser/ui/series_detail_logic.h"

namespace media_browser {
class SonarrClient;
class QbittorrentClient;
}

namespace media_browser::ui {

// TV series detail (Phase 2c-2): poster/overview + a paged season list with
// per-season state, and (Tasks 5-7) the season-at-a-time add flow, the
// whole-series blocking preflight, and orphan-proof remove.
//
// Deliberately Radarr-free: everything mutating is SonarrClient-shaped,
// the mirror image of DetailScreen being Radarr-shaped — the two screens
// share chrome helpers and idioms, never clients. All decisions live in
// series_detail_logic.h (pure, Mac-tested); this class is transport +
// paint.
class SeriesDetailScreen : public MbScreen {
public:
    SeriesDetailScreen(SonarrClient& sonarr, TmdbClient& tmdb,
                       QbittorrentClient* qbit, bool sonarr_configured);
    ~SeriesDetailScreen();

    // Same contract as DetailScreen::set_tmdb_id: no-op on the same id
    // (preserves loaded state on back-and-forth), refetch on a new one.
    void set_tmdb_id(int tmdb_id);
    int tmdb_id() const { return tmdb_id_; }

    // Where BTN4 returns to. Set by the dispatcher at transition time.
    void set_origin(Screen origin) { origin_ = origin; }
    Screen origin() const { return origin_; }

    void enter() override;
    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void update() override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

private:
    // ---- load pipeline (DetailScreen's FetchWorker idiom) ----
    struct FetchWorker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    // Everything a worker may publish. Declared complete in this task even
    // though Task 8's re-poll is what fills the last three fields — the
    // struct is the contract between every worker and apply_pending(), and
    // growing it later would mean editing the drain in two places.
    struct PendingLoad {
        // TMDB half
        bool tmdb_done = false;
        bool tmdb_ok = false;
        std::optional<TmdbTvDetail> detail;
        // Sonarr half
        bool sonarr_done = false;
        bool sonarr_ok = false;
        bool in_library = false;
        std::optional<Series> series;
        std::vector<QualityDefinition> quality_defs;
        // Has Sonarr ever actually refreshed this record? (record_refreshed)
        bool has_settled = false;
        bool settled = true;
        // Seasons with live queue activity (Task 8's poll).
        std::unordered_set<int> downloading;
        bool has_downloading = false;
    };

    void fetch();                       // spawns both workers under gen
    void run_tmdb_fetch(uint64_t gen, int tmdb_id,
                        std::shared_ptr<std::atomic<bool>> done);
    void run_sonarr_fetch(uint64_t gen, int tmdb_id,
                          std::shared_ptr<std::atomic<bool>> done);
    void reap_finished_workers();
    void apply_pending();               // render-thread drain
    void rebuild_rows();                // rows_ = merge_season_rows(...)

    // ---- action row (Tasks 5-7) ----
    // `Action` / `ActionButton` / `whole_series_label` and the row's whole
    // decision (decide_action_row) live in series_detail_logic.h: that
    // algebra is pure, it was the source of a wrong-button bug, and Mac
    // table tests cannot include this Renderer-bound header.
    void rebuild_buttons();             // thin caller of decide_action_row
    void dispatch_action(Action a);
    void expire_confirms();

    // ONE mutation at a time, on ONE reused worker thread (WatchdogSec=10:
    // add_series alone can take ~13.5 s — never on the render thread).
    // spawn_mutation joins the previous worker, wraps the body so mut_done_
    // flips on EVERY exit path including a throw, and catches
    // std::system_error from the thread ctor (a raw throw there is
    // std::terminate).
    void spawn_mutation(std::function<void()> body);
    void drain_mutation();

    // ~9s quiet re-poll while InLibrary: fresh per-season statistics +
    // queue-derived downloading set. Single reused worker; never flashes
    // Loading (writes land via pending_/apply_pending like the fetch).
    // BOTH priors are passed BY VALUE — reading in_library_ / sonarr_ok_
    // from the worker thread was an unsynchronized read of render-thread
    // state. poll_gen_ and last_poll_at_ already live in the Task-3 block.
    void maybe_repoll_series();
    void run_series_poll(uint64_t gen, int sonarr_id, bool prev_sonarr_ok,
                         bool prev_in_library);
    std::atomic<bool> poll_inflight_{false};
    static constexpr int kSeriesPollMs = 9000;
    std::thread poll_worker_;

    std::vector<ActionButton> buttons_;
    int focus_ = 0;

    // Confirm state — render-thread ONLY. No worker ever writes these: the
    // press-1 worker publishes a verdict and drain_mutation arms the button,
    // so the countdown starts when the LABEL appears, and there is no
    // unsynchronized cross-thread write to a member render() reads.
    bool whole_armed_ = false;
    std::chrono::steady_clock::time_point whole_armed_at_{};
    int64_t whole_estimate_bytes_ = 0;
    static constexpr int kWholeConfirmMs = 4000;
    bool remove_pending_ = false;
    std::chrono::steady_clock::time_point remove_pending_at_{};
    static constexpr int kRemovePendingMs = 2000;
    // Drain-set, consumed by handle_input's relay at the top (DetailScreen's
    // drain_remove_result idiom). CLEARED when consumed and in fetch() —
    // a latched flag would return origin_ on every frame forever.
    bool navigate_back_ = false;

    std::thread mut_worker_;
    std::atomic<bool> mut_in_flight_{false};
    std::atomic<bool> mut_done_{false};
    // Which series this mutation was started for. Render-thread only
    // (written in spawn_mutation, read in drain_mutation): the user can back
    // out mid-add and open a different show, and an outcome must never
    // rewrite THAT page's state.
    int mut_tmdb_id_ = 0;
    // Which LOAD this mutation was started against. Same thread discipline
    // as mut_tmdb_id_, and it closes the A→B→A hole that the id alone
    // cannot see: leaving series A, opening B, then coming BACK to A passes
    // an id-only gate, and the refetch's pre-mutation library snapshot then
    // clobbers the drain's result (the page shows "Add Season 1" for a
    // series that is now in the library, sticky until you leave again).
    uint64_t mut_fetch_gen_ = 0;
    std::mutex mut_mtx_;
    std::string mut_toast_;                          // guarded by mut_mtx_
    std::optional<Series> mut_series_;               // guarded
    bool mut_settled_ = true;                        // guarded
    bool mut_removed_ = false;                       // guarded
    bool mut_have_verdict_ = false;                  // guarded
    DiskVerdict mut_verdict_ = DiskVerdict::Block;   // guarded
    int64_t mut_estimate_ = 0;                       // guarded

    SonarrClient& sonarr_;
    TmdbClient& tmdb_;
    QbittorrentClient* qbit_;           // Task 7 (remove); may be null
    const bool sonarr_configured_;

    int tmdb_id_ = 0;
    bool needs_refresh_ = true;
    Screen origin_ = Screen::Browse;

    // Authoritative render-thread state (apply_pending / drain_mutation only).
    std::optional<TmdbTvDetail> detail_;
    std::optional<Series> series_;
    std::vector<SeasonRow> rows_;
    std::unordered_set<int> downloading_seasons_;  // fed by Task 8
    double mb_per_min_ = 70.0;
    bool tmdb_done_ = false, tmdb_ok_ = false;
    bool sonarr_done_ = false, sonarr_ok_ = false, in_library_ = false;
    // False for the window where Sonarr holds the record but has never
    // refreshed it: seasons[] is empty and EVERY row reads unmonitored, so
    // the add controls must not be offered (they would say "Download
    // Season 1" one second after adding Season 1).
    bool series_settled_ = true;

    // Season-list paging. BTN1/BTN3 move pages; render() recomputes the page
    // count each frame from the space actually left after the reserved
    // action row + indicator row, and clamps season_page_ into range.
    int season_page_ = 0;
    int season_page_count_ = 1;

    std::atomic<uint64_t> fetch_gen_{0};
    // Task 8's quiet re-poll publishes into pending_ as well, so its
    // generation must be invalidated by fetch() and the dtor. Declared here
    // so that discipline is in place from the file's first version.
    std::atomic<uint64_t> poll_gen_{0};
    std::chrono::steady_clock::time_point last_poll_at_{};
    std::mutex pending_mtx_;
    PendingLoad pending_;
    std::atomic<bool> pending_ready_{false};
    std::vector<FetchWorker> workers_;
};

}  // namespace media_browser::ui
