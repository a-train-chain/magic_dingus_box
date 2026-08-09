#include "media_browser/ui/mb_exit_modal.h"

#include "media_browser/ui/mb_chrome.h"
#include "ui/renderer.h"

namespace media_browser::ui {

namespace {
// Interior insets from the shared overlay contract (ui/theme.h, padding
// audit 2026-08-09) — was 28/22 local magic numbers. The card grows by
// the pad delta (440x190 -> 448x198) so the interior layout is unchanged
// while the text gains the standard breathing room from the borders.
constexpr int kPaddingX = ::ui::overlay::kCardPadX;
constexpr int kPaddingY = ::ui::overlay::kCardPadY;
constexpr int kCardW = 384 + 2 * kPaddingX;   // 448: interior width preserved
constexpr int kCardH = 146 + 2 * kPaddingY;   // 198: interior height preserved
constexpr float kBgDimAlpha = 0.50f;
constexpr int kBtnW = 160;
constexpr int kBtnH = 44;      // chrome::draw_button auto-sizes, but kept for layout anchor
}  // namespace

void ExitModal::open() {
    open_ = true;
    focus_exit_ = false;            // safer default
    last_result_ = Result::Pending;
}

void ExitModal::close() {
    open_ = false;
}

bool ExitModal::on_rotate(int delta) {
    if (!open_) return false;
    if (delta == 0) return false;
    focus_exit_ = !focus_exit_;
    return true;
}

bool ExitModal::on_select() {
    if (!open_) return false;
    last_result_ = focus_exit_ ? Result::Exit : Result::Cancel;
    open_ = false;
    return true;
}

bool ExitModal::on_btn1() { return on_rotate(-1); }
bool ExitModal::on_btn3() { return on_rotate(+1); }

bool ExitModal::on_btn2() {
    if (!open_) return false;
    last_result_ = Result::Exit;
    open_ = false;
    return true;
}

bool ExitModal::on_btn4() {
    if (!open_) return false;
    last_result_ = Result::Cancel;
    open_ = false;
    return true;
}

void ExitModal::render(::ui::Renderer& r, int screen_w, int screen_h) {
    if (!open_) return;

    namespace chrome = ::media_browser::ui::chrome;
    const auto& th = r.mb_theme();

    // Dim background — slightly more opaque than before so the modal reads
    // clearly against busy poster art.
    r.mb_fill_rect(0.0f, 0.0f,
                   static_cast<float>(screen_w), static_cast<float>(screen_h),
                   th.bg, kBgDimAlpha);

    const int cx = (screen_w - kCardW) / 2;
    const int cy = (screen_h - kCardH) / 2;
    const float fcx = static_cast<float>(cx);
    const float fcy = static_cast<float>(cy);

    // Card fill — bg_lift.
    r.mb_fill_rect(fcx, fcy,
                   static_cast<float>(kCardW), static_cast<float>(kCardH),
                   th.bg_lift, 1.0f);
    // Gold border (2px each side) — matches the Library/Filter overlay panels.
    r.mb_fill_rect(fcx, fcy, static_cast<float>(kCardW), 2.0f, th.accent, 1.0f);
    r.mb_fill_rect(fcx, fcy + static_cast<float>(kCardH) - 2.0f,
                   static_cast<float>(kCardW), 2.0f, th.accent, 1.0f);
    r.mb_fill_rect(fcx, fcy, 2.0f, static_cast<float>(kCardH), th.accent, 1.0f);
    r.mb_fill_rect(fcx + static_cast<float>(kCardW) - 2.0f, fcy,
                   2.0f, static_cast<float>(kCardH), th.accent, 1.0f);

    // Headline — ZenDots title font, gold (same convention as overlay panel headings).
    // Use mb_draw_title_text so it matches "FILTERS" / "LIBRARY" / "SORT BY" headings.
    r.mb_draw_title_text("EXIT MARQUEE?",
                         fcx + static_cast<float>(kPaddingX),
                         fcy + static_cast<float>(kPaddingY) + 24.0f,
                         24, th.accent);

    // Subtitle — dim-cream body font, 16 px (matches dim label style elsewhere).
    r.mb_draw_text("Return to the main menu",
                   fcx + static_cast<float>(kPaddingX),
                   fcy + static_cast<float>(kPaddingY) + 24.0f + 32.0f,
                   16, th.dim, 1.0f);

    // Thin dim rule separating subtitle from buttons.
    const float rule_y = fcy + static_cast<float>(kCardH) - static_cast<float>(kBtnH)
                       - static_cast<float>(kPaddingY) - 12.0f;
    r.mb_fill_rect(fcx + static_cast<float>(kPaddingX),
                   rule_y,
                   static_cast<float>(kCardW - 2 * kPaddingX),
                   1.0f, th.dim, 0.4f);

    // Buttons — use chrome::draw_button so they match Detail's action row.
    // "Cancel" on the left (Neutral style when unfocused, OK when focused),
    // "Exit" on the right (Warn/red). Both show a 2px focus ring when active.
    const int btn_y = cy + kCardH - kBtnH - kPaddingY;

    // Cancel button — Neutral style (dim border) when not focused; highlighted
    // with the standard gold focus ring when focused.
    const int cancel_x = cx + kPaddingX;
    {
        // Draw a fixed-width button manually (chrome::draw_button auto-sizes
        // to content, but we want both buttons the same width so they balance).
        const float fbx = static_cast<float>(cancel_x);
        const float fby = static_cast<float>(btn_y);
        const float fbw = static_cast<float>(kBtnW);
        const float fbh = static_cast<float>(kBtnH);
        const bool focused = !focus_exit_;
        const ::ui::Color border_c = focused ? th.accent : th.dim;
        const ::ui::Color text_c   = focused ? th.accent : th.fg;
        r.mb_fill_rect(fbx, fby, fbw, fbh, th.bg_lift, 1.0f);
        r.mb_stroke_rect(fbx, fby, fbw, fbh, 2.0f, border_c, 1.0f);
        const int tw = r.mb_text_width("Cancel", 18);
        const float tx = fbx + (fbw - static_cast<float>(tw)) / 2.0f;
        const int baseline = r.mb_text_baseline(18);
        const float ty = fby + (fbh - 18.0f) / 2.0f + static_cast<float>(baseline);
        r.mb_draw_text("Cancel", tx, ty, 18, text_c, 1.0f);
        if (focused) chrome::draw_focus_ring(r, cancel_x, btn_y, kBtnW, kBtnH);
    }

    // Exit button — highlight2 (red) border/text when not focused; gold fill +
    // bg text when focused (gold focused = "confirm destructive action").
    const int exit_x = cx + kCardW - kBtnW - kPaddingX;
    {
        const float fbx = static_cast<float>(exit_x);
        const float fby = static_cast<float>(btn_y);
        const float fbw = static_cast<float>(kBtnW);
        const float fbh = static_cast<float>(kBtnH);
        const bool focused = focus_exit_;
        if (focused) {
            // Gold fill, bg text — "confirm this" primary CTA.
            r.mb_fill_rect(fbx, fby, fbw, fbh, th.accent, 1.0f);
            const int tw = r.mb_text_width("Exit", 18);
            const float tx = fbx + (fbw - static_cast<float>(tw)) / 2.0f;
            const int baseline = r.mb_text_baseline(18);
            const float ty = fby + (fbh - 18.0f) / 2.0f + static_cast<float>(baseline);
            r.mb_draw_text("Exit", tx, ty, 18, th.bg, 1.0f);
            chrome::draw_focus_ring(r, exit_x, btn_y, kBtnW, kBtnH);
        } else {
            // Dim outline, red (highlight2) border + text — destructive idle style.
            r.mb_fill_rect(fbx, fby, fbw, fbh, th.bg_lift, 1.0f);
            r.mb_stroke_rect(fbx, fby, fbw, fbh, 2.0f, th.highlight2, 1.0f);
            const int tw = r.mb_text_width("Exit", 18);
            const float tx = fbx + (fbw - static_cast<float>(tw)) / 2.0f;
            const int baseline = r.mb_text_baseline(18);
            const float ty = fby + (fbh - 18.0f) / 2.0f + static_cast<float>(baseline);
            r.mb_draw_text("Exit", tx, ty, 18, th.highlight2, 1.0f);
        }
    }
}

}  // namespace media_browser::ui
