#include "ui/toast.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "ui/font_manager.h"

#include <algorithm>
#include <string>
#include <vector>

namespace ui {

std::string Toast::message_;
std::chrono::steady_clock::time_point Toast::shown_at_;
bool Toast::active_ = false;

namespace {
constexpr int FADE_IN_MS = 300;
constexpr int HOLD_MS = 2400;
constexpr int FADE_OUT_MS = 300;
constexpr int TOTAL_MS = FADE_IN_MS + HOLD_MS + FADE_OUT_MS;

constexpr int kFontSize = 24;
constexpr int kLineH    = 31;  // 24 px body font at ~1.3 leading

// Greedy word-wrap by measured pixel width. The 2026-08-09 padding audit
// found long toast messages ("Media Browser unavailable — VPN tunnel
// down", "Controller setup reset — using built-in mapping") rendering
// WIDER than the fixed 480 px panel and crossing both side borders —
// the panel never measured its text. Words that individually exceed
// max_w get emitted on their own line and clipped by the caller's
// centering rather than crashing the layout (no such message exists
// today; every current toast wraps within two lines on the 480 canvas).
std::vector<std::string> wrap_to_width(FontManager& fm,
                                       const std::string& text,
                                       int font_size, int max_w) {
    std::vector<std::string> lines;
    std::string cur;
    size_t i = 0;
    while (i < text.size()) {
        size_t sp = text.find(' ', i);
        if (sp == std::string::npos) sp = text.size();
        std::string word = text.substr(i, sp - i);
        i = sp + 1;
        if (word.empty()) continue;
        std::string candidate = cur.empty() ? word : cur + " " + word;
        if (!cur.empty() && fm.get_text_width(candidate, font_size) > max_w) {
            lines.push_back(cur);
            cur = word;
        } else {
            cur = std::move(candidate);
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    if (lines.empty()) lines.push_back("");
    return lines;
}
}  // namespace

void Toast::show(std::string message) {
    message_ = std::move(message);
    shown_at_ = std::chrono::steady_clock::now();
    active_ = true;
}

void Toast::clear() {
    active_ = false;
    message_.clear();
}

bool Toast::is_active() {
    if (!active_) return false;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - shown_at_).count();
    return elapsed < TOTAL_MS;
}

void Toast::render(Renderer& r, int screen_w, int screen_h) {
    if (!active_) return;

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - shown_at_).count();

    if (elapsed_ms >= TOTAL_MS) {
        active_ = false;
        return;
    }

    // Alpha curve: fade-in → hold → fade-out
    float alpha = 1.0f;
    if (elapsed_ms < FADE_IN_MS) {
        alpha = static_cast<float>(elapsed_ms) / FADE_IN_MS;
    } else if (elapsed_ms >= FADE_IN_MS + HOLD_MS) {
        auto fade_out_elapsed = elapsed_ms - (FADE_IN_MS + HOLD_MS);
        alpha = 1.0f - static_cast<float>(fade_out_elapsed) / FADE_OUT_MS;
    }
    alpha = std::max(0.0f, std::min(1.0f, alpha));

    // Centered panel geometry — sized to the MEASURED text (audit
    // 2026-08-09): the panel grows to fit the message plus the shared
    // overlay::kCardPadX/kCardPadY insets, wraps to multiple lines when
    // the message exceeds what the canvas allows, and never drops below
    // the long-shipped 480x80 single-line look (short toasts render
    // pixel-identically to before). All sizing goes through the pure
    // helpers in theme.h so the math is unit-tested on the Mac.
    std::vector<std::string> lines;
    int longest_line_w = 0;
    if (r.body_font_manager_) {
        const int max_line_w = overlay::toast_max_line_w(screen_w);
        if (r.body_font_manager_->get_text_width(message_, kFontSize)
                <= max_line_w) {
            lines.push_back(message_);
        } else {
            lines = wrap_to_width(*r.body_font_manager_, message_,
                                  kFontSize, max_line_w);
        }
        for (const auto& ln : lines) {
            longest_line_w = std::max(
                longest_line_w,
                r.body_font_manager_->get_text_width(ln, kFontSize));
        }
    }
    const float panel_w = static_cast<float>(
        overlay::toast_panel_w(longest_line_w, screen_w));
    const float panel_h = static_cast<float>(
        overlay::toast_panel_h(static_cast<int>(lines.size()), kLineH));
    const float x = (static_cast<float>(screen_w) - panel_w) / 2.0f;
    const float y = (static_cast<float>(screen_h) - panel_h) / 2.0f;

    // Semi-transparent panel background (alpha baked into Color.a, scaled by alpha_multiplier)
    // 0.9 * alpha → 229 * alpha in 0-255 space; we pass 230 and multiply by alpha.
    ui::Color bg = r.theme_->bg;
    bg.a = 230;
    r.draw_quad(x, y, panel_w, panel_h, bg, alpha);

    // 2px border in accent color — four line segments
    const ui::Color& accent = r.theme_->accent;
    const float thickness = 2.0f;
    r.draw_line(x,            y,            x + panel_w, y,            thickness, accent, alpha);
    r.draw_line(x + panel_w,  y,            x + panel_w, y + panel_h,  thickness, accent, alpha);
    r.draw_line(x + panel_w,  y + panel_h,  x,           y + panel_h,  thickness, accent, alpha);
    r.draw_line(x,            y + panel_h,  x,           y,            thickness, accent, alpha);

    // Text centered in panel (body font, ~24pt), one baseline per
    // wrapped line, the block vertically centered as a whole so the
    // top/bottom margins stay symmetric however many lines wrap.
    if (r.body_font_manager_) {
        const int baseline = r.body_font_manager_->get_baseline_at_size(kFontSize);
        const float block_h = static_cast<float>(lines.size()) * kLineH;
        float line_top = y + (panel_h - block_h) / 2.0f;
        for (const auto& ln : lines) {
            const int text_w = r.body_font_manager_->get_text_width(ln, kFontSize);
            const float text_x = x + (panel_w - static_cast<float>(text_w)) / 2.0f;
            const float text_y = line_top
                               + (kLineH - static_cast<float>(kFontSize)) / 2.0f
                               + baseline;
            r.draw_text(ln, text_x, text_y, kFontSize, r.theme_->fg,
                        /*use_title_font=*/false, alpha);
            line_top += kLineH;
        }
    }
}

}  // namespace ui
