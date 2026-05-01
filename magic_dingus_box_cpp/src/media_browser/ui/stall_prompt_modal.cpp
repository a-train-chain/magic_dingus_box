#include "media_browser/ui/stall_prompt_modal.h"

#include "media_browser/ui/mb_chrome.h"
#include "ui/renderer.h"

#include <utility>

namespace media_browser::ui {

namespace {
// Layout — slightly wider/taller than ExitModal because we display a
// movie title and a reason line in addition to the headline.
constexpr int   kCardW        = 560;
constexpr int   kCardH        = 240;
constexpr float kBgDimAlpha   = 0.50f;
constexpr int   kBtnW         = 200;
constexpr int   kBtnH         = 44;
constexpr int   kPaddingX     = 28;
constexpr int   kPaddingY     = 22;
constexpr int   kBtnGap       = 16;
}  // namespace

void StallPromptModal::set_handlers(PickHandler on_pick, DismissHandler on_dismiss) {
    pick_handler_    = std::move(on_pick);
    dismiss_handler_ = std::move(on_dismiss);
}

void StallPromptModal::show(Pending p) {
    pending_ = std::move(p);
    focus_   = 0;       // default to Pick another (primary action)
    active_  = true;
}

void StallPromptModal::commit_pick() {
    const Pending p = pending_;       // copy before we deactivate
    active_ = false;
    if (pick_handler_) {
        pick_handler_(p.tmdb_id, p.radarr_movie_id, p.title);
    }
}

void StallPromptModal::commit_dismiss() {
    const int tmdb_id = pending_.tmdb_id;
    active_ = false;
    if (dismiss_handler_) dismiss_handler_(tmdb_id);
}

bool StallPromptModal::handle_input(const std::vector<platform::InputEvent>& events) {
    if (!active_) return false;

    // Modal traps ALL input while active — same convention as
    // ExitModal (its dispatcher in main.cpp consumes every event when
    // is_open()). We process every event here and always return true.
    for (const auto& e : events) {
        if (!active_) break;          // a prior event already committed

        switch (e.action) {
            case platform::InputAction::PREV:
                // BTN1 — toggle focus left (to Pick another).
                if (e.pressed) focus_ = 0;
                break;

            case platform::InputAction::NEXT:
                // BTN3 — toggle focus right (to Dismiss).
                if (e.pressed) focus_ = 1;
                break;

            case platform::InputAction::ROTATE:
            case platform::InputAction::ROTATE_VERTICAL:
                // Rotary nudge — toggle focus (same convention as ExitModal).
                if (e.delta != 0) {
                    focus_ = (focus_ == 0) ? 1 : 0;
                }
                break;

            case platform::InputAction::SELECT:
                if (e.pressed) {
                    if (focus_ == 0) commit_pick();
                    else             commit_dismiss();
                }
                break;

            case platform::InputAction::SETTINGS_MENU:
                // BTN4 — explicit Dismiss shortcut (Cancel-equivalent).
                if (e.pressed) commit_dismiss();
                break;

            default:
                break;
        }
    }

    return true;
}

void StallPromptModal::render(::ui::Renderer& r, int screen_w, int screen_h) {
    if (!active_) return;

    namespace chrome = ::media_browser::ui::chrome;
    const auto& th = r.mb_theme();

    // Dim background — same alpha as ExitModal so the two modals read
    // at the same visual weight.
    r.mb_fill_rect(0.0f, 0.0f,
                   static_cast<float>(screen_w), static_cast<float>(screen_h),
                   th.bg, kBgDimAlpha);

    const int cx  = (screen_w - kCardW) / 2;
    const int cy  = (screen_h - kCardH) / 2;
    const float fcx = static_cast<float>(cx);
    const float fcy = static_cast<float>(cy);

    // Card fill — bg_lift, matching ExitModal.
    r.mb_fill_rect(fcx, fcy,
                   static_cast<float>(kCardW), static_cast<float>(kCardH),
                   th.bg_lift, 1.0f);

    // Gold 2-px border — matches ExitModal / Library / Filter overlays.
    r.mb_fill_rect(fcx, fcy, static_cast<float>(kCardW), 2.0f, th.accent, 1.0f);
    r.mb_fill_rect(fcx, fcy + static_cast<float>(kCardH) - 2.0f,
                   static_cast<float>(kCardW), 2.0f, th.accent, 1.0f);
    r.mb_fill_rect(fcx, fcy, 2.0f, static_cast<float>(kCardH), th.accent, 1.0f);
    r.mb_fill_rect(fcx + static_cast<float>(kCardW) - 2.0f, fcy,
                   2.0f, static_cast<float>(kCardH), th.accent, 1.0f);

    // Headline — gold ZenDots title, matches "EXIT MARQUEE?" baseline math.
    r.mb_draw_title_text("DOWNLOAD STALLED",
                         fcx + static_cast<float>(kPaddingX),
                         fcy + static_cast<float>(kPaddingY) + 24.0f,
                         24, th.accent);

    // Movie title — body font, foreground colour, 18 px.
    if (!pending_.title.empty()) {
        r.mb_draw_text(pending_.title,
                       fcx + static_cast<float>(kPaddingX),
                       fcy + static_cast<float>(kPaddingY) + 24.0f + 32.0f,
                       18, th.fg, 1.0f);
    }

    // Reason line — dim 16 px subtitle (mirrors ExitModal's subtitle slot).
    if (!pending_.reason_label.empty()) {
        r.mb_draw_text(pending_.reason_label,
                       fcx + static_cast<float>(kPaddingX),
                       fcy + static_cast<float>(kPaddingY) + 24.0f + 32.0f + 26.0f,
                       16, th.dim, 1.0f);
    }

    // Thin dim rule above the button row — same primitive as ExitModal.
    const float rule_y = fcy + static_cast<float>(kCardH) - static_cast<float>(kBtnH)
                       - static_cast<float>(kPaddingY) - 12.0f;
    r.mb_fill_rect(fcx + static_cast<float>(kPaddingX),
                   rule_y,
                   static_cast<float>(kCardW - 2 * kPaddingX),
                   1.0f, th.dim, 0.4f);

    // Button row — center-aligned. Two equal-width buttons separated by
    // kBtnGap. "Pick another" is the primary action (gold-filled when
    // focused), "Dismiss (10m)" is the neutral secondary.
    const int btn_y    = cy + kCardH - kBtnH - kPaddingY;
    const int row_w    = (kBtnW * 2) + kBtnGap;
    const int row_x    = cx + (kCardW - row_w) / 2;
    const int pick_x   = row_x;
    const int dismiss_x = row_x + kBtnW + kBtnGap;

    // ----- Pick another button -----
    {
        const float fbx = static_cast<float>(pick_x);
        const float fby = static_cast<float>(btn_y);
        const float fbw = static_cast<float>(kBtnW);
        const float fbh = static_cast<float>(kBtnH);
        const bool focused = (focus_ == 0);
        const char* label = "Pick another";

        if (focused) {
            // Gold fill, bg text — primary CTA, mirrors ExitModal's focused Exit.
            r.mb_fill_rect(fbx, fby, fbw, fbh, th.accent, 1.0f);
            const int tw = r.mb_text_width(label, 18);
            const float tx = fbx + (fbw - static_cast<float>(tw)) / 2.0f;
            const int baseline = r.mb_text_baseline(18);
            const float ty = fby + (fbh - 18.0f) / 2.0f + static_cast<float>(baseline);
            r.mb_draw_text(label, tx, ty, 18, th.bg, 1.0f);
            chrome::draw_focus_ring(r, pick_x, btn_y, kBtnW, kBtnH);
        } else {
            // Neutral idle: bg_lift fill, dim border, fg text.
            r.mb_fill_rect(fbx, fby, fbw, fbh, th.bg_lift, 1.0f);
            r.mb_stroke_rect(fbx, fby, fbw, fbh, 2.0f, th.dim, 1.0f);
            const int tw = r.mb_text_width(label, 18);
            const float tx = fbx + (fbw - static_cast<float>(tw)) / 2.0f;
            const int baseline = r.mb_text_baseline(18);
            const float ty = fby + (fbh - 18.0f) / 2.0f + static_cast<float>(baseline);
            r.mb_draw_text(label, tx, ty, 18, th.fg, 1.0f);
        }
    }

    // ----- Dismiss button -----
    {
        const float fbx = static_cast<float>(dismiss_x);
        const float fby = static_cast<float>(btn_y);
        const float fbw = static_cast<float>(kBtnW);
        const float fbh = static_cast<float>(kBtnH);
        const bool focused = (focus_ == 1);
        const char* label = "Dismiss (10m)";

        // Always neutral styling — no destructive intent. Focus differs
        // only by the gold border + focus ring (matches ExitModal's
        // unfocused Cancel → focused Cancel transition).
        const ::ui::Color border_c = focused ? th.accent : th.dim;
        const ::ui::Color text_c   = focused ? th.accent : th.fg;
        r.mb_fill_rect(fbx, fby, fbw, fbh, th.bg_lift, 1.0f);
        r.mb_stroke_rect(fbx, fby, fbw, fbh, 2.0f, border_c, 1.0f);
        const int tw = r.mb_text_width(label, 18);
        const float tx = fbx + (fbw - static_cast<float>(tw)) / 2.0f;
        const int baseline = r.mb_text_baseline(18);
        const float ty = fby + (fbh - 18.0f) / 2.0f + static_cast<float>(baseline);
        r.mb_draw_text(label, tx, ty, 18, text_c, 1.0f);
        if (focused) chrome::draw_focus_ring(r, dismiss_x, btn_y, kBtnW, kBtnH);
    }
}

}  // namespace media_browser::ui
