#include "media_browser/ui/queue_screen.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

#include "media_browser/radarr/radarr_client.h"
#include "ui/renderer.h"
#include "ui/theme.h"

namespace media_browser::ui {

namespace {

// --- Layout constants (pixels) -----------------------------------------

constexpr float kTopBarHeight        = 56.0f;
constexpr float kBottomBarHeight     = 40.0f;
constexpr float kRowHeight           = 100.0f;
constexpr float kRowGap              = 10.0f;
constexpr float kRowPaddingX         = 48.0f;
constexpr float kRowInnerPadding     = 12.0f;
constexpr float kPosterW             = 60.0f;
constexpr float kPosterH             = 84.0f;
constexpr float kProgressBarW        = 320.0f;
constexpr float kProgressBarH        = 22.0f;
constexpr float kOutlineThickness    = 3.0f;

// Deterministic colored tint for a queue item. We hash on the queue row
// id rather than movie_id because the spec says "simpler: hash on queue
// item id" — and queue rows can outlive a movie record being shuffled.
::ui::Color tint_for_queue_id(int queue_id) {
    uint32_t h = static_cast<uint32_t>(queue_id) * 2654435761u;  // Knuth hash
    uint8_t r = 64 + static_cast<uint8_t>((h >>  0) & 0x7F);
    uint8_t g = 40 + static_cast<uint8_t>((h >>  8) & 0x5F);
    uint8_t b = 80 + static_cast<uint8_t>((h >> 16) & 0x7F);
    return {r, g, b, 255};
}

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

// "downloading" -> "Downloading" for display.
std::string titlecase_state(const std::string& s) {
    if (s.empty()) return "Unknown";
    std::string out = s;
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

}  // namespace

QueueScreen::QueueScreen(RadarrClient& radarr) : radarr_(radarr) {}

void QueueScreen::enter() {
    cursor_ = 0;
    scroll_row_ = 0;
    cancel_pending_ = false;
    cancel_pending_queue_id_ = 0;
    refresh();
}

void QueueScreen::refresh() {
    refreshing_ = true;
    // get_queue() is synchronous in the current client. If it becomes
    // async, refreshing_ will naturally stay true until completion.
    queue_ = radarr_.get_queue();
    refreshing_ = false;
    last_refresh_at_ = std::chrono::steady_clock::now();
    last_error_ = queue_.empty() ? radarr_.last_error() : std::string{};

    // Clamp cursor to valid range.
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

    // Periodic refresh.
    auto since_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - last_refresh_at_).count();
    if (since_ms >= kRefreshIntervalMs) {
        refresh();
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
    // waiting on the 2s poll.
    refresh();
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

    // --- Top bar ------------------------------------------------------
    r.mb_fill_rect(0.0f, 0.0f, w, kTopBarHeight, th.bg, 0.75f);
    r.mb_fill_rect(0.0f, kTopBarHeight - 1.0f, w, 1.0f, th.dim, 0.6f);

    {
        int title_size = th.font_large_size;
        int title_baseline = r.mb_text_baseline(title_size);
        float title_y = (kTopBarHeight / 2.0f) - (title_size / 2.0f)
                      + static_cast<float>(title_baseline);
        r.mb_draw_text("Download Queue", kRowPaddingX, title_y, title_size,
                       th.accent, 1.0f);

        int count_size = th.font_medium_size;
        int count_baseline = r.mb_text_baseline(count_size);
        std::ostringstream cs;
        cs << queue_.size() << (queue_.size() == 1 ? " item" : " items");
        std::string count_text = cs.str();
        int title_w = r.mb_text_width("Download Queue", title_size);
        float count_x = kRowPaddingX + static_cast<float>(title_w) + 18.0f;
        float count_y = (kTopBarHeight / 2.0f) - (count_size / 2.0f)
                      + static_cast<float>(count_baseline);
        r.mb_draw_text(count_text, count_x, count_y, count_size, th.dim, 0.85f);

        // Refresh indicator on the right. Either "refreshing..." with a
        // small dot, or "updated Xs ago".
        int ind_size = th.font_small_size;
        int ind_baseline = r.mb_text_baseline(ind_size);
        std::string ind_text;
        if (refreshing_) {
            ind_text = "refreshing...";
        } else {
            auto now = std::chrono::steady_clock::now();
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                            now - last_refresh_at_).count();
            char buf[48];
            snprintf(buf, sizeof(buf), "updated %llds ago",
                     static_cast<long long>(secs));
            ind_text = buf;
        }
        int ind_w = r.mb_text_width(ind_text, ind_size);
        float ind_x = w - kRowPaddingX - static_cast<float>(ind_w);
        float ind_y = (kTopBarHeight / 2.0f) - (ind_size / 2.0f)
                    + static_cast<float>(ind_baseline);

        // Small dot prefix when refreshing.
        if (refreshing_) {
            float dot_r = 4.0f;
            float dot_x = ind_x - (dot_r * 2.0f + 8.0f);
            float dot_y = (kTopBarHeight / 2.0f) - dot_r;
            r.mb_fill_rect(dot_x, dot_y, dot_r * 2.0f, dot_r * 2.0f,
                           th.highlight1, 0.9f);
        }
        r.mb_draw_text(ind_text, ind_x, ind_y, ind_size, th.dim, 0.85f);
    }

    // --- Main list area ----------------------------------------------
    const float list_top = kTopBarHeight + 12.0f;
    const float list_bottom = h - kBottomBarHeight;
    const float list_h = list_bottom - list_top;

    // Empty / offline states.
    if (queue_.empty()) {
        // If Radarr gave us an error string, treat as offline. Otherwise
        // empty-queue is the happy path.
        if (!last_error_.empty()) {
            int sz = th.font_large_size;
            std::string msg = "Radarr service offline";
            int mw = r.mb_text_width(msg, sz);
            float mx = (w - static_cast<float>(mw)) / 2.0f;
            float my = list_top + list_h / 2.0f
                     - static_cast<float>(sz) * 0.6f
                     + static_cast<float>(r.mb_text_baseline(sz));
            r.mb_draw_text(msg, mx, my, sz, th.highlight2, 0.95f);

            int sz2 = th.font_small_size;
            std::string detail = truncate_to_width(r, last_error_, sz2,
                                                   w - 2.0f * kRowPaddingX);
            int dw = r.mb_text_width(detail, sz2);
            float dx = (w - static_cast<float>(dw)) / 2.0f;
            float dy = my + static_cast<float>(sz) * 0.9f
                     + static_cast<float>(r.mb_text_baseline(sz2));
            r.mb_draw_text(detail, dx, dy, sz2, th.dim, 0.85f);
        } else {
            int sz = th.font_large_size;
            std::string msg = "Queue is empty — no active downloads";
            int mw = r.mb_text_width(msg, sz);
            float mx = (w - static_cast<float>(mw)) / 2.0f;
            float my = list_top + list_h / 2.0f
                     + static_cast<float>(r.mb_text_baseline(sz));
            r.mb_draw_text(msg, mx, my, sz, th.dim, 0.9f);
        }
    } else {
        // Clamp scroll so the cursor stays visible.
        int visible_rows = std::max(1,
            static_cast<int>(list_h / (kRowHeight + kRowGap)));
        if (cursor_ < scroll_row_) scroll_row_ = cursor_;
        if (cursor_ >= scroll_row_ + visible_rows) {
            scroll_row_ = cursor_ - visible_rows + 1;
        }
        int n = static_cast<int>(queue_.size());
        int end_row = std::min(n, scroll_row_ + visible_rows);

        const float row_x = kRowPaddingX;
        const float row_w = w - 2.0f * kRowPaddingX;

        for (int i = scroll_row_; i < end_row; ++i) {
            const auto& q = queue_[i];
            bool focused = (i == cursor_);
            float ry = list_top
                     + (i - scroll_row_) * (kRowHeight + kRowGap);

            // Row background: slight lift for focused rows.
            r.mb_fill_rect(row_x, ry, row_w, kRowHeight,
                           th.bg, focused ? 0.9f : 0.65f);
            r.mb_stroke_rect(row_x, ry, row_w, kRowHeight, 1.0f,
                             th.dim, 0.4f);
            if (focused) {
                r.mb_stroke_rect(row_x - kOutlineThickness / 2.0f,
                                 ry - kOutlineThickness / 2.0f,
                                 row_w + kOutlineThickness,
                                 kRowHeight + kOutlineThickness,
                                 kOutlineThickness,
                                 th.accent, 1.0f);
            }

            // --- Left: poster placeholder ---
            float poster_x = row_x + kRowInnerPadding;
            float poster_y = ry + (kRowHeight - kPosterH) / 2.0f;
            r.mb_fill_rect(poster_x, poster_y, kPosterW, kPosterH,
                           tint_for_queue_id(q.id), 1.0f);
            r.mb_stroke_rect(poster_x, poster_y, kPosterW, kPosterH,
                             1.0f, th.dim, 0.4f);

            // --- Right: progress bar (right-edge aligned) ---
            float bar_x = row_x + row_w - kRowInnerPadding - kProgressBarW;
            float bar_y = ry + (kRowHeight - kProgressBarH) / 2.0f;
            // Track
            r.mb_fill_rect(bar_x, bar_y, kProgressBarW, kProgressBarH,
                           th.dim, 0.35f);
            r.mb_stroke_rect(bar_x, bar_y, kProgressBarW, kProgressBarH,
                             1.0f, th.dim, 0.6f);
            // Fill
            double pct = std::clamp(q.progress, 0.0, 1.0);
            float fill_w = static_cast<float>(pct) * kProgressBarW;
            ::ui::Color fill_color = (q.state == "failed")
                                         ? th.highlight2
                                         : th.highlight1;
            if (fill_w > 0.0f) {
                r.mb_fill_rect(bar_x, bar_y, fill_w, kProgressBarH,
                               fill_color, 0.85f);
            }
            // Percentage overlay
            int pct_size = th.font_small_size;
            int pct_baseline = r.mb_text_baseline(pct_size);
            char pct_buf[16];
            snprintf(pct_buf, sizeof(pct_buf), "%d%%",
                     static_cast<int>(pct * 100.0 + 0.5));
            std::string pct_text = pct_buf;
            int pct_w = r.mb_text_width(pct_text, pct_size);
            float pct_x = bar_x + (kProgressBarW - static_cast<float>(pct_w)) / 2.0f;
            float pct_y = bar_y + (kProgressBarH / 2.0f)
                        - static_cast<float>(pct_size) / 2.0f
                        + static_cast<float>(pct_baseline);
            r.mb_draw_text(pct_text, pct_x, pct_y, pct_size, th.fg, 1.0f);

            // ETA under the bar, right-aligned.
            {
                int eta_size = th.font_small_size;
                int eta_baseline = r.mb_text_baseline(eta_size);
                std::string eta_text = "ETA " + format_eta(q.eta_seconds);
                int ew = r.mb_text_width(eta_text, eta_size);
                float ex = bar_x + kProgressBarW - static_cast<float>(ew);
                float ey = bar_y + kProgressBarH + 4.0f
                         + static_cast<float>(eta_baseline);
                if (ey < ry + kRowHeight - 2.0f) {
                    r.mb_draw_text(eta_text, ex, ey, eta_size, th.dim, 0.85f);
                }
            }

            // --- Middle: title + state + rate/peers ---
            float mid_x = poster_x + kPosterW + kRowInnerPadding;
            float mid_max_w = bar_x - mid_x - kRowInnerPadding;

            // Title (top).
            int title_size = th.font_medium_size;
            int title_baseline = r.mb_text_baseline(title_size);
            std::string title = q.title.empty() ? std::string("Untitled") : q.title;
            title = truncate_to_width(r, title, title_size, mid_max_w);
            float title_y = ry + kRowInnerPadding
                          + static_cast<float>(title_baseline);
            r.mb_draw_text(title, mid_x, title_y, title_size, th.fg,
                           focused ? 1.0f : 0.92f);

            // State + (optional) "Confirm Cancel" flag, middle line.
            int line_size = th.font_small_size;
            int line_baseline = r.mb_text_baseline(line_size);

            std::string state_line;
            if (cancel_pending_ && cancel_pending_queue_id_ == q.id) {
                state_line = "SELECT again to Confirm Cancel";
            } else {
                state_line = titlecase_state(q.state);
            }
            std::string state_drawn = truncate_to_width(r, state_line,
                                                        line_size, mid_max_w);
            float state_y = title_y + static_cast<float>(title_size) * 0.4f
                          + static_cast<float>(line_size) * 0.9f
                          + static_cast<float>(line_baseline) * 0.1f;
            ::ui::Color state_color =
                (cancel_pending_ && cancel_pending_queue_id_ == q.id)
                    ? th.highlight2
                    : (q.state == "failed"   ? th.highlight2
                     : q.state == "completed" ? th.highlight1
                                              : th.accent);
            r.mb_draw_text(state_drawn, mid_x, state_y, line_size,
                           state_color,
                           focused ? 1.0f : 0.9f);

            // Bottom line: rate + peers.
            std::ostringstream bs;
            bs << format_rate(q.download_rate_bps);
            if (q.peers > 0) {
                bs << "   " << q.peers << " peer" << (q.peers == 1 ? "" : "s");
            }
            std::string bottom_line = truncate_to_width(r, bs.str(),
                                                         line_size, mid_max_w);
            float bottom_y = state_y + static_cast<float>(line_size) * 1.4f;
            if (bottom_y < ry + kRowHeight - 4.0f) {
                r.mb_draw_text(bottom_line, mid_x, bottom_y, line_size,
                               th.dim, 0.85f);
            }
        }
    }

    // --- Bottom hint bar ---------------------------------------------
    float bar_y = h - kBottomBarHeight;
    r.mb_fill_rect(0.0f, bar_y, w, kBottomBarHeight, th.bg, 0.75f);
    r.mb_fill_rect(0.0f, bar_y, w, 1.0f, th.dim, 0.6f);

    std::string hint;
    if (cancel_pending_) {
        hint = "SELECT: Confirm Cancel   UP/DOWN: navigate   Menu: Back";
    } else {
        hint = "SELECT: Cancel selected   UP/DOWN: navigate   Menu: Back";
    }
    int hint_size = th.font_small_size;
    int hint_baseline = r.mb_text_baseline(hint_size);
    int hint_w = r.mb_text_width(hint, hint_size);
    float hint_x = (w - static_cast<float>(hint_w)) / 2.0f;
    float hint_y = bar_y + (kBottomBarHeight / 2.0f) - (hint_size / 2.0f)
                 + static_cast<float>(hint_baseline);
    r.mb_draw_text(hint, hint_x, hint_y, hint_size,
                   cancel_pending_ ? th.highlight2 : th.fg, 0.9f);
}

}  // namespace media_browser::ui
