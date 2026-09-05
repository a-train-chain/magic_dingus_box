#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace media_browser { class QbittorrentClient; }

#include "media_browser/radarr/radarr_types.h"
#include "media_browser/ui/mb_screen.h"
#include "media_browser/ui/queue_groups.h"

namespace media_browser { class RadarrClient; }
namespace media_browser { class SonarrClient; }

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
//   - ROTATE (rotary CW/CCW + D-pad left/right) and ROTATE_VERTICAL
//     (D-pad up/down): move cursor through rows. Both walk the same
//     list — the rotary encoder is the only cursor input on a
//     controller-free enclosure, so ROTATE must never be vertical-only.
//   - SELECT / ROTARY_CLICK: two-stage cancel of the focused row.
//     The button label flips to "Confirm Cancel" for kCancelPendingMs,
//     and a second SELECT within that window calls cancel_queue_item().
//     Any navigation or the 2s expiry clears the pending state.
//   - BTN2 (PLAY_PAUSE): back to Screen::Library. SETTINGS_MENU short-press
//     is a no-op in v1.6.x; long-press exits MB → MainMenu via the input
//     dispatcher.
//
// TV downloads (Sonarr):
//   - Optional. When a SonarrClient is wired, the same refresh worker
//     also fetches Sonarr's queue and collapses it with
//     group_tv_queue() — Sonarr's /queue is per EPISODE, so a season
//     pack is N rows sharing one downloadId and must render as ONE
//     row. The groups are appended AFTER the movie rows in the same
//     scrolling list, so one cursor walks both.
//   - Live progress comes from the SAME qBit overlay the movie rows
//     use: Sonarr's downloadId IS the torrent hash, lowercased to
//     match qBit's normalization exactly as Radarr's is.
//   - Cancelling a TV row issues exactly ONE
//     SonarrClient::cancel_queue_item() — that DELETE removes the
//     whole download and every sibling row 404s by design.
//   - The worker reads SonarrClient::get_queue_checked(), never the bare
//     get_queue() — nullopt (Sonarr didn't answer this cycle) and
//     "genuinely empty" must render differently. A Sonarr outage RATCHETS
//     tv_: the previously-shown groups are retained rather than replaced
//     with an empty list, so a routine Gluetun netns re-link cannot make
//     a live season pack read as "no active downloads." tv_unreachable_
//     tracks the current cycle's reachability (not ratcheted) and drives
//     the "Sonarr offline" warning line below.
//
// Error / empty states:
//   - Radarr side: RadarrClient::get_queue_checked() returned nullopt
//     -> "Radarr service offline" (centered, when nothing else is on
//     screen) or a small "Radarr offline — <detail>" warning line
//     stacked under the count/refresh sub-line (when TV rows keep the
//     combined list non-empty — this is keyed on the Radarr side alone,
//     independent of tv_).
//   - Sonarr side: tv_unreachable_ true and tv_ non-empty -> a small
//     "Sonarr offline — TV downloads may be out of date" warning line,
//     same stack. tv_unreachable_ true and tv_ EMPTY (nothing retained
//     to show) -> the centered empty-state swaps its "Add a movie…"
//     hint for the same warning rather than implying everything's fine.
//   - Nothing empty, nothing erroring -> "Queue is empty — no active
//     downloads".
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
    //
    // sonarr is optional too — nullptr keeps the screen movie-only,
    // which is exactly the behaviour every call site had before TV
    // downloads existed. main.cpp passes it only when Sonarr is
    // genuinely configured, so an unprovisioned box never shows the
    // SonarrMockClient's fixture pack as a real download.
    explicit QueueScreen(RadarrClient& radarr,
                         QbittorrentClient* qbit = nullptr,
                         SonarrClient* sonarr = nullptr);
    ~QueueScreen();

    void enter() override;
    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void update() override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

    // Pure cursor-navigation decision for the queue list, header-inline
    // so the unit tests can exercise it without linking this screen's
    // Renderer-dependent .cpp (same pattern as ReleasePickerScreen's
    // static helpers). handle_input() delegates to this — it is the
    // single source of truth for which actions walk the rows.
    struct CursorStep {
        bool is_nav = false;  // event was a cursor-walk action with delta != 0
        int  cursor = 0;      // resulting cursor position
    };
    //
    // `count` is the COMBINED row count (movie queue rows + TV download
    // groups) — the two sections are one list to the cursor.
    static CursorStep step_cursor(platform::InputAction action, int delta,
                                  int cursor, int count) {
        CursorStep r;
        r.cursor = cursor;
        if (action != platform::InputAction::ROTATE &&
            action != platform::InputAction::ROTATE_VERTICAL) {
            return r;
        }
        if (delta == 0) return r;
        r.is_nav = true;
        if (count <= 0) return r;  // consume, but nothing to move through
        r.cursor = std::clamp(cursor + delta, 0, count - 1);
        return r;
    }

    // An engaged empty result is a real, healthy answer and must never
    // inherit RadarrClient's shared stale error. Header-inline keeps the
    // decision testable without linking the Renderer-dependent .cpp file.
    static std::string radarr_queue_error(
        const std::optional<std::vector<QueueItem>>& queue_result) {
        return queue_result.has_value() ? std::string{}
                                        : std::string{"Queue request failed"};
    }

    // The inline warning is suppressed only when the exact centered-offline
    // state will render. Awaiting-release rows bypass that centered state, so
    // they still need the inline warning when Radarr's queue request failed.
    static bool show_inline_radarr_warning(bool has_error,
                                           bool movie_queue_empty,
                                           bool tv_queue_empty,
                                           bool awaiting_empty) {
        return has_error &&
               !(movie_queue_empty && tv_queue_empty && awaiting_empty);
    }

private:
    // One TV download as the screen renders it: the pure group plus the
    // live telemetry the qBit overlay supplies. The telemetry lives here
    // rather than on TvQueueGroup so queue_groups.h stays a pure,
    // Renderer-free, network-free grouping helper.
    struct TvQueueRow {
        TvQueueGroup group;      // size/sizeleft/status updated by the overlay
        double progress = 0.0;
        int download_rate_bps = 0;
        int peers = 0;
        int seeds = 0;
        int eta_seconds = 0;
    };

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

    // Cancel the focused row (Radarr for a movie row, Sonarr for a TV
    // row), then refresh().
    void do_cancel_focused();

    // Total selectable rows: movie queue rows first, TV groups after.
    // A cursor >= queue_.size() addresses tv_[cursor - queue_.size()].
    int row_count() const {
        return static_cast<int>(queue_.size() + tv_.size());
    }

    RadarrClient&      radarr_;
    QbittorrentClient* qbit_ = nullptr;
    SonarrClient*      sonarr_ = nullptr;

    std::vector<QueueItem> queue_;

    // TV downloads, one entry per Sonarr download (NOT per episode).
    // Always empty when sonarr_ is null.
    std::vector<TvQueueRow> tv_;

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
    // Non-empty only when the most recent checked queue request failed.
    std::string last_error_;
    // Mirrors PendingResult::qbit_overlay_failed for the most-recently-
    // applied refresh — used by render() to decide whether to surface a
    // "live data unavailable" indicator.
    bool qbit_overlay_failed_ = false;
    // True when sonarr_ is wired but the most recent refresh's
    // get_queue_checked() came back nullopt (transport/HTTP failure —
    // e.g. a Gluetun netns re-link mid-poll). NOT ratcheted like tv_:
    // this reflects THIS cycle's reachability, so it clears the instant
    // Sonarr answers again even if tv_ itself is still catching up.
    // render() reads it to decide whether to show the "Sonarr offline"
    // warning line (and to swap the empty-state copy when tv_ has
    // nothing retained to show alongside it).
    bool tv_unreachable_ = false;

    // --- Async refresh state ----------------------------------------
    // Pending result from the background worker. Worker writes under
    // result_mtx_; main thread reads + clears under the same lock in
    // apply_pending(). result_ready_ is the fast atomic check that
    // lets update() avoid taking the mutex on every frame when nothing
    // changed.
    struct PendingResult {
        std::vector<QueueItem> queue;
        std::vector<TvQueueRow> tv;
        // True when `tv` above is a genuine answer this cycle — either
        // sonarr_ is null (no TV configured; empty IS the answer) or
        // get_queue_checked() came back engaged (possibly empty, meaning
        // Sonarr genuinely has nothing queued). False means sonarr_ is
        // wired but did not answer this cycle: apply_pending() must NOT
        // overwrite the screen's tv_ with this (empty) `tv` — a Gluetun
        // netns re-link that outlives one 1.5s poll must not make a live
        // season pack vanish from the screen. See queue_groups.h for why
        // a partial/absent answer must never be rendered as "nothing is
        // downloading."
        bool                   tv_data_valid = false;
        // True when sonarr_ is wired and get_queue_checked() returned
        // nullopt this cycle. Always false when sonarr_ is null (TV
        // simply isn't configured, which is not an outage).
        bool                   tv_unreachable = false;
        std::vector<Movie>     awaiting;
        ActiveSearches         active_searches;
        std::string            error;
        // True when the qBit live-data overlay step couldn't fetch the
        // torrent list (qBit unreachable behind a netns flap, auth
        // desync, etc.). Radarr's / Sonarr's queue snapshots are still
        // returned and shown, but their progress / dlspeed / peers
        // fields are the last values the arr cached internally —
        // typically 30-60 s stale, sometimes much older. Surfaces in
        // the UI as a yellow sub-line so the user knows the bars aren't
        // reflecting reality.
        bool                   qbit_overlay_failed = false;
    };
    std::mutex                 result_mtx_;
    PendingResult              pending_;
    std::atomic<bool>          result_ready_{false};
    std::atomic<bool>          refresh_in_flight_{false};
    std::thread                worker_;

    // Library snapshot cache — WORKER-THREAD-ONLY (one refresh worker at
    // a time, serialized by refresh_in_flight_). See run_refresh: the
    // full library is the heaviest Radarr response and feeds only
    // slow-changing data, so it refreshes on a 30s TTL (or immediately
    // when the queue references a movie the snapshot doesn't know).
    std::vector<Movie> lib_cache_;
    std::chrono::steady_clock::time_point lib_cache_at_{};

    // Sonarr series-library snapshot cache — same contract as lib_cache_
    // above: WORKER-THREAD-ONLY, 30s TTL, and a queue group whose
    // series_id the snapshot doesn't know forces an immediate refetch (a
    // just-added series must get its poster + clean title right away, not
    // after the TTL). Feeds enrich_tv_groups() only, so it's fetched only
    // on cycles that actually have TV groups to enrich — there is no TV
    // "awaiting" section to keep warm. Untouched when sonarr_ is null
    // (tv groups can't exist then).
    std::vector<Series> tv_lib_cache_;
    std::chrono::steady_clock::time_point tv_lib_cache_at_{};

    // Cancel-confirmation state. The id alone is NOT a unique row key
    // across both sections — Radarr and Sonarr both hand out small
    // sequential queue ids, so movie row 5 and TV row 5 can coexist.
    // cancel_pending_is_tv_ disambiguates; without it, arming a movie
    // row would also paint the colliding TV row's bar red.
    bool cancel_pending_ = false;
    bool cancel_pending_is_tv_ = false;
    int cancel_pending_queue_id_ = 0;
    std::chrono::steady_clock::time_point cancel_pending_at_{};
};

}  // namespace media_browser::ui
