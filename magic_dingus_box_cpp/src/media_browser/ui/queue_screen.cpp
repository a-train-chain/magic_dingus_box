#include "media_browser/ui/queue_screen.h"
#include "media_browser/ui/worker_pool.h"
#include "media_browser/ui/mb_chrome.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/radarr/radarr_client.h"
#include "media_browser/sonarr/sonarr_client.h"
#include "media_browser/tmdb_image.h"
#include "media_browser/ui/mb_ui_utils.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "ui/toast.h"

namespace media_browser::ui {

namespace {

// QueueScreen owns body geometry only; the top-of-screen chrome (the
// 6-tab Marquee strip + "Queue" title) is delegated to
// `chrome::draw_screen_header`, which returns a `header_bottom` Y the body
// anchors against. Below that we draw a count + refresh-status sub-line
// in the dim-cream small-font idiom Library uses, then the body proper:
//   - Active queue: borderless rows with a small poster, title + state /
//     rate / ETA sub-line, and an outline-only progress bar (green for
//     downloading, gold for queued/paused, red for failed). Focused row
//     gets a 3px gold outline + blinking right-edge marker.
//   - AWAITING RELEASE section below: monitored library entries with no
//     file yet, separated from the queue by a steel-blue rule.
//   - Footer hints come from `chrome::draw_footer_hints` so the bordered
//     key glyphs match every other Marquee screen.

// kPaddingX matches chrome::kSafeInset_px (60) so queue rows stay inside
// the 40 px wood-frame bezel that surrounds every Marquee screen.
constexpr float kPaddingX        = 60.0f;

// Row geometry. 100px is tall enough for poster + 2 lines of metadata +
// a progress bar without being so tall that fewer than ~5 rows fit on a
// 720p display.
constexpr float kRowHeight           = 100.0f;
constexpr float kRowGap              = 12.0f;
constexpr float kRowInnerPadding     = 12.0f;
constexpr float kPosterW             = 70.0f;
// Poster height intentionally leaves kRowInnerPadding worth of inset on
// top + bottom so the focused row's gold outline (~3px) doesn't clip
// through the poster's edges. Width stays the same; mb_draw_poster_fit
// letterboxes if the poster's native aspect doesn't match.
constexpr float kPosterH             = kRowHeight - 2.0f * kRowInnerPadding;
constexpr float kPosterBorderW       = 1.0f;
constexpr float kProgressBarW        = 300.0f;
constexpr float kProgressBarH        = 14.0f;
constexpr float kProgressBorderW     = 2.0f;
constexpr float kRowOutlineW         = 3.0f;   // gold focus outline
constexpr float kRowDimOutlineW      = 1.0f;   // unfocused row outline

// Confirm-cancel CTA box (replaces the ◂ marker on the focused row when
// the cancel is armed). Outlined-only, red — same border-and-text idiom as
// the genre chips on Detail.
constexpr float kCancelBoxH      = 30.0f;
constexpr float kCancelBoxPadX   = 12.0f;
constexpr float kCancelBoxBorder = 2.0f;

// Footer hint geometry — centered, dim. No background bar; matches
// DetailScreen's bottom-hint styling exactly.
constexpr float kFooterMarginY   = 12.0f;

}  // namespace

QueueScreen::QueueScreen(RadarrClient& radarr, QbittorrentClient* qbit,
                         SonarrClient* sonarr)
    : radarr_(radarr), qbit_(qbit), sonarr_(sonarr) {}

QueueScreen::~QueueScreen() {
    // Wait for any in-flight worker before destruction so we don't
    // leave a thread holding references to a dying QueueScreen.
    if (worker_.joinable()) worker_.join();
    if (cancel_worker_.joinable()) cancel_worker_.join();
}

void QueueScreen::enter() {
    cursor_ = 0;
    scroll_row_ = 0;
    cancel_pending_ = false;
    cancel_pending_is_tv_ = false;
    cancel_pending_queue_id_ = 0;
    // Async — no UI block on entry. The screen renders immediately
    // (showing whatever stale state we had from last visit, or a
    // "Loading..." centered if first ever visit), and apply_pending()
    // drops the new data in on a future update() tick when the worker
    // finishes (~50-500 ms typical, up to 5s if Radarr is slow).
    refresh_async();
}

void QueueScreen::refresh_async() {
    // Atomic CAS — only one worker thread runs at a time. If a previous
    // refresh is still in flight, we no-op; the next update() tick
    // will trigger another check after this one publishes.
    bool expected = false;
    if (!refresh_in_flight_.compare_exchange_strong(expected, true)) {
        return;
    }
    refreshing_ = true;

    // Join any prior worker that finished but wasn't yet reaped. This
    // is fast (the thread has already exited) and bounds our worker
    // accumulation at one.
    if (worker_.joinable()) worker_.join();

    try {
        worker_ = std::thread([this] {
            run_guarded("queue refresh", [this] { run_refresh(); });
        });
    } catch (const std::system_error& e) {
        // An uncaught throw from the thread ctor is std::terminate. Release
        // the CAS so the next update() tick simply tries again.
        spdlog::warn("[QueueScreen] refresh spawn failed: {}", e.what());
        refresh_in_flight_.store(false, std::memory_order_release);
    }
}

void QueueScreen::run_refresh() {
    // Clears refresh_in_flight_ on EVERY exit path: run_guarded swallows a
    // throw, and a latched flag silently stopped every later refresh — the
    // queue froze on its last snapshot for the rest of the session.
    struct InflightGuard {
        std::atomic<bool>& flag;
        ~InflightGuard() { flag.store(false, std::memory_order_release); }
    } inflight_guard{refresh_in_flight_};
    PendingResult r;
    auto queue_checked = radarr_.get_queue_checked();
    r.error = radarr_queue_error(queue_checked);
    if (queue_checked) r.queue = std::move(*queue_checked);

    // TV downloads. Worker thread only (WatchdogSec budget) — same rule
    // the Radarr calls above follow. Sonarr's queue is per EPISODE, so a
    // season pack arrives as N rows sharing one downloadId; group_tv_queue
    // collapses them to one row per download BEFORE anything renders.
    //
    // get_queue_checked(), NOT get_queue() — the checked shape is the whole
    // point of this fix. Sonarr rides Gluetun's netns exactly like Radarr
    // does, so a routine re-link can fail this call mid-refresh; {} and
    // "Sonarr is unreachable" must not collapse to the same on-screen state,
    // or a live season pack reads as "no downloads" every time the tunnel
    // blips (see sonarr_client.h's doc comment on get_queue_checked).
    if (sonarr_) {
        auto tv_checked = sonarr_->get_queue_checked();
        if (!tv_checked) {
            // Sonarr did not answer this cycle. Leave r.tv empty and
            // r.tv_data_valid false — apply_pending() reads tv_data_valid
            // and RATCHETS: it retains whatever tv_ already held instead
            // of overwriting it with this empty result. r.tv_unreachable
            // drives the "Sonarr offline" warning line independent of
            // whether anything was retained to show alongside it.
            r.tv_unreachable = true;
        } else {
            r.tv_data_valid = true;
            const auto& tv_rows = *tv_checked;
            // Sonarr's per-row ETA, keyed by the row id the group kept.
            // Used only when the qBit overlay can't supply a live one.
            auto tv_groups = group_tv_queue(tv_rows);

            // Poster + clean-title enrichment. Sonarr's /queue carries no
            // images and no series title (queue_groups.h), so cross-ref
            // the series library — the exact mirror of the Radarr library
            // cross-ref below that fills movie posters. Same cache
            // contract too: tv_lib_cache_* are worker-thread-only
            // (serialized by refresh_in_flight_), 30s TTL, and a group
            // whose series_id the snapshot doesn't know forces an
            // immediate refetch so a just-added series shows its poster
            // right away. Only fetched when there are groups to enrich —
            // unlike the movie library there is no TV awaiting section,
            // so an empty TV queue needs no snapshot at all.
            if (!tv_groups.empty()) {
                const auto tv_now = std::chrono::steady_clock::now();
                // Aged out, or a group names a series the snapshot does
                // not know (tv_lib_cache_stale).
                const bool tv_lib_stale = tv_lib_cache_stale(
                    (tv_now - tv_lib_cache_at_) > std::chrono::seconds(30),
                    tv_lib_cache_, tv_groups);
                if (tv_lib_stale) {
                    // CHECKED: a failed read keeps the previous snapshot
                    // (stale titles/posters beat blank ones) and leaves the
                    // timestamp alone so the next tick retries.
                    if (auto lib = sonarr_->get_library_checked()) {
                        tv_lib_cache_ = std::move(*lib);
                        tv_lib_cache_at_ = tv_now;
                    }
                }

                enrich_tv_groups(tv_groups, series_refs_by_id(tv_lib_cache_));
            }

            // Sonarr's per-row ETA rides along (tv_rows_from_groups) —
            // used only when the qBit overlay can't supply a live one.
            r.tv = tv_rows_from_groups(std::move(tv_groups), tv_rows);
        }
    } else {
        // No Sonarr configured at all — an empty tv[] IS the correct,
        // genuine answer, not a ratchet-worthy non-answer.
        r.tv_data_valid = true;
    }

    // Library snapshot — used for two things:
    //   1. The "awaiting release" list (monitored, no file, not in queue).
    //   2. Filling in poster_url on each queue item — Radarr's /queue
    //      API doesn't include movie images, so without this cross-ref
    //      every queue row would render the deterministic-tint
    //      placeholder instead of the actual poster.
    // CACHED with a 30s TTL: this is the full movie list — the heaviest
    // Radarr response — and it only feeds data that changes on add/
    // remove/import, not per-second. Re-fetching it on every 1.5s tick
    // re-downloaded and re-parsed the entire library ~57,000 times a
    // day. The queue itself and the qBit live overlay keep the 1.5s
    // cadence. Bypass: a queue item whose movie_id is missing from the
    // snapshot (a just-added movie) forces an immediate refetch so its
    // poster and awaiting-state appear right away. lib_cache_* members
    // are worker-thread-only (one refresh worker at a time, serialized
    // by refresh_in_flight_).
    const auto now = std::chrono::steady_clock::now();
    const bool lib_stale = movie_lib_cache_stale(
        (now - lib_cache_at_) > std::chrono::seconds(30), lib_cache_, r.queue);
    if (lib_stale) {
        // CHECKED: an unchecked read turned a Radarr blip into an EMPTY
        // snapshot for the full 30 s TTL — the "awaiting release" section
        // vanished and every queue row lost its poster. A failed read keeps
        // the previous snapshot and leaves the timestamp alone so the next
        // tick retries.
        if (auto lib = radarr_.get_library_checked()) {
            lib_cache_ = std::move(*lib);
            lib_cache_at_ = now;
        }
    }
    const auto& library = lib_cache_;

    // Patch poster_url on queue items that came back without one, then
    // build the "awaiting release" list — monitored library movies that
    // don't have a file yet and aren't already in the active queue. COPIES
    // out of the cached snapshot (moving would gut lib_cache_ for the next
    // tick).
    const std::unordered_set<int> active_movie_ids =
        patch_queue_posters(r.queue, library);
    r.awaiting = awaiting_release_movies(library, active_movie_ids);

    // Which of those are being actively searched right now (add-time
    // search, manual re-search, or the missing-movies sweep). Cheap
    // extra call on the 1.5s refresh cadence; only queried when there's
    // an awaiting list to annotate.
    if (!r.awaiting.empty()) {
        r.active_searches = radarr_.get_active_searches();
    }

    // Live-data overlay: pull current per-torrent stats from qBit
    // directly and merge them into the queue items by hash. Radarr
    // caches qBit's data on a 30-60s internal poll cycle, which makes
    // the kiosk's progress bar appear "frozen" between Radarr refreshes;
    // going direct gives us per-second updates. We keep Radarr's
    // identity fields (title, movie_id, queue id, poster) and only
    // override the live-changing telemetry (progress, dlspeed, peers,
    // seeds, eta, sizeleft, state).
    if (qbit_) {
        auto qbit_map = qbit_->get_torrents_by_hash();
        // Detect overlay failure: qbit_ is wired but we got no torrents
        // back AND Radarr's queue is non-empty (so we expected at least
        // those hashes to be in qBit). Either qBit is unreachable (netns
        // flap, container restart) or auth desynced (config drift).
        // Either way, the queue rows below this branch will retain
        // whatever stale progress Radarr returned — flag it so render()
        // can surface a warning instead of silently misleading the user.
        if (qbit_overlay_failed(qbit_map.empty(), r.queue.empty(), r.tv.empty())) {
            r.qbit_overlay_failed = true;
        }
        if (!qbit_map.empty()) {
            // Same map, same lowercased-hash key for both sections:
            // Sonarr's downloadId IS the torrent hash, so a season pack's
            // group resolves to exactly the torrent qBit is moving — ONE
            // live progress bar instead of ten stale ones. Translates
            // qBit's state names to the Radarr-style vocabulary
            // (apply_qbit_overlay / arr_state_from_qbit).
            apply_qbit_overlay(r.queue, r.tv, qbit_map);
        }
    }

    // Progress for any TV row the overlay didn't reach (qbit_ null,
    // unreachable, or the hash not in the map). Derived from the group's
    // MAXed size/sizeleft, which is the whole pack's — exactly what the
    // per-episode rows each reported. Mirrors SonarrParsers::parse_queue.
    derive_tv_progress(r.tv);

    // Path-independent import-state normalization. A download that has
    // finished in qBit but is still being copied into the library by
    // Radarr reports, Radarr-side, status="completed" with
    // trackedDownloadState="importing"/"importPending". The qBit overlay
    // above collapses the torrent's seeding state to a bare "completed"
    // (green), which reads to the user as "done" — then the row silently
    // vanishes once import finishes. Reclassify here, AFTER the overlay
    // and independent of it, so this also fires when qbit_ is null,
    // unreachable, or the hash isn't in the qBit map (paths where the
    // overlay block above never ran and q.state is still Radarr's raw
    // "completed"). q.tracked_download_state is never mutated by the
    // overlay, so it's safe to read here.
    // (reclassify_import_state: importing/importPending -> amber
    // "importing"; importBlocked/importFailed -> "warning", LibraryScreen's
    // BAD RELEASE semantics.)
    for (auto& q : r.queue) reclassify_import_state(q.state, q.tracked_download_state);

    // Same reclassification for TV rows — a season pack that finished in
    // qBit but is still being imported reads "completed" (green, i.e.
    // "done") right up until the row vanishes, which is exactly the
    // confusion the movie path above was fixed for.
    for (auto& t : r.tv)
        reclassify_import_state(t.group.status, t.group.tracked_download_state);

    // r.error was captured in-band with the queue request above. Reading
    // RadarrClient::last_error() here would pair this result with whichever
    // unrelated worker happened to write that shared diagnostic most recently.

    // Publish under the mutex; result_ready_ is the atomic flag the
    // main thread polls each frame. Cleared by apply_pending() when
    // the result is consumed.
    {
        std::lock_guard<std::mutex> lk(result_mtx_);
        pending_ = std::move(r);
    }
    result_ready_.store(true);
    // refresh_in_flight_ is cleared by inflight_guard as this returns.
}

void QueueScreen::apply_pending() {
    if (!result_ready_.load()) return;

    PendingResult r;
    {
        std::lock_guard<std::mutex> lk(result_mtx_);
        r = std::move(pending_);
    }
    result_ready_.store(false);

    queue_ = std::move(r.queue);
    // RATCHET: only overwrite tv_ when this cycle's data is a genuine
    // answer (Sonarr engaged, or sonarr_ is null). A nullopt cycle leaves
    // tv_ exactly as it was — see PendingResult::tv_data_valid and
    // run_refresh()'s doc comment for why an unanswered poll must not be
    // allowed to erase a live season pack from the screen.
    if (r.tv_data_valid) {
        tv_ = std::move(r.tv);
    }
    tv_unreachable_ = r.tv_unreachable;
    awaiting_ = std::move(r.awaiting);
    active_searches_ = std::move(r.active_searches);
    last_error_ = std::move(r.error);
    qbit_overlay_failed_ = r.qbit_overlay_failed;
    last_refresh_at_ = std::chrono::steady_clock::now();
    refreshing_ = false;

    // Clamp cursor to valid range now that we have new data. Spans both
    // sections — the movie rows and the TV groups are one list.
    cursor_ = clamp_queue_cursor(cursor_, row_count());

    // Clear a pending cancel if the row it was attached to vanished.
    // Searched in the section it was armed in: the two id spaces are
    // independent (Radarr's queue ids and Sonarr's both start small), so
    // matching across sections could keep a cancel armed on a row the
    // user never touched.
    if (cancel_pending_) {
        const bool still_present = cancel_target_present(
            cancel_pending_is_tv_, cancel_pending_queue_id_, queue_, tv_);
        if (!still_present) {
            cancel_pending_ = false;
            cancel_pending_is_tv_ = false;
            cancel_pending_queue_id_ = 0;
        }
    }
}

void QueueScreen::update() {
    // Drain any worker result into the live state. Cheap — it's an
    // atomic load most frames, only takes the mutex when result is ready.
    apply_pending();
    drain_cancel_result();

    auto now = std::chrono::steady_clock::now();

    // Expire a stale cancel confirmation.
    if (cancel_pending_) {
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - cancel_pending_at_).count();
        if (elapsed_ms >= kCancelPendingMs) {
            cancel_pending_ = false;
            cancel_pending_is_tv_ = false;
            cancel_pending_queue_id_ = 0;
        }
    }

    // Trigger a refresh every kRefreshIntervalMs. refresh_async() is
    // a no-op if a worker is already running, so this won't pile up
    // requests during a slow Radarr response.
    auto since_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - last_refresh_at_).count();
    if (since_ms >= kRefreshIntervalMs) {
        refresh_async();
    }
}

void QueueScreen::do_cancel_focused() {
    if (cursor_ < 0 || cursor_ >= row_count()) return;
    const int movie_rows = static_cast<int>(queue_.size());
    const bool is_tv = cursor_ >= movie_rows;
    if (is_tv && sonarr_ == nullptr) return;
    // Capture by VALUE: queue_/tv_ are render-thread state that the next
    // apply_pending() replaces while the DELETE is in flight.
    const int queue_id =
        is_tv ? tv_[static_cast<size_t>(cursor_ - movie_rows)].group.first_queue_id
              : queue_[cursor_].id;
    std::string title =
        is_tv ? tv_row_title(tv_[static_cast<size_t>(cursor_ - movie_rows)].group)
              : queue_[cursor_].title;
    cancel_pending_ = false;
    cancel_pending_is_tv_ = false;
    cancel_pending_queue_id_ = 0;
    if (cancel_in_flight_.load(std::memory_order_acquire)) {
        ::ui::Toast::show("Still cancelling the last download\xE2\x80\xA6");
        return;
    }
    // cancel_in_flight_ was false, so the previous worker has finished and
    // this join is instant.
    if (cancel_worker_.joinable()) cancel_worker_.join();
    cancel_in_flight_.store(true, std::memory_order_release);
    try {
        cancel_worker_ = std::thread([this, is_tv, queue_id,
                                      title = std::move(title)]() {
            bool ok = false;
            run_guarded("queue cancel", [&] {
                // TV: EXACTLY ONE call. Sonarr's DELETE acts on the whole
                // download, so this removes every episode row of the pack;
                // iterating the pack's sibling ids would only collect 404s
                // (sonarr_client.h).
                ok = is_tv ? sonarr_->cancel_queue_item(queue_id)
                           : radarr_.cancel_queue_item(queue_id);
            });
            cancel_ok_ = ok;
            cancel_title_ = title;
            cancel_done_.store(true, std::memory_order_release);
            cancel_in_flight_.store(false, std::memory_order_release);
        });
    } catch (const std::system_error&) {
        cancel_in_flight_.store(false, std::memory_order_release);
        ::ui::Toast::show("Couldn't start the cancel \xE2\x80\x94 try again");
    }
}

void QueueScreen::drain_cancel_result() {
    if (!cancel_done_.exchange(false, std::memory_order_acq_rel)) return;
    if (!cancel_ok_) ::ui::Toast::show(cancel_failed_toast(cancel_title_));
    // Refresh now either way — success makes the row disappear without
    // waiting on the poll; failure shows the row is still there. Async;
    // a refresh already in flight makes this a no-op and the next tick
    // catches up.
    refresh_async();
}

Screen QueueScreen::handle_input(const std::vector<platform::InputEvent>& events) {
    for (const auto& e : events) {
        // BTN4 (SETTINGS_MENU, black) — short-press is a no-op in v1.6.x.
        // No slide-in overlay on Queue.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            continue;
        }

        // BTN1 (PREV, yellow) — previous tab. Queue sits at index 4 in the
        // 6-tab Marquee strip (Popular | Top Rated | Search | Library |
        // Queue | Settings), so PREV walks to Library at index 3.
        if (e.action == platform::InputAction::PREV && e.pressed) {
            return Screen::Library;
        }

        // BTN3 (NEXT, green) — next tab. Queue is at index 4, NEXT goes
        // to Settings at index 5 (rightmost).
        if (e.action == platform::InputAction::NEXT && e.pressed) {
            return Screen::MovieSettings;
        }

        // ROTATE (rotary CW/CCW + D-pad left/right) and ROTATE_VERTICAL
        // (D-pad up/down) both walk the rows — the rotary encoder is the
        // only cursor input on a controller-free enclosure.
        const auto step = step_cursor(e.action, e.delta, cursor_, row_count());
        if (step.is_nav) {
            if (row_count() > 0) {
                cursor_ = step.cursor;
                // Navigation clears any pending cancel so the user can't
                // accidentally confirm on the wrong row.
                if (cancel_pending_) {
                    cancel_pending_ = false;
                    cancel_pending_is_tv_ = false;
                    cancel_pending_queue_id_ = 0;
                }
            }
            continue;
        }

        // BTN2 (PLAY_PAUSE, red) — intercepted globally by the exit modal
        // in main.cpp. It never reaches here; no per-screen handler needed.

        if (e.action == platform::InputAction::SELECT && e.pressed) {
            bool focused_is_tv = false;
            int focused_id = 0;
            if (!focused_row(focused_is_tv, focused_id)) continue;
            if (decide_queue_select(cancel_pending_, cancel_pending_is_tv_,
                                    cancel_pending_queue_id_, focused_is_tv,
                                    focused_id) == QueueSelect::Confirm) {
                // Stage 2: confirm.
                do_cancel_focused();
            } else {
                // Stage 1: arm.
                cancel_pending_ = true;
                cancel_pending_is_tv_ = focused_is_tv;
                cancel_pending_queue_id_ = focused_id;
                cancel_pending_at_ = std::chrono::steady_clock::now();
            }
            continue;
        }
    }
    return Screen::Queue;
}

// ----------------------------------------------------------------------------
// Rendering
// ----------------------------------------------------------------------------

void QueueScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    const ::ui::Theme& th = r.mb_theme();
    r.mb_fill_background();

    const float w = static_cast<float>(screen_w);
    const float h = static_cast<float>(screen_h);

    // 500ms blink cycle, sourced from epoch time. Keeps the focused-row ◂
    // cursor breathing in lockstep with DetailScreen's button cursor and
    // the home menu's playlist cursor — when the user crosses between
    // screens the indicators visually share the same heartbeat.
    auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    const bool blink_on = (epoch_ms / 500) % 2 == 0;

    // --- Marquee 6-tab header: "Queue" title (left) + tab strip (right) -
    // Matches the chrome the other Marquee screens render so the user
    // sees one consistent UI walking BTN1/BTN3 across the strip. The
    // previous in-screen "DOWNLOAD QUEUE" + count + refresh-status block
    // is collapsed into a sub-line drawn just below the chrome header,
    // so the count + refresh affordance survives without competing with
    // the tab strip for header real estate.
    int header_bottom = 0;
    {
        namespace chrome = ::media_browser::ui::chrome;
        // Shared strip (chrome::marquee_tabs) — this screen used to carry
        // its own six-label copy, which silently lost the "For You" chip
        // when that tab was added to Browse.
        const std::vector<chrome::TabSpec> tabs = chrome::marquee_tabs("Queue");
        header_bottom = chrome::draw_screen_header(
            r, screen_w, "Queue", tabs, /*focused_tab=*/-1);
    }

    // Count + refresh-status sub-line beneath the chrome header. Reads
    // as a status banner for the screen below the tab strip, in the same
    // dim-cream small-font idiom Library uses for its stats line.
    // Anchored to chrome::kSafeInset_px so the left/right edges line up
    // with the tab strip + title above (NOT kPaddingX, which is the
    // body-row inset for the queue rows below).
    // Warning lines drawn under the count/refresh sub-line this frame.
    // body_top consumes this so the row list starts BELOW the stack —
    // with two lines up (Radarr + Sonarr offline during a netns flap),
    // line 2 landed ~8px inside the first row's poster. Zero lines =
    // zero shift: the everyday layout is byte-identical.
    int warn_lines = 0;
    {
        // TV groups count as downloads alongside the movie rows — one
        // entry per DOWNLOAD, so a 10-episode pack adds 1, not 10.
        const int downloading = static_cast<int>(queue_.size() + tv_.size());
        const std::string count_text = queue_count_line(downloading, awaiting_.size());
        int sub_size = th.font_small_size;
        int sub_baseline = r.mb_text_baseline(sub_size);
        float sub_y = static_cast<float>(header_bottom)
                    + static_cast<float>(::media_browser::ui::chrome::kPad2)
                    + static_cast<float>(sub_baseline);
        const float sub_x_left =
            static_cast<float>(::media_browser::ui::chrome::kSafeInset_px);
        r.mb_draw_text(count_text, sub_x_left, sub_y, sub_size, th.dim, 0.85f);

        // Refresh indicator on the far right of the sub-line. Color-coded
        // by age so a stuck refresh (Radarr or qBit unreachable behind the
        // VPN netns when Gluetun flaps, etc.) is visually obvious — the
        // progress bars on the rows below would otherwise look frozen
        // because Radarr's queue snapshot stays at its last cached
        // values while the live qBit overlay can't reach the container.
        //   <  5 s : dim cream      — normal cadence (refresh fires @ 1.5 s)
        //   5–15 s : dim cream      — still acceptable
        //  15–45 s : accent gold    — refresh starting to slip
        //   > 45 s : highlight red  — refresh is clearly broken; bars stale
        const QueueLineView ind = queue_refresh_indicator(
            refreshing_,
            static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::steady_clock::now() -
                                       last_refresh_at_)
                                       .count()));
        const std::string& ind_text = ind.text;
        const ::ui::Color& ind_color = tone_color(th, ind.tone);
        const float ind_alpha = ind.alpha;
        int ind_w = r.mb_text_width(ind_text, sub_size);
        const float sub_x_right = static_cast<float>(
            screen_w - ::media_browser::ui::chrome::kSafeInset_px);
        float ind_x = sub_x_right - static_cast<float>(ind_w);
        r.mb_draw_text(ind_text, ind_x, sub_y, sub_size, ind_color, ind_alpha);

        // Warning lines, stacked below the count/refresh line — same
        // dim-cream-idiom-but-accent-colored sub-line pattern, one on top
        // of the other when more than one applies. Order is
        // most-fundamental-first: Radarr offline (the movie backend is
        // unreachable) before Sonarr offline (the TV backend is
        // unreachable) before the qBit live-data warning (a live overlay
        // gap on top of two backends that ARE reachable).
        float warn_y = sub_y + static_cast<float>(sub_size) + 4.0f;

        // Radarr-offline: keyed on the RADAR side alone (queue_ empty +
        // last_error_ set), independent of tv_ — restores the signal a TV
        // pack used to suppress when the combined row list stopped being
        // empty (review Fix 2). Skipped here when the centered "Radarr
        // service offline" state below is ALSO about to render (movie queue,
        // TV queue, and awaiting list all empty) so it isn't drawn twice.
        if (show_inline_radarr_warning(!last_error_.empty(), queue_.empty(),
                                       tv_.empty(), awaiting_.empty())) {
            std::string warn_text = radarr_offline_line(last_error_);
            warn_text = truncate_to_width(r, warn_text, sub_size,
                                          w - 2.0f * kPaddingX);
            r.mb_draw_text(warn_text, sub_x_left, warn_y, sub_size,
                           th.highlight2, 0.95f);
            warn_y += static_cast<float>(sub_size) + 4.0f;
            ++warn_lines;
        }

        // Sonarr-offline: only while TV rows are actually on screen — with
        // none retained, the centered empty-state / awaiting-only view
        // carries the same message instead (see below and the "Add a
        // movie" hint swap further down).
        if (show_sonarr_offline_line(tv_unreachable_, tv_.empty())) {
            const std::string warn_text = sonarr_offline_line();
            r.mb_draw_text(warn_text, sub_x_left, warn_y, sub_size,
                           th.accent, 0.95f);
            warn_y += static_cast<float>(sub_size) + 4.0f;
            ++warn_lines;
        }

        // qBit-overlay-failed sub-line. Gives the user a clear "the bars
        // below are stale" signal — without it a netns flap or qBit auth
        // desync makes downloads look frozen with no explanation. Neutral
        // "Radarr/Sonarr" wording (review Fix 3): the old text hardcoded
        // "Radarr's cached snapshot", which was simply wrong on any
        // refresh where the stale bars belonged to a TV row instead.
        if (show_qbit_overlay_line(qbit_overlay_failed_, queue_.empty(), tv_.empty())) {
            const std::string warn_text = qbit_overlay_line();
            r.mb_draw_text(warn_text, sub_x_left, warn_y, sub_size,
                           th.accent, 0.95f);
            ++warn_lines;
        }
    }

    // --- Footer band (the hints themselves are drawn last) ------------
    // Reserve the footer band height (font + a little breathing room
    // above) so list rows don't collide with it.
    const int hint_size = th.font_small_size;
    const float footer_band_top = h - kFooterMarginY
                                - static_cast<float>(hint_size) - 8.0f;

    // --- Centered states: empty queue / Radarr offline ---------------
    // Same mid-screen single-message pattern as Detail's Loading/NoTmdb
    // states: large text vertically centered between header band and
    // footer band, with an optional dim sub-line for context. Body
    // starts below the chrome header AND the count/refresh sub-line —
    // header_bottom + kPad2 (gap above sub-line) + font_small_size
    // (sub-line glyph height) + 16px breathing room.
    const float body_top    = static_cast<float>(header_bottom)
                            + static_cast<float>(::media_browser::ui::chrome::kPad2)
                            + static_cast<float>(th.font_small_size)
                            + 16.0f
                            // Each warning line advances the stack by
                            // sub-size + 4; the list clears exactly that.
                            + static_cast<float>(warn_lines)
                            * (static_cast<float>(th.font_small_size) + 4.0f);
    const float body_bottom = footer_band_top;
    const float body_h      = body_bottom - body_top;

    // Track the y-extent consumed by the active-queue render so the
    // AWAITING RELEASE section below knows where to start drawing. Stays
    // at body_top when the queue is empty — the awaiting section then
    // claims the whole body region.
    float post_queue_y = body_top;

    // --- The combined row list ---------------------------------------
    // Movie rows first (unchanged order and content), TV downloads after.
    // One vector means one cursor, one scroll window, and one painter for
    // both sections. Empty tail when sonarr_ is null, which is exactly the
    // movie-only screen this was before.
    const std::vector<QueueRowView> rows = build_queue_rows(queue_, tv_);

    if (rows.empty() && awaiting_.empty()) {
        if (!last_error_.empty()) {
            // Radarr offline — primary message in red (highlight2),
            // matching the destructive/warning idiom from Detail's
            // Confirm-Remove button.
            int sz = th.font_large_size;
            std::string msg = "Radarr service offline";
            int mw = r.mb_text_width(msg, sz);
            float mx = (w - static_cast<float>(mw)) / 2.0f;
            float my = body_top + body_h / 2.0f
                     - static_cast<float>(sz) * 0.6f
                     + static_cast<float>(r.mb_text_baseline(sz));
            r.mb_draw_text(msg, mx, my, sz, th.highlight2, 0.95f);

            // Dim sub-line: the underlying error string, truncated to fit.
            int sz2 = th.font_small_size;
            std::string detail = truncate_to_width(r, last_error_, sz2,
                                                   w - 2.0f * kPaddingX);
            int dw = r.mb_text_width(detail, sz2);
            float dx = (w - static_cast<float>(dw)) / 2.0f;
            float dy = my + static_cast<float>(sz) * 0.9f
                     + static_cast<float>(r.mb_text_baseline(sz2));
            r.mb_draw_text(detail, dx, dy, sz2, th.dim, 0.85f);
        } else {
            // Happy-path empty state — dim large text, with a
            // medium-font instruction underneath.
            int sz = th.font_large_size;
            std::string msg = "Queue is empty";
            int mw = r.mb_text_width(msg, sz);
            float mx = (w - static_cast<float>(mw)) / 2.0f;
            float my = body_top + body_h / 2.0f
                     - static_cast<float>(sz) * 0.6f
                     + static_cast<float>(r.mb_text_baseline(sz));
            r.mb_draw_text(msg, mx, my, sz, th.dim, 0.9f);

            // The instruction line normally invites the user to start a
            // download. When Sonarr didn't answer this cycle AND there is
            // no retained TV row to show alongside it (both queue_ and
            // tv_ are empty here, by construction of `rows`), "no active
            // downloads, add a movie" would be a false reassurance — we
            // genuinely do not know whether a season pack is running.
            // Swap in the same warning line the stacked sub-line uses
            // instead of the add-a-movie copy (review Fix 1c).
            int sz2 = th.font_medium_size;
            const QueueLineView hint_view = queue_empty_hint(tv_unreachable_);
            const std::string& hint_msg = hint_view.text;
            const ::ui::Color& hint_color = tone_color(th, hint_view.tone);
            const float hint_alpha = hint_view.alpha;
            int hw = r.mb_text_width(hint_msg, sz2);
            float hx = (w - static_cast<float>(hw)) / 2.0f;
            float hy = my + static_cast<float>(sz) * 0.9f
                     + static_cast<float>(r.mb_text_baseline(sz2));
            r.mb_draw_text(hint_msg, hx, hy, sz2, hint_color, hint_alpha);
        }
    } else if (!rows.empty()) {
        // --- Queue rows --------------------------------------------------
        // Vertical scrolling list. We clamp scroll_row_ here (in render —
        // not handle_input — because this is where we know how many rows
        // can fit on the current screen height).
        const float list_top = body_top;
        const float list_h   = body_h;
        int visible_rows = std::max(1,
            static_cast<int>(list_h / (kRowHeight + kRowGap)));
        scroll_row_ = queue_scroll_row(cursor_, scroll_row_, visible_rows);
        int n = static_cast<int>(rows.size());
        int end_row = std::min(n, scroll_row_ + visible_rows);

        const float row_x = kPaddingX;
        const float row_w = w - 2.0f * kPaddingX;

        for (int i = scroll_row_; i < end_row; ++i) {
            const auto& q = rows[static_cast<size_t>(i)];
            const bool focused      = (i == cursor_);
            // Section-qualified: Radarr and Sonarr queue ids are separate
            // sequences, so the id alone can match a row in the other
            // section that the user never armed.
            const bool cancel_armed = cancel_armed_for(
                cancel_pending_, cancel_pending_is_tv_, cancel_pending_queue_id_,
                q.is_tv, q.id);
            float ry = list_top + (i - scroll_row_) * (kRowHeight + kRowGap);

            // Row outline. Unfocused: thin dim outline at low alpha to
            // delineate without competing with content. Focused: 3px gold
            // outline — same border-and-text idiom as Detail's focused
            // button. NO row fill — pure background, like the home menu.
            if (focused) {
                r.mb_stroke_rect(row_x, ry, row_w, kRowHeight,
                                 kRowOutlineW, th.accent, 1.0f);
            } else {
                r.mb_stroke_rect(row_x, ry, row_w, kRowHeight,
                                 kRowDimOutlineW, th.dim, 0.4f);
            }

            // --- Left column: small poster (aspect-fit) --------------
            // 1px dim border around it — same single-pixel frame Detail
            // uses on its big poster, just sized down for a row context.
            float poster_x = row_x + kRowInnerPadding;
            float poster_y = ry + (kRowHeight - kPosterH) / 2.0f;
            // 70 px thumbnail: the grid-size (w185) variant, shared with
            // the Library card's texture.
            r.mb_draw_poster_fit(media_browser::tmdb_poster_url_for_card(
                                     q.poster_url, static_cast<int>(kPosterW)),
                                 poster_x, poster_y, kPosterW, kPosterH,
                                 stable_tint_for_id(q.id), 1.0f);
            r.mb_stroke_rect(poster_x, poster_y, kPosterW, kPosterH,
                             kPosterBorderW, th.dim, 0.6f);

            // --- Right edge: cursor / cancel CTA ---------------------
            // Reserve a slot at the right edge of the row for either a
            // blinking ◂ marker (focused, idle) or a "CONFIRM CANCEL"
            // call-to-action box (focused, cancel armed). When the row
            // is unfocused, this slot is empty — the row is pure content.
            //
            // We compute the slot width FIRST so the middle-column text
            // wrap math knows how much horizontal real estate it has.
            float right_slot_w = 0.0f;
            if (focused) {
                if (cancel_armed) {
                    // CONFIRM CANCEL box — outlined in red (highlight2),
                    // text in red. No fill (border-and-text idiom). It
                    // pulses with the same 500ms blink to communicate
                    // urgency: alpha drops on the off-phase.
                    int btn_size = th.font_small_size;
                    const std::string label = "CONFIRM CANCEL";
                    int btn_text_w = r.mb_text_width(label, btn_size);
                    float box_w = static_cast<float>(btn_text_w)
                                + 2.0f * kCancelBoxPadX;
                    right_slot_w = box_w + kRowInnerPadding;
                } else {
                    // Blinking ◂ marker. Reserve enough slot for the
                    // triangle plus a small gutter on either side.
                    right_slot_w = 28.0f;
                }
            }

            // --- Middle column: title (top), sub-line (mid), bar (bot) -
            // The middle column is everything between the poster and the
            // right-edge cursor slot. We stack three elements vertically
            // inside it: title, state/rate/peers/ETA sub-line, and the
            // progress bar with its percentage.
            float mid_x = poster_x + kPosterW + kRowInnerPadding;
            float mid_right = row_x + row_w - kRowInnerPadding - right_slot_w;
            float mid_max_w = std::max(40.0f, mid_right - mid_x);

            // Live-activity dot on actively-downloading rows. Pulses
            // alpha 0.4 → 1.0 on a 500ms cycle (same heartbeat as the
            // focused-row marker and the home-menu cursor blink). The
            // pulse is the "heartbeat" signal — even when speed is
            // steady and percent is moving slowly, the dot keeps
            // breathing so the user can see at a glance that the
            // download is alive vs stalled.
            //
            // The phase is derived from a smooth sin curve rather than
            // a binary on/off blink — that's harder to mistake for a
            // visual artifact and reads more clearly as "alive."
            const bool is_active_dl = queue_row_active(q);
            float dot_inset_x = 0.0f;
            if (is_active_dl) {
                // sin-based smooth pulse, period = 1.2s
                double phase = (epoch_ms % 1200) / 1200.0;
                float pulse = 0.4f + 0.6f *
                    static_cast<float>(0.5 + 0.5 * std::sin(
                        phase * 2.0 * 3.14159265358979));
                const float dot_d = 8.0f;
                const float dot_x = poster_x + kPosterW + kRowInnerPadding;
                const float dot_y = ry + kRowInnerPadding
                                  + (static_cast<float>(th.font_medium_size) / 2.0f)
                                  - dot_d / 2.0f;
                r.mb_fill_rect(dot_x, dot_y, dot_d, dot_d,
                               th.highlight1, pulse);  // green when active
                dot_inset_x = dot_d + 8.0f;  // shift title to make room
            }

            // Title — body font, fg cream, prominent.
            int title_size = th.font_medium_size;
            int title_baseline = r.mb_text_baseline(title_size);
            std::string title_str =
                q.title.empty() ? std::string("Untitled") : q.title;
            title_str = truncate_to_width(r, title_str, title_size,
                                          mid_max_w - dot_inset_x);
            float title_y = ry + kRowInnerPadding
                          + static_cast<float>(title_baseline);
            r.mb_draw_text(title_str, mid_x + dot_inset_x, title_y, title_size,
                           th.fg, focused ? 1.0f : 0.92f);

            // Sub-line — "Downloading · 1.2 MB/s · 18 peers · ETA 12m 05s"
            // in dim cream small-font, separated by middle-dot bullets.
            // Same separator the Detail meta line uses.
            int sub_size = th.font_small_size;
            int sub_baseline = r.mb_text_baseline(sub_size);
            // TV marker first (survives truncation), then state ("Importing…"
            // gets its ellipsis), downloaded/total, rate, ETA, and peers/seeds
            // last (least critical) — queue_row_sub_line.
            std::string sub_line = truncate_to_width(r, queue_row_sub_line(q),
                                                     sub_size, mid_max_w);
            float sub_y = title_y + static_cast<float>(title_size) * 0.5f
                        + static_cast<float>(sub_baseline);
            r.mb_draw_text(sub_line, mid_x, sub_y, sub_size, th.dim,
                           focused ? 0.9f : 0.8f);

            // Progress bar — outline-only track with a colored fill.
            // The bar lives at the bottom of the row, spanning the
            // middle column up to a percentage label on the right.
            const float pct_label_w = 56.0f;  // reserved for "100%"
            const float pct_gap     = 10.0f;
            float bar_track_w = std::max(40.0f,
                std::min(kProgressBarW, mid_max_w - pct_label_w - pct_gap));
            float bar_x = mid_x;
            float bar_y = ry + kRowHeight - kRowInnerPadding - kProgressBarH;

            // Track: outline-only, dim. Same idiom as the home menu's
            // volume slider track.
            r.mb_stroke_rect(bar_x, bar_y, bar_track_w, kProgressBarH,
                             kProgressBorderW, th.dim, 0.5f);

            // Fill: colored by state. Inset by the border thickness so
            // the fill sits cleanly inside the outline.
            double pct = std::clamp(q.progress, 0.0, 1.0);
            ::ui::Color fill_color = tone_color(th, queue_progress_tone(q.state));
            // Cancel-armed rows recolor the fill red so the visual state
            // matches the CTA on the right edge.
            if (cancel_armed) fill_color = th.highlight2;
            float inset = kProgressBorderW;
            float inner_w = bar_track_w - 2.0f * inset;
            float inner_h = kProgressBarH - 2.0f * inset;
            float fill_w = static_cast<float>(pct) * inner_w;
            if (fill_w > 0.0f && inner_h > 0.0f) {
                r.mb_fill_rect(bar_x + inset, bar_y + inset,
                               fill_w, inner_h, fill_color, 0.9f);
            }

            // Percentage — right-aligned next to the bar, in the fill
            // color so the user's eye associates the number with the
            // progress hue. One decimal place ("3.8%") rather than
            // integer ("4%"): pieces in a multi-GB torrent are 4-8 MB
            // each = ~0.05% per piece. Integer rounding made progress
            // appear to "jump" from 1% to 3% because 5+ pieces would
            // complete between display updates. Tenths give the user
            // continuous visual feedback that things are moving.
            int pct_size = th.font_small_size;
            int pct_baseline = r.mb_text_baseline(pct_size);
            // "100%" / "0%" special cases, else one decimal
            // (queue_percent_text).
            std::string pct_text = queue_percent_text(pct);
            int pct_text_w = r.mb_text_width(pct_text, pct_size);
            // Right-align the percentage inside its reserved label slot
            // so wider strings ("100%") sit flush with the row's right
            // content edge instead of jumping around as digits change.
            float pct_x = bar_x + bar_track_w + pct_gap
                        + (pct_label_w - static_cast<float>(pct_text_w));
            float pct_y = bar_y + (kProgressBarH / 2.0f)
                        - static_cast<float>(pct_size) / 2.0f
                        + static_cast<float>(pct_baseline);
            r.mb_draw_text(pct_text, pct_x, pct_y, pct_size, fill_color,
                           focused ? 1.0f : 0.9f);

            // --- Right-edge cursor / cancel CTA (drawn last) ---------
            if (focused && cancel_armed) {
                // CONFIRM CANCEL box. Vertically centered on the row,
                // anchored to the right edge.
                int btn_size = th.font_small_size;
                int btn_baseline = r.mb_text_baseline(btn_size);
                const std::string label = "CONFIRM CANCEL";
                int btn_text_w = r.mb_text_width(label, btn_size);
                float box_w = static_cast<float>(btn_text_w)
                            + 2.0f * kCancelBoxPadX;
                float box_x = row_x + row_w - kRowInnerPadding - box_w;
                float box_y = ry + (kRowHeight - kCancelBoxH) / 2.0f;
                // Pulse: full alpha on blink-on, 0.45 on blink-off.
                float pulse = blink_on ? 1.0f : 0.45f;
                r.mb_stroke_rect(box_x, box_y, box_w, kCancelBoxH,
                                 kCancelBoxBorder, th.highlight2, pulse);
                float tx = box_x + kCancelBoxPadX;
                float ty = box_y + (kCancelBoxH - static_cast<float>(btn_size))
                                / 2.0f
                         + static_cast<float>(btn_baseline);
                r.mb_draw_text(label, tx, ty, btn_size, th.highlight2, pulse);
            } else if (focused && blink_on) {
                // Blinking ◂ marker — same triangle primitive Detail uses
                // for its focused-button cursor, in steel-blue (accent2).
                int marker_ref_size = th.font_medium_size;
                float marker_size = static_cast<float>(marker_ref_size) * 0.45f;
                float marker_cx = row_x + row_w - 14.0f;
                float marker_cy = ry + kRowHeight / 2.0f;
                r.mb_fill_triangle(
                    marker_cx,                         marker_cy - marker_size,
                    marker_cx,                         marker_cy + marker_size,
                    marker_cx - marker_size * 1.2f,    marker_cy,
                    th.dim, 1.0f);
            }
        }

        // Where the next row would have been drawn — anchor for the
        // AWAITING RELEASE section that may follow below.
        int drawn = end_row - scroll_row_;
        post_queue_y = list_top
                     + static_cast<float>(drawn) * (kRowHeight + kRowGap);
    }

    // -----------------------------------------------------------------
    // AWAITING RELEASE section — monitored library movies that haven't
    // grabbed yet because no usable release is available. Radarr's RSS
    // sync re-checks every ~30 minutes; this section makes the
    // background watchlist visible to the user.
    // -----------------------------------------------------------------
    if (!awaiting_.empty()) {
        const float row_x_a = kPaddingX;
        const float row_w_a = w - 2.0f * kPaddingX;
        // Leave a 32px gap below the active queue rows. When the queue
        // was empty (post_queue_y == body_top) we sit flush with the
        // header rule margin so the section fills the body region.
        float section_y = post_queue_y;
        if (post_queue_y > body_top) {
            section_y = post_queue_y + 32.0f - kRowGap;
        }

        const float footer_reserve =
            (h - footer_band_top) + 8.0f;
        if (section_y < h - footer_reserve - 80.0f) {
            // Section header — same Zen Dots / steel-blue chrome the
            // top "DOWNLOAD QUEUE" header uses, scaled down so it reads
            // as a sub-section rather than a peer.
            int hd_size = th.font_heading_size;
            const std::string heading = "AWAITING RELEASE";
            r.mb_draw_title_text(heading, row_x_a,
                                 section_y + static_cast<float>(hd_size),
                                 hd_size, th.dim, 1.0f);
            float rule_y = section_y + static_cast<float>(hd_size) + 12.0f;
            r.mb_draw_line(row_x_a, rule_y, row_x_a + row_w_a, rule_y,
                           2.0f, th.dim, 0.85f);
            section_y = rule_y + 14.0f;

            // How many awaiting movies are being actively searched right
            // now — drives both the section sub-line and per-row state.
            const int searching_now = count_searching(awaiting_, active_searches_);

            // Sub-line explaining the state — dim cream small-font. When
            // a search is live, lead with that (green) so a just-added
            // movie doesn't read as "nothing happening for 30 minutes".
            int sub_size = th.font_small_size;
            int sub_baseline = r.mb_text_baseline(sub_size);
            const QueueLineView sub_view = awaiting_sub_line(searching_now);
            const std::string& sub = sub_view.text;
            const ::ui::Color& sub_col = tone_color(th, sub_view.tone);
            std::string sub_drawn = truncate_to_width(r, sub, sub_size,
                                                      row_w_a);
            r.mb_draw_text(sub_drawn, row_x_a,
                           section_y + static_cast<float>(sub_baseline),
                           sub_size, sub_col, sub_view.alpha);
            section_y += static_cast<float>(sub_size) + 14.0f;

            // One row per awaiting movie. Read-only, no cursor focus.
            int title_size = th.font_medium_size;
            int title_baseline = r.mb_text_baseline(title_size);
            float row_h_a = static_cast<float>(title_size) * 1.6f;

            // Smooth pulse for the "Searching…" rows (same 1.2s sin
            // heartbeat as the active-download dot) so the operator can
            // see at a glance which titles are being worked right now.
            double s_phase = (epoch_ms % 1200) / 1200.0;
            float s_pulse = 0.55f + 0.45f *
                static_cast<float>(0.5 + 0.5 * std::sin(
                    s_phase * 2.0 * 3.14159265358979));

            int drawn_count = 0;
            for (const auto& m : awaiting_) {
                if (section_y + row_h_a > h - footer_reserve) break;
                const bool searching = awaiting_searching(m, active_searches_);
                std::string label_drawn = truncate_to_width(
                    r, awaiting_row_label(m, searching), title_size, row_w_a);
                // Searching rows glow green and pulse; passive rows stay
                // calm cream.
                ::ui::Color row_col = searching ? th.highlight1 : th.fg;
                float row_alpha = searching ? s_pulse : 0.85f;
                r.mb_draw_text(label_drawn, row_x_a,
                               section_y + static_cast<float>(title_baseline),
                               title_size, row_col, row_alpha);
                section_y += row_h_a;
                ++drawn_count;
            }

            // If we ran out of room and there are more awaiting items,
            // surface a "+N more..." indicator.
            int hidden = static_cast<int>(awaiting_.size()) - drawn_count;
            if (hidden > 0
                && section_y + row_h_a <= h - footer_reserve) {
                std::string more = "+" + std::to_string(hidden)
                                 + " more awaiting";
                r.mb_draw_text(more, row_x_a,
                               section_y + static_cast<float>(title_baseline),
                               title_size, th.dim, 0.7f);
            }
        }
    }

    // --- Footer hints (Marquee shared style) -------------------------
    // The rotary press is the two-stage cancel: "Cancel" on a focused row,
    // "Confirm" while that row is armed (the footer half of the row's
    // CONFIRM CANCEL box) — queue_footer_hints.
    bool focused_is_tv = false;
    int focused_id = 0;
    const bool has_focused = focused_row(focused_is_tv, focused_id);
    const bool focused_armed =
        has_focused &&
        decide_queue_select(cancel_pending_, cancel_pending_is_tv_,
                            cancel_pending_queue_id_, focused_is_tv,
                            focused_id) == QueueSelect::Confirm;
    ::media_browser::ui::chrome::draw_footer_hints(
        r, screen_w, screen_h, queue_footer_hints(has_focused, focused_armed));
}

}  // namespace media_browser::ui
