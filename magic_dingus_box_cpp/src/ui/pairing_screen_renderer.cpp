// pairing_screen_renderer.cpp
//
// Implements Renderer::render_pairing_screen() and the standalone helper
// load_paired_devices().
//
// render_pairing_screen() is a private method of Renderer so it can call
// the private draw_text / draw_quad / render_qr_code helpers directly.
// It is invoked from Renderer::render() in renderer.cpp when the settings
// menu is open AND is_pairing_screen_active() is true.

#include "pairing_screen_renderer.h"
#include "renderer.h"
#include "pairing_screen.h"
#include "theme.h"
#include "font_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <json/json.h>
#include <string>

namespace ui {

// ---------------------------------------------------------------------------
// load_paired_devices — parses paired_remotes.json, safe on any error
// ---------------------------------------------------------------------------

std::vector<PairedDevice> load_paired_devices(const std::string& path) {
    std::vector<PairedDevice> out;
    std::ifstream f(path);
    if (!f) return out;
    Json::Value root;
    try {
        f >> root;
    } catch (...) {
        return out;
    }
    if (!root.isMember("devices") || !root["devices"].isArray()) return out;
    for (const auto& d : root["devices"]) {
        PairedDevice pd;
        pd.id        = d.get("id",        "").asString();
        pd.nickname  = d.get("nickname",  "").asString();
        pd.last_seen = d.get("last_seen",  0).asInt64();
        out.push_back(std::move(pd));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Renderer::render_pairing_screen — full-screen pairing overlay
// ---------------------------------------------------------------------------

void Renderer::render_pairing_screen(const PairingScreen& ps,
                                     const std::vector<PairedDevice>& paired_devices) {
    const float vw = static_cast<float>(width_);
    const float vh = static_cast<float>(height_);

    // ---- Full-screen dark background ----------------------------------------
    draw_quad(0.0f, 0.0f, vw, vh, theme_->bg, 0.92f);

    // ---- Title ---------------------------------------------------------------
    {
        const std::string title = "Phone Remote";
        int tw = title_font_manager_->get_text_width(title, theme_->font_heading_size);
        float tx = (vw - static_cast<float>(tw)) / 2.0f;
        float ty = 60.0f + title_font_manager_->get_baseline_at_size(theme_->font_heading_size);
        draw_text(title, tx, ty, theme_->font_heading_size, theme_->accent, /*use_title_font=*/true);
    }

    // ---- QR code (URL encodes pairing code so the web app can pre-fill) -----
    float qr_size = std::min(vw, vh) * 0.40f;
    float qr_x    = (vw - qr_size) / 2.0f;
    float qr_y    = 120.0f;
    {
        std::string url = "http://magicpi.local:5000/?pair=" + ps.current_code() + "&tab=remote";
        render_qr_code(url, qr_x, qr_y, qr_size, 1.0f);
    }

    // ---- 6-digit code -------------------------------------------------------
    float code_y;
    {
        const std::string& code = ps.current_code();
        int cw = title_font_manager_->get_text_width(code, theme_->font_heading_size);
        float cx = (vw - static_cast<float>(cw)) / 2.0f;
        code_y = qr_y + qr_size + 28.0f;
        float cy = code_y + title_font_manager_->get_baseline_at_size(theme_->font_heading_size);
        draw_text(code, cx, cy, theme_->font_heading_size, theme_->accent, /*use_title_font=*/true);
    }

    // ---- Expiry countdown ---------------------------------------------------
    {
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            ps.expires_at() - std::chrono::system_clock::now()).count();
        if (remaining < 0) remaining = 0;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Code refreshes in %lld:%02lld",
                      static_cast<long long>(remaining / 60),
                      static_cast<long long>(remaining % 60));
        std::string s = buf;
        int sw = body_font_manager_->get_text_width(s, theme_->font_small_size);
        float sx = (vw - static_cast<float>(sw)) / 2.0f;
        float sy = code_y + 36.0f + body_font_manager_->get_baseline_at_size(theme_->font_small_size);
        draw_text(s, sx, sy, theme_->font_small_size, theme_->dim);
    }

    // ---- Paired devices list ------------------------------------------------
    float list_top = vh - 200.0f;
    {
        const std::string label = "Paired devices";
        int lw = body_font_manager_->get_text_width(label, theme_->font_medium_size);
        float lx = (vw - static_cast<float>(lw)) / 2.0f;
        float ly = list_top + body_font_manager_->get_baseline_at_size(theme_->font_medium_size);
        draw_text(label, lx, ly, theme_->font_medium_size, theme_->fg);
    }

    if (paired_devices.empty()) {
        const std::string hint = "(none yet — scan the QR code with your phone)";
        int hw = body_font_manager_->get_text_width(hint, theme_->font_small_size);
        float hx = (vw - static_cast<float>(hw)) / 2.0f;
        float hy = list_top + 30.0f + body_font_manager_->get_baseline_at_size(theme_->font_small_size);
        draw_text(hint, hx, hy, theme_->font_small_size, theme_->dim);
    } else {
        const float row_h = 24.0f;
        for (size_t i = 0; i < paired_devices.size(); ++i) {
            const auto& d = paired_devices[i];
            float row_top = list_top + 30.0f + static_cast<float>(i) * row_h;
            float row_y   = row_top + body_font_manager_->get_baseline_at_size(theme_->font_small_size);

            // Highlight the selected row with a background band.
            if (static_cast<int>(i) == ps.selected_device_index()) {
                draw_quad(0.0f, row_top - 2.0f, vw, row_h, theme_->bg_lift, 1.0f);
            }

            // Prefix selected row with a chevron for extra clarity.
            std::string label = (static_cast<int>(i) == ps.selected_device_index())
                                ? "> " + d.nickname
                                : d.nickname;
            int nw = body_font_manager_->get_text_width(label, theme_->font_small_size);
            float nx = (vw - static_cast<float>(nw)) / 2.0f;
            draw_text(label, nx, row_y, theme_->font_small_size, theme_->fg);
        }
        // Hint: BTN2 = forget selected device.
        {
            const std::string hint = "[BTN2 forget · BTN4 back]";
            int hw = body_font_manager_->get_text_width(hint, theme_->font_small_size);
            float hx = (vw - static_cast<float>(hw)) / 2.0f;
            float hy = list_top + 30.0f
                     + static_cast<float>(paired_devices.size()) * row_h
                     + body_font_manager_->get_baseline_at_size(theme_->font_small_size);
            draw_text(hint, hx, hy, theme_->font_small_size, theme_->dim);
        }
    }
}

}  // namespace ui
