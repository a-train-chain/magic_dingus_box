#include "media_browser/ui/queue_screen.h"
#include "media_browser/ui/mb_chrome.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/radarr/radarr_client.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "ui/toast.h"

namespace media_browser::ui {

namespace {

// Retro home-menu-inspired layout (target 1280x720). The Queue screen is
// the third member of the Browse / Detail / Queue trio, and it adopts the
// same chrome as DetailScreen so the user feels they're inside one unified
// kiosk surface as they cross between screens:
//   - "DOWNLOAD QUEUE" header in the Zen Dots title font, steel-blue
//     (accent2), with a full-width 2px steel-blue rule beneath.
//   - Right side of the header carries a count + a "refreshing..." /
//     "updated Ns ago" indicator, both in dim cream.
//   - Body is a vertical list of borderless rows. Focused row gets a 3px
//     gold outline (same idiom as the home menu's playlist cursor); the
//     rest of the rows have NO chrome — pure background. A blinking ◂
//     marker rides the right edge of the focused row, on the same 500ms
//     phase as Detail's button cursor.
//   - Progress bars are outline-only tracks with a colored fill (green
//     for active, gold for paused/queued, red for failed) — borrowing the
//     border-and-text idiom from the home menu's volume slider.
//   - Footer is a centered, dim small-font hint, no background fill.
//
// Drop fills wherever possible: it's all borders and text, the way the
// kiosk's other surfaces look.

// Outer padding and header geometry — kept identical to DetailScreen so
// the header rule, footer hint, and side-padding align perfectly when the
// user navigates between screens.
constexpr float kPaddingX        = 32.0f;
constexpr float kHeaderBaselineY = 38.0f;     // baseline of header text
constexpr float kHeaderRuleY     = 58.0f;     // Y of the 2px steel-blue rule

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

// Deterministic colored tint for a queue item poster fallback. Hashing on
// queue id (rather than movie id) keeps the placeholder color stable for
// the lifetime of a download row even if Radarr renumbers things between
// refreshes — same hash idiom Detail uses for poster_tint_for_tmdb().
::ui::Color tint_for_queue_id(int queue_id) {
    uint32_t h = static_cast<uint32_t>(queue_id) * 2654435761u;  // Knuth hash
    uint8_t r = 64 + static_cast<uint8_t>((h >>  0) & 0x7F);
    uint8_t g = 40 + static_cast<uint8_t>((h >>  8) & 0x5F);
    uint8_t b = 80 + static_cast<uint8_t>((h >> 16) & 0x7F);
    return {r, g, b, 255};
}

// Truncate `text` with a trailing ellipsis if it exceeds max_w at
// font_size. Same helper Detail uses — mirrored here so the row's title
// degrades gracefully when the poster + progress bar squeeze the middle
// column on narrow displays.
std::string truncate_to_width(::ui::Renderer& r, const std::string& text,
                              int font_size, float max_w) {
    if (r.mb_text_width(text, font_size) <= max_w) return text;
    const std::string ellipsis = "...";
    for (size_t n = text.size(); n > 0; --n) {
        std::string candidate = text.substr(0, n) + ellipsis;
        if (r.mb_text_width(candidate, font_size) <= max_w) return candidate;
    }
    return ellipsis;
}

// Human-readable rate: "1.2 MB/s", "480 KB/s", "0 B/s".
std::string format_rate(int bps) {
    if (bps <= 0) return "0 B/s";
    double v = static_cast<double>(bps);
    const char* unit = "B/s";
    if (v >= 1024.0 * 1024.0) { v /= (1024.0 * 1024.0); unit = "MB/s"; }
    else if (v >= 1024.0)      { v /= 1024.0;            unit = "KB/s"; }
    char buf[32];
    if (v >= 100.0) snprintf(buf, sizeof(buf), "%.0f %s", v, unit);
    else            snprintf(buf, sizeof(buf), "%.1f %s", v, unit);
    return buf;
}

// ETA as "1h 23m", "12m 05s", "45s", or "--" when unknown.
std::string format_eta(int eta_seconds) {
    if (eta_seconds <= 0) return "--";
    int s = eta_seconds;
    char buf[32];
    if (s >= 3600) {
        int h = s / 3600;
        int m = (s % 3600) / 60;
        snprintf(buf, sizeof(buf), "%dh %02dm", h, m);
    } else if (s >= 60) {
        int m = s / 60;
        int r = s % 60;
        snprintf(buf, sizeof(buf), "%dm %02ds", m, r);
    } else {
        snprintf(buf, sizeof(buf), "%ds", s);
    }
    return buf;
}

// "downloading" -> "Downloading" for display. Used for the sub-line state
// label.
std::string titlecase_state(const std::string& s) {
    if (s.empty()) return "Unknown";
    std::string out = s;
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

// Human-readable byte count. Picks GB / MB / KB / B based on magnitude.
// One decimal under 100 of the chosen unit, no decimal above. Used to
// surface "1.8 / 4.5 GB" on each queue row so the user can see how
// much of the download is left to go independently of the percentage.
std::string format_bytes(int64_t b) {
    if (b <= 0) return "0 B";
    double v = static_cast<double>(b);
    const char* unit = "B";
    if (v >= 1024.0 * 1024.0 * 1024.0) {
        v /= (1024.0 * 1024.0 * 1024.0); unit = "GB";
    } else if (v >= 1024.0 * 1024.0) {
        v /= (1024.0 * 1024.0); unit = "MB";
    } else if (v >= 1024.0) {
        v /= 1024.0; unit = "KB";
    }
    char buf[32];
    if (v >= 100.0) snprintf(buf, sizeof(buf), "%.0f %s", v, unit);
    else            snprintf(buf, sizeof(buf), "%.1f %s", v, unit);
    return buf;
}

// Pick the progress-bar fill color based on download state. Mirrors the
// semantic palette used on Detail's action buttons:
//   - green (highlight1)   : healthy, downloading, completed.
//   - red   (highlight2)   : failed / warning / stalled.
//   - gold  (accent)       : queued / paused / waiting — same neutral
//                            "armed-but-idle" gold the home menu uses.
::ui::Color progress_color_for_state(const std::string& state,
                                     const ::ui::Theme& th) {
    if (state == "failed" || state == "warning" || state == "stalled") {
        return th.highlight2;
    }
    if (state == "downloading" || state == "completed") {
        return th.highlight1;
    }
    // queued, delay, paused, unknown — anything indeterminate or idle.
    return th.accent;
}

}  // namespace

QueueScreen::QueueScreen(RadarrClient& radarr, QbittorrentClient* qbit)
    : radarr_(radarr), qbit_(qbit) {}

QueueScreen::~QueueScreen() {
    // Wait for any in-flight worker before destruction so we don't
    // leave a thread holding references to a dying QueueScreen.
    if (worker_.joinable()) worker_.join();
}

void QueueScreen::enter() {
    cursor_ = 0;
    scroll_row_ = 0;
    cancel_pending_ = false;
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

    worker_ = std::thread(&QueueScreen::run_refresh, this);
}

void QueueScreen::run_refresh() {
    PendingResult r;
    r.queue = radarr_.get_queue();

    // Fetch library once. We use it for two things:
    //   1. The "awaiting release" list (monitored, no file, not in queue).
    //   2. Filling in poster_url on each queue item — Radarr's /queue
    //      API doesn't include movie images, so without this cross-ref
    //      every queue row would render the deterministic-tint
    //      placeholder instead of the actual poster.
    auto library = radarr_.get_library();

    // Build a movie_id -> poster_url lookup for the queue cross-ref.
    std::unordered_map<int, std::string> id_to_poster;
    id_to_poster.reserve(library.size());
    for (const auto& m : library) {
        if (!m.poster_url.empty()) {
            id_to_poster.emplace(m.radarr_id, m.poster_url);
        }
    }

    // Patch poster_url on queue items that came back without one.
    std::unordered_set<int> active_movie_ids;
    for (auto& q : r.queue) {
        active_movie_ids.insert(q.movie_id);
        if (q.poster_url.empty()) {
            auto it = id_to_poster.find(q.movie_id);
            if (it != id_to_poster.end()) q.poster_url = it->second;
        }
    }

    // Build "awaiting release" list — monitored library movies that
    // don't have a file yet and aren't already in the active queue.
    for (auto& m : library) {
        if (!m.monitored) continue;
        if (m.has_file) continue;
        if (active_movie_ids.count(m.radarr_id) > 0) continue;
        r.awaiting.push_back(std::move(m));
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
        if (!qbit_map.empty()) {
            for (auto& q : r.queue) {
                if (q.download_id.empty()) continue;
                // Radarr emits uppercase hash; qBit normalizes to
                // lowercase. Convert to match.
                std::string key = q.download_id;
                std::transform(key.begin(), key.end(), key.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                auto it = qbit_map.find(key);
                if (it == qbit_map.end()) continue;
                const auto& qt = it->second;
                q.progress           = qt.progress;
                q.download_rate_bps  = qt.dlspeed;
                q.upload_rate_bps    = qt.upspeed;
                q.peers              = qt.num_leechs;
                q.seeds              = qt.num_seeds;
                q.size_bytes         = qt.size;
                q.sizeleft_bytes     = qt.size - qt.downloaded;
                q.eta_seconds        = qt.eta_seconds;
                // Translate qBit's state names to the Radarr-style
                // single-word vocabulary the renderer expects.
                if (qt.state == "downloading" || qt.state == "stalledDL"
                    || qt.state == "metaDL" || qt.state == "queuedDL"
                    || qt.state == "checkingDL" || qt.state == "allocating") {
                    q.state = qt.state == "downloading" ? "downloading"
                            : qt.state == "stalledDL"   ? "stalled"
                            : "queued";
                } else if (qt.state == "uploading" || qt.state == "pausedUP"
                           || qt.state == "stalledUP" || qt.state == "queuedUP"
                           || qt.state == "checkingUP" || qt.state == "forcedUP") {
                    q.state = "completed";
                } else if (qt.state == "error" || qt.state == "missingFiles") {
                    q.state = "failed";
                } else if (qt.state == "pausedDL") {
                    q.state = "paused";
                }
            }
        }
    }

    r.error = r.queue.empty() ? radarr_.last_error() : std::string{};

    // Publish under the mutex; result_ready_ is the atomic flag the
    // main thread polls each frame. Cleared by apply_pending() when
    // the result is consumed.
    {
        std::lock_guard<std::mutex> lk(result_mtx_);
        pending_ = std::move(r);
    }
    result_ready_.store(true);
    refresh_in_flight_.store(false);
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
    awaiting_ = std::move(r.awaiting);
    last_error_ = std::move(r.error);
    last_refresh_at_ = std::chrono::steady_clock::now();
    refreshing_ = false;

    // Clamp cursor to valid range now that we have new data.
    int n = static_cast<int>(queue_.size());
    if (cursor_ >= n) cursor_ = std::max(0, n - 1);
    if (cursor_ < 0) cursor_ = 0;

    // Clear a pending cancel if the row it was attached to vanished.
    if (cancel_pending_) {
        bool still_present = false;
        for (const auto& q : queue_) {
            if (q.id == cancel_pending_queue_id_) { still_present = true; break; }
        }
        if (!still_present) {
            cancel_pending_ = false;
            cancel_pending_queue_id_ = 0;
        }
    }
}

void QueueScreen::update() {
    // Drain any worker result into the live state. Cheap — it's an
    // atomic load most frames, only takes the mutex when result is ready.
    apply_pending();

    auto now = std::chrono::steady_clock::now();

    // Expire a stale cancel confirmation.
    if (cancel_pending_) {
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - cancel_pending_at_).count();
        if (elapsed_ms >= kCancelPendingMs) {
            cancel_pending_ = false;
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
    if (queue_.empty()) return;
    if (cursor_ < 0 || cursor_ >= static_cast<int>(queue_.size())) return;
    int queue_id = queue_[cursor_].id;
    radarr_.cancel_queue_item(queue_id);
    cancel_pending_ = false;
    cancel_pending_queue_id_ = 0;
    // Force an immediate refresh so the row disappears without the user
    // waiting on the 2s poll. Async — UI thread doesn't block.
    refresh_async();
}

Screen QueueScreen::handle_input(const std::vector<platform::InputEvent>& events) {
    for (const auto& e : events) {
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return Screen::Browse;
        }

        if (e.action == platform::InputAction::ROTATE_VERTICAL && e.delta != 0) {
            if (queue_.empty()) continue;
            int n = static_cast<int>(queue_.size());
            cursor_ = std::clamp(cursor_ + e.delta, 0, n - 1);
            // Navigation clears any pending cancel so the user can't
            // accidentally confirm on the wrong row.
            if (cancel_pending_) {
                cancel_pending_ = false;
                cancel_pending_queue_id_ = 0;
            }
            continue;
        }

        // BTN2 (PLAY_PAUSE): pause/resume is not implemented on the
        // Radarr API surface we currently expose, so — per spec — punt
        // with an interim toast. The user can still pause/resume from the
        // qBittorrent web UI.
        if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
            ::ui::Toast::show(
                "Pause/resume via qBittorrent web UI (localhost:8080)");
            continue;
        }

        if (e.action == platform::InputAction::SELECT && e.pressed) {
            if (queue_.empty()) continue;
            if (cursor_ < 0 || cursor_ >= static_cast<int>(queue_.size())) continue;
            int focused_id = queue_[cursor_].id;
            if (cancel_pending_ && cancel_pending_queue_id_ == focused_id) {
                // Stage 2: confirm.
                do_cancel_focused();
            } else {
                // Stage 1: arm.
                cancel_pending_ = true;
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

    // --- Top header bar: "DOWNLOAD QUEUE" + count + refresh status ----
    // Same pattern as DetailScreen's header: a Zen Dots heading in
    // steel-blue (accent2) followed by a full-width 2px steel-blue rule.
    // The right side carries dim secondary information (count, refresh
    // status), mirroring how Detail puts "BTN4: back" on the right.
    {
        const std::string heading = "DOWNLOAD QUEUE";
        int hd_size = th.font_heading_size;
        r.mb_draw_title_text(heading, kPaddingX, kHeaderBaselineY,
                             hd_size, th.dim, 1.0f);

        // Count chip ("3 downloads") — sits just to the right of the
        // heading, dim. Uses singular/plural so it reads naturally.
        std::ostringstream cs;
        // Count includes both active downloads and awaiting-release movies
        // so the user sees the full set Radarr is managing for them.
        int total = static_cast<int>(queue_.size() + awaiting_.size());
        if (total == 1) {
            cs << "(1 monitored)";
        } else {
            cs << "(" << total << " monitored \xE2\x80\x94 " << queue_.size()
               << " downloading, " << awaiting_.size() << " awaiting)";
        }
        std::string count_text = cs.str();
        int count_size = th.font_small_size;
        int hd_w = r.mb_title_text_width(heading, hd_size);
        float count_x = kPaddingX + static_cast<float>(hd_w) + 14.0f;
        // Align the count baseline to the heading baseline so both ride
        // the same line.
        float count_y = kHeaderBaselineY + 2.0f;
        r.mb_draw_text(count_text, count_x, count_y, count_size,
                       th.dim, 0.85f);

        // Refresh indicator on the far right. "refreshing..." in green
        // (highlight1) when in flight; "updated Ns ago" in dim cream
        // otherwise. Same sub-line vertical position as the count.
        std::string ind_text;
        ::ui::Color ind_color = th.dim;
        float ind_alpha = 0.85f;
        if (refreshing_) {
            ind_text = "refreshing...";
            ind_color = th.highlight1;
            ind_alpha = 0.95f;
        } else {
            auto now = std::chrono::steady_clock::now();
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                            now - last_refresh_at_).count();
            char buf[48];
            snprintf(buf, sizeof(buf), "updated %llds ago",
                     static_cast<long long>(secs));
            ind_text = buf;
        }
        int ind_size = th.font_small_size;
        int ind_w = r.mb_text_width(ind_text, ind_size);
        float ind_x = w - kPaddingX - static_cast<float>(ind_w);
        float ind_y = kHeaderBaselineY + 2.0f;
        r.mb_draw_text(ind_text, ind_x, ind_y, ind_size, ind_color, ind_alpha);

        // Full-width 2px steel-blue rule beneath the header — IDENTICAL
        // to DetailScreen so the chrome lines up pixel-for-pixel as the
        // user crosses between screens.
        r.mb_draw_line(kPaddingX, kHeaderRuleY,
                       w - kPaddingX, kHeaderRuleY,
                       2.0f, th.dim, 0.95f);
    }

    // --- Footer hint (drawn early so we can reserve the bottom band) -
    // No background fill, no rule — DetailScreen's footer is a plain
    // centered dim small-font line, and we match. We compute and draw it
    // up front so the row-list's available height calc can use the same
    // y-extent as the visible content.
    const std::string hint = cancel_pending_
        ? "SELECT: Confirm Cancel   Rotate: nav   BTN4: back (hold: exit)"
        : "Rotate: nav   RCLICK / BTN2: cancel   BTN4: back (hold: exit)";
    const int hint_size = th.font_small_size;
    const int hint_baseline = r.mb_text_baseline(hint_size);
    const float hint_y = h - kFooterMarginY - static_cast<float>(hint_size)
                       + static_cast<float>(hint_baseline);
    // Reserve the footer band height (font + a little breathing room
    // above) so list rows don't collide with it.
    const float footer_band_top = h - kFooterMarginY
                                - static_cast<float>(hint_size) - 8.0f;

    // --- Centered states: empty queue / Radarr offline ---------------
    // Same mid-screen single-message pattern as Detail's Loading/NoTmdb
    // states: large text vertically centered between header rule and
    // footer band, with an optional dim sub-line for context.
    const float body_top    = kHeaderRuleY + 16.0f;
    const float body_bottom = footer_band_top;
    const float body_h      = body_bottom - body_top;

    // Track the y-extent consumed by the active-queue render so the
    // AWAITING RELEASE section below knows where to start drawing. Stays
    // at body_top when the queue is empty — the awaiting section then
    // claims the whole body region.
    float post_queue_y = body_top;

    if (queue_.empty() && awaiting_.empty()) {
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

            int sz2 = th.font_medium_size;
            std::string hint_msg =
                "Add a movie from Browse or Search to start a download.";
            int hw = r.mb_text_width(hint_msg, sz2);
            float hx = (w - static_cast<float>(hw)) / 2.0f;
            float hy = my + static_cast<float>(sz) * 0.9f
                     + static_cast<float>(r.mb_text_baseline(sz2));
            r.mb_draw_text(hint_msg, hx, hy, sz2, th.dim, 0.8f);
        }
    } else if (!queue_.empty()) {
        // --- Queue rows --------------------------------------------------
        // Vertical scrolling list. We clamp scroll_row_ here (in render —
        // not handle_input — because this is where we know how many rows
        // can fit on the current screen height).
        const float list_top = body_top;
        const float list_h   = body_h;
        int visible_rows = std::max(1,
            static_cast<int>(list_h / (kRowHeight + kRowGap)));
        if (cursor_ < scroll_row_) scroll_row_ = cursor_;
        if (cursor_ >= scroll_row_ + visible_rows) {
            scroll_row_ = cursor_ - visible_rows + 1;
        }
        int n = static_cast<int>(queue_.size());
        int end_row = std::min(n, scroll_row_ + visible_rows);

        const float row_x = kPaddingX;
        const float row_w = w - 2.0f * kPaddingX;

        for (int i = scroll_row_; i < end_row; ++i) {
            const auto& q = queue_[i];
            const bool focused      = (i == cursor_);
            const bool cancel_armed = cancel_pending_
                                   && cancel_pending_queue_id_ == q.id;
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
            r.mb_draw_poster_fit(q.poster_url,
                                 poster_x, poster_y, kPosterW, kPosterH,
                                 tint_for_queue_id(q.id), 1.0f);
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
            const bool is_active_dl = (q.state == "downloading"
                                       && q.download_rate_bps > 0);
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
            std::ostringstream ss;
            ss << titlecase_state(q.state);
            // Downloaded/total goes FIRST (after state) so it remains
            // visible even if the row gets truncated. On every refresh
            // tick this string changes — the most reliable "this is
            // alive" signal in the row, even more so than percentage
            // (which only ticks every ~0.5% = ~10MB). Computed from
            // sizeleft so we get exact bytes, not rounded-from-progress.
            if (q.size_bytes > 0) {
                int64_t left  = std::max<int64_t>(0, q.sizeleft_bytes);
                int64_t down  = std::max<int64_t>(0, q.size_bytes - left);
                ss << "  \xE2\x80\xA2  " << format_bytes(down)
                   << " / " << format_bytes(q.size_bytes);
            }
            if (q.download_rate_bps > 0) {
                ss << "  \xE2\x80\xA2  " << format_rate(q.download_rate_bps);
            }
            if (q.eta_seconds > 0) {
                ss << "  \xE2\x80\xA2  ETA " << format_eta(q.eta_seconds);
            }
            // Peers/seeds last — least critical, OK to truncate. Combined
            // into one cluster ("2 peers / 0 seeds") makes weak swarms
            // obvious at a glance.
            if (q.peers > 0 || q.seeds > 0) {
                ss << "  \xE2\x80\xA2  " << q.peers
                   << " peer" << (q.peers == 1 ? "" : "s")
                   << " / " << q.seeds
                   << " seed" << (q.seeds == 1 ? "" : "s");
            }
            std::string sub_line = truncate_to_width(r, ss.str(),
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
            ::ui::Color fill_color = progress_color_for_state(q.state, th);
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
            char pct_buf[16];
            // Special-case 100% (no decimal — "100.0%" is overkill) and
            // 0% (don't show "0.0%" while a torrent is queued, just "0%")
            double pct100 = pct * 100.0;
            if (pct100 >= 99.95) {
                snprintf(pct_buf, sizeof(pct_buf), "100%%");
            } else if (pct100 < 0.05) {
                snprintf(pct_buf, sizeof(pct_buf), "0%%");
            } else {
                snprintf(pct_buf, sizeof(pct_buf), "%.1f%%", pct100);
            }
            std::string pct_text = pct_buf;
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

            // Sub-line explaining the state — dim cream small-font.
            int sub_size = th.font_small_size;
            int sub_baseline = r.mb_text_baseline(sub_size);
            const std::string sub =
                "Radarr re-checks indexers every 30 minutes. Movies will "
                "auto-download when seeders are available.";
            std::string sub_drawn = truncate_to_width(r, sub, sub_size,
                                                      row_w_a);
            r.mb_draw_text(sub_drawn, row_x_a,
                           section_y + static_cast<float>(sub_baseline),
                           sub_size, th.dim, 0.85f);
            section_y += static_cast<float>(sub_size) + 14.0f;

            // One row per awaiting movie. Read-only, no cursor focus.
            int title_size = th.font_medium_size;
            int title_baseline = r.mb_text_baseline(title_size);
            float row_h_a = static_cast<float>(title_size) * 1.6f;

            int drawn_count = 0;
            for (const auto& m : awaiting_) {
                if (section_y + row_h_a > h - footer_reserve) break;
                std::ostringstream label;
                label << m.title;
                if (m.year > 0) label << " (" << m.year << ")";
                label << "  \xE2\x80\xA2  Monitored, awaiting release";
                std::string label_drawn = truncate_to_width(
                    r, label.str(), title_size, row_w_a);
                r.mb_draw_text(label_drawn, row_x_a,
                               section_y + static_cast<float>(title_baseline),
                               title_size, th.fg, 0.85f);
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
    // The bordered-key glyphs from mb_chrome replace the previous
    // centered text-only hint. Cancel-armed state still surfaces visually
    // through the "Confirm" text in the rotary key's label.
    if (cancel_pending_) {
        ::media_browser::ui::chrome::draw_footer_hints(
            r, screen_w, screen_h, {
                {"A",      "Confirm Cancel"},
                {"Rotary", "Nav"},
                {"BTN4",   "Back"},
            });
    } else {
        ::media_browser::ui::chrome::draw_footer_hints(
            r, screen_w, screen_h, {
                {"Rotary", "Nav"},
                {"A",      "Cancel"},
                {"BTN2",   "Cancel"},
                {"BTN4",   "Back"},
            });
    }
    (void)hint;
    (void)hint_size;
    (void)hint_y;
}

}  // namespace media_browser::ui
