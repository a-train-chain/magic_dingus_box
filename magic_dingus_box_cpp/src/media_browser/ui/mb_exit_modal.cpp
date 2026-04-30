#include "media_browser/ui/mb_exit_modal.h"

#include "ui/renderer.h"

namespace media_browser::ui {

namespace {
constexpr int kCardW = 440;
constexpr int kCardH = 180;
constexpr float kBgDimAlpha = 0.40f;
constexpr int kBtnW = 160;
constexpr int kBtnH = 44;
constexpr int kPaddingX = 24;
constexpr int kPaddingY = 20;
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

    const auto& th = r.mb_theme();

    // Dim background
    r.mb_fill_rect(0.0f, 0.0f,
                   static_cast<float>(screen_w), static_cast<float>(screen_h),
                   th.bg, kBgDimAlpha);

    const int cx = (screen_w - kCardW) / 2;
    const int cy = (screen_h - kCardH) / 2;
    const float fcx = static_cast<float>(cx);
    const float fcy = static_cast<float>(cy);

    // Card fill
    r.mb_fill_rect(fcx, fcy, static_cast<float>(kCardW), static_cast<float>(kCardH),
                   th.bg_lift, 1.0f);
    // Gold border (2px each side)
    r.mb_fill_rect(fcx, fcy, static_cast<float>(kCardW), 2.0f, th.accent, 1.0f);
    r.mb_fill_rect(fcx, fcy + static_cast<float>(kCardH) - 2.0f,
                   static_cast<float>(kCardW), 2.0f, th.accent, 1.0f);
    r.mb_fill_rect(fcx, fcy, 2.0f, static_cast<float>(kCardH), th.accent, 1.0f);
    r.mb_fill_rect(fcx + static_cast<float>(kCardW) - 2.0f, fcy,
                   2.0f, static_cast<float>(kCardH), th.accent, 1.0f);

    // Headline
    r.mb_draw_text("Exit Marquee?",
                   fcx + static_cast<float>(kPaddingX),
                   fcy + 28.0f,
                   28, th.accent, 1.0f);
    // Subtitle
    r.mb_draw_text("Return to the main menu",
                   fcx + static_cast<float>(kPaddingX),
                   fcy + 70.0f,
                   16, th.dim, 1.0f);

    // Buttons (Cancel left, Exit right)
    const int btn_y = cy + kCardH - kBtnH - kPaddingY;
    const int cancel_x = cx + kPaddingX;
    const int exit_x   = cx + kCardW - kBtnW - kPaddingX;

    auto draw_btn = [&](int bx, const char* label, bool focused) {
        const float fbx  = static_cast<float>(bx);
        const float fby  = static_cast<float>(btn_y);
        const float fbw  = static_cast<float>(kBtnW);
        const float fbh  = static_cast<float>(kBtnH);
        const float tw   = static_cast<float>(r.mb_text_width(label, 18));
        const float tx   = fbx + (fbw - tw) / 2.0f;
        const float ty   = fby + 12.0f;

        if (focused) {
            r.mb_fill_rect(fbx, fby, fbw, fbh, th.accent, 1.0f);
            r.mb_draw_text(label, tx, ty, 18, th.bg, 1.0f);
        } else {
            r.mb_fill_rect(fbx, fby, fbw, fbh, th.bg_lift, 1.0f);
            // Gold 1px border
            r.mb_fill_rect(fbx,              fby,              fbw,  1.0f, th.accent, 1.0f);
            r.mb_fill_rect(fbx,              fby + fbh - 1.0f, fbw,  1.0f, th.accent, 1.0f);
            r.mb_fill_rect(fbx,              fby,              1.0f, fbh,  th.accent, 1.0f);
            r.mb_fill_rect(fbx + fbw - 1.0f, fby,              1.0f, fbh,  th.accent, 1.0f);
            r.mb_draw_text(label, tx, ty, 18, th.fg, 1.0f);
        }
    };

    draw_btn(cancel_x, "Cancel", !focus_exit_);
    draw_btn(exit_x,   "Exit",    focus_exit_);
}

}  // namespace media_browser::ui
