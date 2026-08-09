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
#include "pairing_url.h"
#include "theme.h"
#include "font_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <json/json.h>
#include <string>
#include <system_error>

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
                                     const std::vector<PairedDevice>& paired_devices,
                                     const std::string& lan_ip,
                                     const std::string& hostname) {
    const float vw = static_cast<float>(width_);
    const float vh = static_cast<float>(height_);

    // ---- Full-screen dark background ----------------------------------------
    draw_quad(0.0f, 0.0f, vw, vh, theme_->bg, 0.92f);

    // Vertical gap scale. The layout is tuned on the 720-tall logical
    // canvas (Modern TV); CRT Native renders this screen at 480, where
    // the same pixel gaps would push the bottom rows off-screen. Only
    // the GAPS scale — font sizes stay at the theme values, same as
    // every other screen. (The old bottom-anchored layout overlapped
    // the QR with the paired-devices list at 480; stacking everything
    // sequentially can't overlap at any height.)
    const float s = vh / 720.0f;

    // ---- Title ---------------------------------------------------------------
    {
        // Matches the (single) Settings-menu row title and the /connect
        // web landing page — one name for the one connection surface.
        const std::string title = "Connect a Device";
        int tw = title_font_manager_->get_text_width(title, theme_->font_heading_size);
        float tx = (vw - static_cast<float>(tw)) / 2.0f;
        float ty = 44.0f * s + title_font_manager_->get_baseline_at_size(theme_->font_heading_size);
        draw_text(title, tx, ty, theme_->font_heading_size, theme_->accent, /*use_title_font=*/true);
    }

    // ---- QR code (URL encodes pairing code so the web app can pre-fill) -----
    float qr_size = std::min(vw, vh) * 0.38f;
    float qr_x    = (vw - qr_size) / 2.0f;
    float qr_y    = 108.0f * s;
    {
        // Built from the box's ACTUAL address. This was hardcoded to
        // "http://magicpi.local:5000/..." — a host that exists only on a
        // Pi named exactly "magicpi", while first_boot.sh names every
        // clone "magicpi-XXXX". The phone could not resolve it, no request
        // ever reached the server, and the pairing audit log stayed empty
        // with nothing to diagnose from. See ui/pairing_url.h.
        const std::string url =
            ui::build_pairing_url(lan_ip, hostname, ps.current_code());
        if (!url.empty()) {
            render_qr_code(url, qr_x, qr_y, qr_size, 1.0f);
        }
    }

    // ---- 6-digit code -------------------------------------------------------
    float code_y;
    {
        const std::string& code = ps.current_code();
        int cw = title_font_manager_->get_text_width(code, theme_->font_heading_size);
        float cx = (vw - static_cast<float>(cw)) / 2.0f;
        code_y = qr_y + qr_size + 24.0f * s;
        float cy = code_y + title_font_manager_->get_baseline_at_size(theme_->font_heading_size);
        draw_text(code, cx, cy, theme_->font_heading_size, theme_->accent, /*use_title_font=*/true);
    }

    // ---- Expiry countdown + typed address + USB-C line ----------------------
    float usb_top;  // top of the last stacked line, anchors the list below
    {
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            ps.expires_at() - std::chrono::system_clock::now()).count();
        if (remaining < 0) remaining = 0;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Code refreshes in %lld:%02lld",
                      static_cast<long long>(remaining / 60),
                      static_cast<long long>(remaining % 60));
        std::string countdown = buf;
        int sw = body_font_manager_->get_text_width(countdown, theme_->font_small_size);
        float sx = (vw - static_cast<float>(sw)) / 2.0f;
        float countdown_top = code_y + 40.0f * s;
        float sy = countdown_top + body_font_manager_->get_baseline_at_size(theme_->font_small_size);
        draw_text(countdown, sx, sy, theme_->font_small_size, theme_->dim);

        // The address in plain text, on its own line. A QR is a single
        // point of failure — bad lighting, a cracked lens, or a camera
        // app that refuses http:// links all leave the operator stuck
        // with no way in. Showing where to go makes a failed scan
        // recoverable. Hostname-first with the IP as the rescue — see
        // connect_typed_address in ui/pairing_url.h.
        float addr_top = countdown_top + 26.0f * s;
        const std::string addr = ui::connect_typed_address(lan_ip, hostname);
        if (!addr.empty()) {
            int aw = body_font_manager_->get_text_width(addr, theme_->font_small_size);
            float ax = (vw - static_cast<float>(aw)) / 2.0f;
            float ay = addr_top + body_font_manager_->get_baseline_at_size(theme_->font_small_size);
            draw_text(addr, ax, ay, theme_->font_small_size, theme_->fg);
        }

        // The cabled-laptop path. usb0 runs its own DHCP + catch-all DNS,
        // so over the cable ANY typed hostname reaches the box —
        // http://dingus.box is the advertised name.
        usb_top = addr_top + 26.0f * s;
        const std::string usb_line =
            "Or: plug a USB-C cable into a computer and open  http://dingus.box";
        int uw = body_font_manager_->get_text_width(usb_line, theme_->font_small_size);
        float ux = (vw - static_cast<float>(uw)) / 2.0f;
        float uy = usb_top + body_font_manager_->get_baseline_at_size(theme_->font_small_size);
        draw_text(usb_line, ux, uy, theme_->font_small_size, theme_->dim);
    }

    // ---- Paired devices list ------------------------------------------------
    // Stacked below the USB-C line rather than anchored to the bottom of
    // the screen — bottom-anchoring collided with the lines above at
    // CRT's 480-tall canvas.
    float list_top = usb_top + 40.0f * s;
    {
        const std::string label = "Paired devices";
        int lw = body_font_manager_->get_text_width(label, theme_->font_medium_size);
        float lx = (vw - static_cast<float>(lw)) / 2.0f;
        float ly = list_top + body_font_manager_->get_baseline_at_size(theme_->font_medium_size);
        draw_text(label, lx, ly, theme_->font_medium_size, theme_->fg);
    }

    if (paired_devices.empty()) {
        // Both paths, because they are not interchangeable: scanning the
        // QR opens Safari, and on iOS an installed home-screen app has a
        // SEPARATE cookie jar from Safari — so a scan can never
        // authenticate the installed app. Someone using the app has to
        // type the code. Naming only the QR sent app users down the one
        // route that cannot work for them.
        const std::string hint =
            "(none yet — scan the QR, or type the code in the app)";
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

// mtime-based shared cache. Single static here so renderer.cpp (display)
// and main.cpp (input handling) read the same list — the prior
// implementation had two independent 1 Hz time-based caches that could
// diverge for up to 1 s after a forget action.
const std::vector<PairedDevice>& paired_devices_cached(const std::string& path) {
    static std::vector<PairedDevice> cached;
    static std::filesystem::file_time_type last_mtime{};
    static bool initialized = false;

    namespace fs = std::filesystem;
    std::error_code ec;
    auto mtime = fs::last_write_time(path, ec);
    if (ec) {
        // File missing — clear cache so a deleted file is reflected.
        if (initialized) {
            cached.clear();
            last_mtime = fs::file_time_type{};
            initialized = false;
        }
        return cached;
    }
    if (!initialized || mtime != last_mtime) {
        cached = load_paired_devices(path);
        last_mtime = mtime;
        initialized = true;
    }
    return cached;
}

}  // namespace ui
