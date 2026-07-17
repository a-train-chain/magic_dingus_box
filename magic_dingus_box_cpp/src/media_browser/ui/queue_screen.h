#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace media_browser { class QbittorrentClient; }

#include "media_browser/radarr/radarr_types.h"
#include "media_browser/ui/mb_screen.h"

namespace media_browser { class RadarrClient; }

namespace media_browser::ui {

// Task 21: the Queue screen — real implementation.
//
// Shows the live Radarr download queue as a vertical list of rows. Each
// row has a colored poster placeholder (deterministic per queue id), the
// movie title, its state / download rate / peer count, and a horizontal
// progress bar with a percentage overlay.
//
// Layout:
//   - Top bar (~56px): "Download Queue" title + item count + small
//     refresh indicator ("refreshing..." text while a get_queue() call
//     is in flight; last-refresh seconds-ago when idle).
//   - Main area: vertical list of queue entry rows, ~100px tall each,
//     scrolling when the list exceeds the visible area. Focused row
//     draws an accent outline.
//   - Bottom bar (~40px): action hints.
//
// Refresh:
//   - enter() does an immediate synchronous fetch.
//   - update() re-fetches every kRefreshIntervalMs (~2 seconds).
//
// Interaction:
//   - ROTATE_VERTICAL (dpad up/down): move cursor through rows.
//   - SELECT / ROTARY_CLICK: two-stage cancel of the focused row.
//     The button label flips to "Confirm Cancel" for kCancelPendingMs,
//     and a second SELECT within that window calls cancel_queue_item().
//     Any navigation or the 2s expiry clears the pending state.
//   - BTN2 (PLAY_PAUSE): back to Screen::Library. SETTINGS_MENU short-press
//     is a no-op in v1.6.x; long-press exits MB → MainMenu via the input
//     dispatcher.
//
// Error / empty states:
//   - Queue empty and RadarrClient::last_error() non-empty  ->
//     "Radarr service offline" with the error detail below.
//   - Queue empty and no error                              ->
//     "Queue is empty — no active downloads".
//
// Deferred:
//   - "Retry all failed": not directly mappable to the controller input
//     vocabulary today (no free SELECT-like binding). Revisit if a
//     second confirm binding lands.
class QueueScreen : public MbScreen {
public:
    // qbit is optional — pass nullptr to fall back to Radarr-cached
    // progress (useful for tests / dev machines without qBit).
    // When provided, QueueScreen overlays qBit's real-time progress
    // onto Radarr's queue items by matching downloadId → qBit hash,
    // eliminating the "frozen percent" gap caused by Radarr's 30-60s
    // internal poll cadence against qBit.
    explicit QueueScreen(RadarrClient& radarr,
                         QbittorrentClient* qbit = nullptr);
    ~QueueScreen();

    void enter() override;
    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void update() override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

private:
    // Refresh cadence. With the new async path the UI never blocks on
    // a refresh, so we can poll faster — 1.5s gives the user a
    // continuously-updating MB-downloaded counter without overloading
    // Radarr (which itself caches qBit's data internally).
    static constexpr int kRefreshIntervalMs = 1500;

    // Two-stage cancel confirmation. Identical pattern to Detail's Remove.
    static constexpr int kCancelPendingMs = 2000;

    // Kick off a background fetch of the queue + library. Returns
    // immediately. apply_pending_locked() picks up the result on a
    // future update() tick once the worker thread completes. Idempotent
    // when a refresh is already in flight (next interval will catch up).
    void refresh_async();

    // Worker thread body — runs the synchronous Radarr HTTP calls off
    // the render thread. Posts results into pending_ via result_mtx_.
    void run_refresh();

    // Drain pending_ into queue_/awaiting_ on the main thread. Called
    // from update() each frame; cheap when no result is ready.
    void apply_pending();

    // Cancel the focused row on the Radarr side, then refresh().
    void do_cancel_focused();

    RadarrClient&      radarr_;
    QbittorrentClient* qbit_ = nullptr;

    std::vector<QueueItem> queue_;

    // Movies in library that are monitored but have no file yet AND aren't
    // already in the active download queue. Populated by refresh() from
    // get_library() each cycle. Rendered as an "AWAITING RELEASE" section
    // below the active downloads so the user sees Radarr is watching for
    // a release even when nothing is grabbing yet.
    std::vector<Movie> awaiting_;
    // Which awaiting movies Radarr is actively searching indexers for
    // right now (snapshot from the last refresh). Drives the per-row
    // "Searching indexers now…" state vs the passive "awaiting release".
    ActiveSearches active_searches_;

    int cursor_ = 0;
    int scroll_row_ = 0;

    std::chrono::steady_clock::time_point last_refresh_at_{};
    bool refreshing_ = false;
    // Snapshotted at render time from radarr_.last_error() whenever the
    // queue comes back empty.
    std::string last_error_;
    // Mirrors PendingResult::qbit_overlay_failed for the most-recently-
    // applied refresh — used by render() to decide whether to surface a
    // "live data unavailable" indicator.
    bool qbit_overlay_failed_ = false;

    // --- Async refresh state ----------------------------------------
    // Pending result from the background worker. Worker writes under
    // result_mtx_; main thread reads + clears under the same lock in
    // apply_pending(). result_ready_ is the fast atomic check that
    // lets update() avoid taking the mutex on every frame when nothing
    // changed.
    struct PendingResult {
        std::vector<QueueItem> queue;
        std::vector<Movie>     awaiting;
        ActiveSearches         active_searches;
        std::string            error;
        // True when the qBit live-data overlay step couldn't fetch the
        // torrent list (qBit unreachable behind a netns flap, auth
        // desync, etc.). Radarr's queue snapshot is still returned and
        // shown, but its progress / dlspeed / peers fields are the
        // last values Radarr cached internally — typically 30-60 s
        // stale, sometimes much older. Surfaces in the UI as a yellow
        // sub-line so the user knows the bars aren't reflecting reality.
        bool                   qbit_overlay_failed = false;
    };
    std::mutex                 result_mtx_;
    PendingResult              pending_;
    std::atomic<bool>          result_ready_{false};
    std::atomic<bool>          refresh_in_flight_{false};
    std::thread                worker_;

    // Cancel-confirmation state.
    bool cancel_pending_ = false;
    int cancel_pending_queue_id_ = 0;
    std::chrono::steady_clock::time_point cancel_pending_at_{};
};

}  // namespace media_browser::ui
