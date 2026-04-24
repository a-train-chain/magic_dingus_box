#include "ui/toast.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "ui/font_manager.h"

#include <algorithm>

namespace ui {

std::string Toast::message_;
std::chrono::steady_clock::time_point Toast::shown_at_;
bool Toast::active_ = false;

namespace {
constexpr int FADE_IN_MS = 300;
constexpr int HOLD_MS = 2400;
constexpr int FADE_OUT_MS = 300;
constexpr int TOTAL_MS = FADE_IN_MS + HOLD_MS + FADE_OUT_MS;
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

    // Centered panel geometry
    const float panel_w = 480.0f;
    const float panel_h = 80.0f;
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

    // Text centered in panel (body font, ~24pt)
    const int font_size = 24;
    if (r.body_font_manager_) {
        const int text_w = r.body_font_manager_->get_text_width(message_, font_size);
        const int baseline = r.body_font_manager_->get_baseline_at_size(font_size);
        const float text_x = x + (panel_w - static_cast<float>(text_w)) / 2.0f;
        const float text_y = y + (panel_h / 2.0f) - (font_size / 2.0f) + baseline;
        r.draw_text(message_, text_x, text_y, font_size, r.theme_->fg, /*use_title_font=*/false, alpha);
    }
}

}  // namespace ui
