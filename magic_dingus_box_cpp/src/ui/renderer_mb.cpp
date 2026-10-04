// ui::Renderer — Media Browser drawing: the Marquee wood frame and the mb_*
// primitives / artwork-cache plumbing the MB screens draw through.
//
// One of several translation units implementing ui::Renderer (split
// out of renderer.cpp by responsibility; the class API in renderer.h
// is unchanged). Shared private bits live in renderer_internal.h.

// Declaration-only include — the implementation TU is
// utils/stb_image_impl.cpp, so renderer edits stop recompiling stb.
#include "../utils/stb_image.h"

#include "renderer.h"
#include "renderer_internal.h"

#include "theme.h"
#include "crt_time.h"
#include "font_manager.h"
#include "settings_menu.h"
#include "controller_wizard.h"
#include "pairing_screen.h"
#include "pairing_screen_renderer.h"
#include "virtual_keyboard.h" // Added for virtual keyboard rendering
#include "qrcodegen.hpp" // QR code generation
#include "text_utf8.h"
#include "../app/app_state.h"
#include "../app/playlist_loader.h"
#include "../app/settings_persistence.h" // For getting config if needed
#include "../utils/config.h"

#ifdef MEDIA_BROWSER_ENABLED
#include "../media_browser/artwork/artwork_cache.h"
#include "../platform/platform_profile.h"
#endif

#include <GLES3/gl3.h>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <cmath>
#include <chrono>
#include <cstring> // strlen — was reaching us transitively through
                   // stb_image's implementation until that moved to its
                   // own TU (utils/stb_image_impl.cpp)
#include <iomanip>
#include <vector>
#include <algorithm> // Added for std::min/max

namespace ui {

// =====================================================================
// Marquee "TV cabinet" frame
// =====================================================================
// Renders the wood-frame overlay used ONLY on Marquee (Media Browser)
// screens. Contract:
//
//   - Asset is a single 1280×720 PNG with TRANSPARENT CENTER and an
//     opaque ~40 px wood frame painted along all four edges (mitered
//     corners, beveled inner highlight). Identical render path to the
//     existing bezel: one full-screen textured quad with alpha blending.
//   - The 40 px is baked into the PNG art — the renderer doesn't need
//     to know the thickness; it just lays the whole image at 1280×720.
//   - Layout code in later Marquee screens treats `kFrameInsetPx = 40`
//     as the safe-area inset (plus an extra ~20 px breathing room
//     between the frame's inner edge and content).
//   - Drawn on top of all Marquee content (after MB screen render,
//     before toast) so the frame visually "covers" the outer pixels
//     of the CRT-processed output, producing the design's inset-CRT
//     effect without needing a real CRT-region change.
//
// Why GL_CLAMP_TO_EDGE (not GL_REPEAT): the asset is a complete frame
// at native canvas size; we draw it 1:1 across the full screen, never
// outside [0,1] UV. Repeat mode would still work but clamping is the
// correct semantic for a non-tiled overlay.
bool Renderer::load_marquee_frame(const std::string& path) {
    if (path == marquee_frame_loaded_path_ && marquee_frame_texture_id_ != 0) {
        return true;
    }
    // Sticky failure — same per-frame retry/log-spam class as load_bezel.
    if (!path.empty() && path == failed_marquee_path_) {
        return false;
    }
    if (marquee_frame_texture_id_ != 0) {
        flush_ui_batch();
        glDeleteTextures(1, &marquee_frame_texture_id_);
        marquee_frame_texture_id_ = 0;
    }
    marquee_frame_loaded_path_ = path;
    if (path.empty()) {
        return true;
    }

    std::vector<std::string> candidates = {
        "../assets/marquee/" + path,
        "assets/marquee/" + path,
        config::get_assets_path() + "/marquee/" + path,
    };

    unsigned char* data = nullptr;
    int channels = 0;
    for (const auto& p : candidates) {
        data = stbi_load(p.c_str(), &marquee_frame_tile_w_, &marquee_frame_tile_h_, &channels, 4);
        if (data) {
            std::cout << "Loaded Marquee frame overlay: " << p
                      << " (" << marquee_frame_tile_w_ << "x" << marquee_frame_tile_h_ << ")" << std::endl;
            break;
        }
    }
    if (!data) {
        std::cerr << "Failed to load Marquee frame overlay: " << path
                  << " (won't retry until GL reset)" << std::endl;
        failed_marquee_path_ = path;
        return false;
    }

    glGenTextures(1, &marquee_frame_texture_id_);
    glBindTexture(GL_TEXTURE_2D, marquee_frame_texture_id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    flush_ui_batch();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 marquee_frame_tile_w_, marquee_frame_tile_h_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, data);
    stbi_image_free(data);
    return true;
}

void Renderer::render_marquee_frame() {
    if (marquee_frame_texture_id_ == 0) return;

    flush_ui_batch();
    glUseProgram(shader_program_);

    // Full-screen overlay using ORIGINAL dimensions, not the (possibly
    // letterboxed) content viewport — the frame is a fixed cabinet that
    // owns the whole HDMI canvas.
    const float screen_w = static_cast<float>(original_width_);
    const float screen_h = static_cast<float>(original_height_);
    flush_ui_batch();
    glUniform2f(u_screen_size_loc_, screen_w, screen_h);
    glUniform4f(u_color_loc_, 1.0f, 1.0f, 1.0f, 1.0f);
    glUniform1i(u_use_texture_loc_, 1);
    flush_ui_batch();
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(cached_uniform(shader_program_, "tex"), 0);

    flush_ui_batch();
    glEnable(GL_BLEND);
    flush_ui_batch();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, marquee_frame_texture_id_);

    float vertices[] = {
        0.0f,     0.0f,     0.0f, 0.0f,
        screen_w, 0.0f,     1.0f, 0.0f,
        0.0f,     screen_h, 0.0f, 1.0f,
        screen_w, screen_h, 1.0f, 1.0f,
    };
    flush_ui_batch();
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    glBindVertexArray(vao_);
    UI_DRAW_ARRAYS(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D, 0);
}

#ifdef MEDIA_BROWSER_ENABLED
// ---- Task 18 public drawing primitives for Media Browser screens ----

void Renderer::mb_fill_background() {
    // Fully opaque so the Media Browser overlay always fully covers any
    // video frame that may still be rendering underneath (e.g., during
    // the brief transition window before the controller's stop()
    // actually flushes the pipeline). Without this, a 6% alpha gap let
    // the previous main-UI playlist video bleed through.
    //
    // Uses bg (#1F191F) — the darker page background from the Marquee
    // palette. Cards and overlay panels use bg_lift (#2A232A) so they
    // read as elevated surfaces against this darker page. v1.6.4 operator
    // direction: page = darker bg, cards = lighter bg_lift.
    draw_quad(0.0f, 0.0f,
              static_cast<float>(width_), static_cast<float>(height_),
              theme_->bg);
}

void Renderer::mb_render_seek_bar(const app::AppState& state) {
    render_seek_bar(state);
}

void Renderer::mb_begin_2d_state() {
    // Mirror the "CRITICAL: Reset OpenGL state after mpv renders" block
    // from render(state). The Media Browser dispatcher skips render(state)
    // entirely, so without this call gst_renderer.render()'s YUV shader
    // stays bound and every subsequent mb_* draw silently writes to the
    // wrong shader's uniforms. Required before any MB screen draws.
    flush_ui_batch();
    glEnable(GL_BLEND);
    flush_ui_batch();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    flush_ui_batch();
    glDisable(GL_DITHER);
    flush_ui_batch();
    glActiveTexture(GL_TEXTURE0);

    if (shader_program_ != 0) {
        flush_ui_batch();
        glUseProgram(shader_program_);

        // Set the screenSize uniform so the shader's NDC conversion
        // uses the actual framebuffer dimensions.
        //
        // Use original_width_ / original_height_ (the framebuffer size
        // from construction), NOT width_ / height_. The latter pair
        // gets temporarily overwritten by set_content_viewport() during
        // the main-menu's 4:3 pillarbox path; if we used width_ here
        // and a future bezel/letterbox feature were added to the MB
        // dispatcher, the screenSize uniform would silently shrink to
        // the pillarbox area and every MB UI element would render at
        // the wrong scale. The MB dispatcher in main.cpp always sets a
        // fullscreen glViewport, so original_* is what matches.
        GLint loc = u_screen_size_loc_;
        if (loc >= 0) {
            flush_ui_batch();
            glUniform2f(loc,
                        static_cast<float>(original_width_),
                        static_cast<float>(original_height_));
        }
    }
}

void Renderer::mb_fill_rect(float x, float y, float w, float h,
                            const ui::Color& color, float alpha_multiplier) {
    draw_quad(x, y, w, h, color, alpha_multiplier);
}

void Renderer::mb_stroke_rect(float x, float y, float w, float h, float thickness,
                              const ui::Color& color, float alpha_multiplier) {
    // Top
    draw_quad(x, y, w, thickness, color, alpha_multiplier);
    // Bottom
    draw_quad(x, y + h - thickness, w, thickness, color, alpha_multiplier);
    // Left
    draw_quad(x, y, thickness, h, color, alpha_multiplier);
    // Right
    draw_quad(x + w - thickness, y, thickness, h, color, alpha_multiplier);
}

void Renderer::mb_fill_star(float cx, float cy, float outer_r,
                            const ui::Color& color, float alpha_multiplier) {
    // Rendered as GL_TRIANGLE_FAN: center + 10 alternating outer/inner radius
    // points + one repeat of the first ring point to close the shape. The
    // inner radius ratio (~0.382 = sin(18°)/sin(54°)) gives the classic
    // 5-point star silhouette; changing it makes the star fatter/skinnier.
    constexpr float kPi = 3.14159265358979323846f;
    constexpr int   kN  = 10;                   // outer+inner points around ring
    constexpr int   kVerts = 1 + kN + 1;        // center + ring + closing point
    const float inner_r = outer_r * 0.382f;

    // Each vertex = (x, y, u, v) matching the VAO layout used by draw_quad.
    // UVs are unused in untextured mode but must still be present for the
    // attribute stride to stay consistent with the textured path.
    float vertices[kVerts * 4];
    vertices[0] = cx;
    vertices[1] = cy;
    vertices[2] = 0.0f;
    vertices[3] = 0.0f;
    for (int i = 0; i <= kN; ++i) {
        // Start angle -pi/2 puts the first outer point straight up, so the
        // star reads as pointing at the ceiling rather than sideways.
        float angle = -kPi / 2.0f
                    + static_cast<float>(i) * (2.0f * kPi / static_cast<float>(kN));
        float r = (i % 2 == 0) ? outer_r : inner_r;
        int o = (i + 1) * 4;
        vertices[o + 0] = cx + r * std::cos(angle);
        vertices[o + 1] = cy + r * std::sin(angle);
        vertices[o + 2] = 0.0f;
        vertices[o + 3] = 0.0f;
    }

    if (batch_enabled_) {
        float su = 0.0f, sv = 0.0f;
        const uint32_t tex = solid_texture(su, sv);
        for (int i = 0; i < kVerts; ++i) {
            vertices[i * 4 + 2] = su;
            vertices[i * 4 + 3] = sv;
        }
        batch_reserve(tex);
        batch_.fan(tex, vertices, kVerts,
                   {color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
                    (color.a / 255.0f) * ui_alpha_ * alpha_multiplier});
        batch_done();
        return;
    }

    flush_ui_batch();
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glUniform4f(u_color_loc_,
                color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
                (color.a / 255.0f) * ui_alpha_ * alpha_multiplier);
    glUniform1i(u_use_texture_loc_, 0);

    glBindVertexArray(vao_);
    UI_DRAW_ARRAYS(GL_TRIANGLE_FAN, 0, kVerts);
    glBindVertexArray(0);
}

void Renderer::mb_draw_text(const std::string& text, float x, float baseline_y,
                            int font_size, const ui::Color& color,
                            float alpha_multiplier) {
    if (!body_font_manager_) return;
    draw_text(text, x, baseline_y, font_size, color, false, alpha_multiplier);
}

int Renderer::mb_text_width(const std::string& text, int font_size) {
    if (!body_font_manager_) return 0;
    return body_font_manager_->get_text_width(text, font_size);
}

int Renderer::mb_text_baseline(int font_size) {
    if (!body_font_manager_) return font_size;
    return body_font_manager_->get_baseline_at_size(font_size);
}

void Renderer::mb_draw_title_text(const std::string& text, float x, float baseline_y,
                                  int font_size, const ui::Color& color,
                                  float alpha_multiplier) {
    if (!title_font_manager_) return;
    // use_title_font=true selects the Zen Dots family — same one render_title()
    // and the "Playlists" header use, which is the look we're matching.
    draw_text(text, x, baseline_y, font_size, color, true, alpha_multiplier);
}

int Renderer::mb_title_text_width(const std::string& text, int font_size) {
    if (!title_font_manager_) return 0;
    return title_font_manager_->get_text_width(text, font_size);
}

int Renderer::mb_title_text_baseline(int font_size) {
    if (!title_font_manager_) return font_size;
    return title_font_manager_->get_baseline_at_size(font_size);
}

void Renderer::mb_draw_line(float x1, float y1, float x2, float y2,
                            float thickness, const ui::Color& color,
                            float alpha_multiplier) {
    draw_line(x1, y1, x2, y2, thickness, color, alpha_multiplier);
}

void Renderer::mb_fill_triangle(float x1, float y1, float x2, float y2,
                                float x3, float y3, const ui::Color& color,
                                float alpha_multiplier) {
    // Same VAO layout as draw_quad (4 floats per vertex: x, y, u, v).
    // UVs unused in untextured mode but stride must match.
    float vertices[] = {
        x1, y1, 0.0f, 0.0f,
        x2, y2, 0.0f, 0.0f,
        x3, y3, 0.0f, 0.0f,
    };

    if (batch_enabled_) {
        float su = 0.0f, sv = 0.0f;
        const uint32_t tex = solid_texture(su, sv);
        for (int i = 0; i < 3; ++i) {
            vertices[i * 4 + 2] = su;
            vertices[i * 4 + 3] = sv;
        }
        batch_reserve(tex);
        batch_.triangles(tex, vertices, 3,
                         {color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
                          (color.a / 255.0f) * ui_alpha_ * alpha_multiplier});
        batch_done();
        return;
    }

    flush_ui_batch();
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glUniform4f(u_color_loc_,
                color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
                (color.a / 255.0f) * ui_alpha_ * alpha_multiplier);
    glUniform1i(u_use_texture_loc_, 0);

    glBindVertexArray(vao_);
    UI_DRAW_ARRAYS(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

media_browser::ArtworkCache& Renderer::artwork_cache() {
    if (!artwork_cache_) {
        // Per-board budget (PlatformProfile::artwork_cache_budget_bytes:
        // Pi 4 64 MB, Pi 5 / Unknown 128 MB — was a fixed 256 MB). It is a
        // real GPU-memory budget: entries are charged base level + full
        // mipmap chain (ArtworkCache::texture_bytes — the old w*h*4 count
        // undercounted by ~1/3). On the Pi that memory is unswappable
        // system RAM, so MB movie playback trims it to 32 MB and a game
        // launch releases it entirely (main.cpp). detect_platform() is a
        // single small file read, done once here on first MB use.
        //
        // Disk-cache directory: /mnt/ssd/cache/posters when the USB SSD
        // is mounted (it always is when the Media Browser is in use —
        // it's where the library files live). The cache constructor
        // tolerates a missing/unwritable directory by silently falling
        // back to network-only mode, so kiosks without the SSD still
        // boot fine.
        const char* env_cache = std::getenv("MDB_ARTWORK_CACHE_DIR");
        std::string cache_dir = env_cache && *env_cache
            ? std::string(env_cache)
            : std::string("/mnt/ssd/cache/posters");
        artwork_cache_ = std::make_unique<media_browser::ArtworkCache>(
            platform::detect_platform().artwork_cache_budget_bytes,
            std::move(cache_dir));
    }
    return *artwork_cache_;
}


void Renderer::mb_draw_poster_or_tint(const std::string& url,
                                      float x, float y, float w, float h,
                                      const ui::Color& fallback_tint,
                                      float alpha_multiplier) {
    // Empty URL: just draw the fallback tint. Don't pester the cache.
    if (url.empty()) {
        draw_quad(x, y, w, h, fallback_tint, alpha_multiplier);
        return;
    }

    // Grid-sized slots get the downscaled Card variant (artwork_sizing.h)
    // whatever host the URL is on; hero slots keep the decoded size.
    uint32_t tex_id = artwork_cache().get_or_fetch(
        url, media_browser::artwork_variant_for_slot(w));
    if (tex_id == 0) {
        // Not yet loaded — draw placeholder tint, the fetch is already
        // enqueued by get_or_fetch.
        draw_quad(x, y, w, h, fallback_tint, alpha_multiplier);
        return;
    }
    draw_textured_quad(tex_id, x, y, w, h, alpha_multiplier);
}

void Renderer::mb_draw_poster_fit(const std::string& url,
                                  float x, float y, float w, float h,
                                  const ui::Color& fallback_tint,
                                  float alpha_multiplier) {
    // Empty URL: just fill the slot with the tint, no fetch.
    if (url.empty()) {
        draw_quad(x, y, w, h, fallback_tint, alpha_multiplier);
        return;
    }

    const auto variant = media_browser::artwork_variant_for_slot(w);
    uint32_t tex_id = artwork_cache().get_or_fetch(url, variant);
    if (tex_id == 0) {
        // Texture still loading — fill the whole slot so the layout doesn't
        // jump on arrival.
        draw_quad(x, y, w, h, fallback_tint, alpha_multiplier);
        return;
    }

    auto dims = artwork_cache().get_dims(url, variant);
    if (!dims || dims->w <= 0 || dims->h <= 0) {
        // Defensive — entry exists but dims unknown. Stretch like the
        // legacy variant rather than skipping the draw entirely.
        draw_textured_quad(tex_id, x, y, w, h, alpha_multiplier);
        return;
    }

    float img_aspect  = static_cast<float>(dims->w) / static_cast<float>(dims->h);
    float slot_aspect = (h > 0.0f) ? (w / h) : img_aspect;

    float out_w = w, out_h = h, out_x = x, out_y = y;
    if (img_aspect > slot_aspect) {
        // Image is wider than slot → fit width, letterbox top/bottom.
        out_h = w / img_aspect;
        out_y = y + (h - out_h) * 0.5f;
    } else {
        // Image is taller than slot → fit height, pillarbox left/right.
        out_w = h * img_aspect;
        out_x = x + (w - out_w) * 0.5f;
    }

    // Fill the full slot with a dim version of the fallback tint so the
    // letterbox/pillarbox bars are visible (not transparent). 0.35 keeps
    // them subtle without making the bars look like part of the image.
    draw_quad(x, y, w, h, fallback_tint, alpha_multiplier * 0.35f);
    draw_textured_quad(tex_id, out_x, out_y, out_w, out_h, alpha_multiplier);
}

void Renderer::begin_artwork_frame() {
    // No lazy init: a frame that never touches the cache needs no tick.
    if (artwork_cache_) {
        artwork_cache_->begin_frame();
    }
}

std::size_t Renderer::pump_artwork() {
    // Only pump if the cache exists. Avoids lazy-init forcing a
    // background thread to start on frames where Media Browser isn't
    // being used.
    if (artwork_cache_) {
        return artwork_cache_->pump();
    }
    return 0;
}
#endif

} // namespace ui
