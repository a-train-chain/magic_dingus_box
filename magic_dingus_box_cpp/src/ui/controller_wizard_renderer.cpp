// controller_wizard_renderer.cpp
//
// Implements Renderer::render_controller_wizard().
//
// Same structure as pairing_screen_renderer.cpp: a private method of Renderer
// living in its own .cpp so it can call the private draw_text / draw_quad
// helpers directly. It is invoked from Renderer::render() in renderer.cpp when
// the settings menu is open AND is_controller_wizard_active() is true, and it
// replaces the sliding settings panel for that frame.
//
// All layout is written in the 1280x720 LOGICAL canvas the UI always draws
// into, via width_/height_ — never the physical mode.

#include "controller_wizard.h"
#include "renderer.h"
#include "theme.h"
#include "font_manager.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace ui {

namespace {

// Footer key legend. The box has four face buttons wired to fixed actions
// (gpio_manager.cpp): BTN1 yellow = PREV, BTN2 red = PLAY_PAUSE, BTN3 green =
// NEXT, BTN4 black = SETTINGS_MENU, rotary click = SELECT. The wizard binds
// skip/redo/cancel to PREV/PLAY_PAUSE/SETTINGS_MENU, so BTN3 is NOT a wizard
// key and must not be advertised as one — telling the user to press a button
// that does nothing is exactly the sort of dead end this screen exists to
// avoid.
constexpr const char* kCancelHint = "BTN4 / B: cancel";

}  // namespace

void Renderer::render_controller_wizard(const ControllerWizard& wiz) {
    const float vw = static_cast<float>(width_);
    const float vh = static_cast<float>(height_);

    // One line of body text, horizontally centered on the whole canvas.
    // `top` is the top of the line box; the baseline is derived from it.
    auto center_text = [&](const std::string& s, float top, int size,
                           const ui::Color& color) {
        if (s.empty()) return;
        int w = body_font_manager_->get_text_width(s, size);
        float x = (vw - static_cast<float>(w)) / 2.0f;
        float y = top + body_font_manager_->get_baseline_at_size(size);
        draw_text(s, x, y, size, color);
    };

    auto footer = [&](const std::string& s) {
        int w = body_font_manager_->get_text_width(s, theme_->font_small_size);
        float x = (vw - static_cast<float>(w)) / 2.0f;
        float y = vh - 34.0f +
                  body_font_manager_->get_baseline_at_size(theme_->font_small_size);
        draw_text(s, x, y, theme_->font_small_size, theme_->dim);
    };

    // ---- Full-screen dark background ----------------------------------------
    draw_quad(0.0f, 0.0f, vw, vh, theme_->bg, 0.95f);

    // ---- Title ---------------------------------------------------------------
    {
        const std::string title = "Controller Setup";
        int tw = title_font_manager_->get_text_width(title, theme_->font_heading_size);
        float tx = (vw - static_cast<float>(tw)) / 2.0f;
        float ty = 40.0f + title_font_manager_->get_baseline_at_size(theme_->font_heading_size);
        draw_text(title, tx, ty, theme_->font_heading_size, theme_->accent,
                  /*use_title_font=*/true);
    }

    switch (wiz.phase()) {
        // -------------------------------------------------------------------
        case ControllerWizard::Phase::PICK_DEVICE: {
            center_text("Press any button on the controller you want to set up",
                        vh * 0.40f, theme_->font_medium_size, theme_->fg);
            center_text("Phone remote and box buttons keep working for everything else",
                        vh * 0.40f + 46.0f, theme_->font_small_size, theme_->dim);
            if (!wiz.status_line().empty()) {
                center_text(wiz.status_line(), vh * 0.40f + 86.0f,
                            theme_->font_small_size, theme_->highlight2);
            }
            footer(kCancelHint);
            break;
        }

        // -------------------------------------------------------------------
        case ControllerWizard::Phase::PICK_STYLE: {
            if (!wiz.device_name().empty()) {
                center_text(wiz.device_name(), 120.0f, theme_->font_small_size,
                            theme_->dim);
            }
            center_text("Which kind of controller is this?", 170.0f,
                        theme_->font_medium_size, theme_->fg);

            const char* rows[2] = {"PlayStation-style controller",
                                   "Nintendo 64-style controller"};
            const float row_h = 54.0f;
            const float band_w = 640.0f;
            for (int i = 0; i < 2; ++i) {
                const bool sel = wiz.style_cursor() == i;
                float top = 250.0f + static_cast<float>(i) * (row_h + 12.0f);
                if (sel) {
                    draw_quad((vw - band_w) / 2.0f, top, band_w, row_h,
                              theme_->bg_lift, 1.0f);
                }
                std::string label = sel ? std::string("> ") + rows[i] : rows[i];
                int w = body_font_manager_->get_text_width(label, theme_->font_medium_size);
                float x = (vw - static_cast<float>(w)) / 2.0f;
                float y = top + (row_h - static_cast<float>(theme_->font_medium_size)) / 2.0f +
                          body_font_manager_->get_baseline_at_size(theme_->font_medium_size);
                draw_text(label, x, y, theme_->font_medium_size,
                          sel ? theme_->accent : theme_->fg);
            }
            footer(std::string("Rotate / D-pad: choose  ·  Select: confirm  ·  ") +
                   kCancelHint);
            break;
        }

        // -------------------------------------------------------------------
        case ControllerWizard::Phase::CAPTURE: {
            // Left column: the whole step list, so the user can see where
            // they are and what they already gave up on.
            const auto steps = retroarch::capture_steps(wiz.style());
            const auto& captured = wiz.captured();
            const size_t idx = wiz.step_index();
            const float list_x = 48.0f;
            const float list_top = 104.0f;
            // 24 PS-style steps have to fit between the title and the footer.
            const float row_h = std::max(
                16.0f, (vh - list_top - 60.0f) / static_cast<float>(
                                                     std::max<size_t>(steps.size(), 1)));
            for (size_t i = 0; i < steps.size(); ++i) {
                const bool done_step = i < idx;
                const bool is_current = i == idx;
                const bool got = captured.count(steps[i]) != 0;
                std::string mark = is_current ? ">" : (done_step ? (got ? "*" : "-") : " ");
                std::string label = mark + " " + short_control_label(steps[i]);
                if (done_step && !got) label += "  (skipped)";
                const ui::Color& c = is_current ? theme_->accent
                                     : (done_step ? (got ? theme_->fg : theme_->dim)
                                                  : theme_->dim);
                float y = list_top + static_cast<float>(i) * row_h +
                          body_font_manager_->get_baseline_at_size(theme_->font_small_size);
                draw_text(label, list_x, y, theme_->font_small_size, c);
            }

            // Right side: the one thing to do right now.
            const float pane_x = 360.0f;
            const float pane_w = vw - pane_x - 48.0f;
            auto pane_text = [&](const std::string& s, float top, int size,
                                 const ui::Color& color) {
                if (s.empty()) return;
                int w = body_font_manager_->get_text_width(s, size);
                float x = pane_x + (pane_w - static_cast<float>(w)) / 2.0f;
                float y = top + body_font_manager_->get_baseline_at_size(size);
                draw_text(s, x, y, size, color);
            };

            char progress[64];
            std::snprintf(progress, sizeof(progress), "Step %zu of %zu",
                          idx + 1, wiz.step_count());
            pane_text(wiz.prompt(), vh * 0.40f, theme_->font_large_size, theme_->fg);
            pane_text(progress, vh * 0.40f + 64.0f, theme_->font_small_size, theme_->dim);
            pane_text(wiz.status_line(), vh * 0.40f + 96.0f, theme_->font_small_size,
                      theme_->highlight2);

            footer(std::string("BTN2 / Play: skip  ·  BTN1 / Prev: redo  ·  ") +
                   kCancelHint);
            break;
        }

        // -------------------------------------------------------------------
        case ControllerWizard::Phase::TEST: {
            center_text("Try it out — press each control to check it lit up",
                        104.0f, theme_->font_medium_size, theme_->fg);

            const auto& captured = wiz.captured();
            const auto& lit = wiz.test_lit();
            std::vector<retroarch::LogicalControl> controls;
            controls.reserve(captured.size());
            for (const auto& [control, binding] : captured) {
                (void)binding;
                controls.push_back(control);
            }
            const size_t per_col = (controls.size() + 1) / 2;
            const float col_x[2] = {vw * 0.18f, vw * 0.58f};
            const float list_top = 160.0f;
            const float row_h = per_col == 0 ? 24.0f
                              : std::max(18.0f, (vh - list_top - 70.0f) /
                                                    static_cast<float>(per_col));
            for (size_t i = 0; i < controls.size(); ++i) {
                const size_t col = per_col == 0 ? 0 : i / per_col;
                const size_t row = per_col == 0 ? i : i % per_col;
                auto it = lit.find(controls[i]);
                const bool is_lit = it != lit.end() && it->second;
                std::string label = (is_lit ? "* " : "  ") +
                                    short_control_label(controls[i]);
                float y = list_top + static_cast<float>(row) * row_h +
                          body_font_manager_->get_baseline_at_size(theme_->font_small_size);
                draw_text(label, col_x[std::min<size_t>(col, 1)], y,
                          theme_->font_small_size,
                          is_lit ? theme_->accent : theme_->dim);
            }
            if (controls.empty()) {
                center_text("(nothing captured — every step was skipped)",
                            list_top + 40.0f, theme_->font_small_size, theme_->dim);
            }
            // Withholding save without saying why is a dead end on an
            // appliance with no other diagnostic surface. Name the controls
            // that are still outstanding, or — when the save was offered and
            // the WRITE failed (read-only /opt, full SD card) — say that,
            // just above the footer where the save prompt sits.
            if (!wiz.can_save()) {
                center_text(wiz.missing_required_line(), vh - 62.0f,
                            theme_->font_small_size, theme_->highlight2);
            } else if (!wiz.status_line().empty()) {
                center_text(wiz.status_line(), vh - 62.0f,
                            theme_->font_small_size, theme_->highlight2);
            }

            // Do NOT offer save until the essentials are captured: a profile
            // that binds too little does not degrade this pad, it disables it
            // (see ControllerWizard::can_save()). Point at the two things that
            // do work instead.
            footer(wiz.can_save()
                       ? std::string("Select: save  ·  BTN1 / Prev: start over  ·  ") +
                             kCancelHint
                       : std::string("BTN1 / Prev: start over  ·  ") + kCancelHint);
            break;
        }

        // -------------------------------------------------------------------
        case ControllerWizard::Phase::DONE: {
            {
                const std::string s = "Saved!";
                int w = title_font_manager_->get_text_width(s, theme_->font_heading_size);
                float x = (vw - static_cast<float>(w)) / 2.0f;
                float y = vh * 0.40f +
                          title_font_manager_->get_baseline_at_size(theme_->font_heading_size);
                draw_text(s, x, y, theme_->font_heading_size, theme_->highlight1,
                          /*use_title_font=*/true);
            }
            center_text(wiz.device_name(), vh * 0.40f + 56.0f,
                        theme_->font_medium_size, theme_->fg);
            footer(std::string("Select: done  ·  ") + kCancelHint);
            break;
        }
    }
}

}  // namespace ui
