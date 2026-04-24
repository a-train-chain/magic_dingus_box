#pragma once

#include <chrono>
#include <string>
#include <vector>

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
//   - SETTINGS_MENU: returns to Screen::Browse.
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
    explicit QueueScreen(RadarrClient& radarr);

    void enter() override;
    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void update() override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

private:
    // Refresh cadence. The spec calls out ~2 seconds.
    static constexpr int kRefreshIntervalMs = 2000;

    // Two-stage cancel confirmation. Identical pattern to Detail's Remove.
    static constexpr int kCancelPendingMs = 2000;

    // Pull the queue from Radarr into queue_. Updates last_refresh_at_,
    // clamps cursor_, and records any error into last_error_.
    void refresh();

    // Cancel the focused row on the Radarr side, then refresh().
    void do_cancel_focused();

    RadarrClient& radarr_;

    std::vector<QueueItem> queue_;
    int cursor_ = 0;
    int scroll_row_ = 0;

    std::chrono::steady_clock::time_point last_refresh_at_{};
    bool refreshing_ = false;
    // Snapshotted at render time from radarr_.last_error() whenever the
    // queue comes back empty.
    std::string last_error_;

    // Cancel-confirmation state.
    bool cancel_pending_ = false;
    int cancel_pending_queue_id_ = 0;
    std::chrono::steady_clock::time_point cancel_pending_at_{};
};

}  // namespace media_browser::ui
