// ui::Renderer — Text: draw_text (glyph atlas quads, UTF-8) and format_time.
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

void Renderer::draw_text(const std::string& text, float x, float y, int font_size, const ui::Color& color, bool use_title_font, float alpha_multiplier) {
    FontManager* font_manager = use_title_font ? title_font_manager_.get() : body_font_manager_.get();
    if (!font_manager || text.empty()) {
        return;
    }
    
    // Use the requested font size
    float current_x = x;
    // Y is the baseline position in screen coordinates (Y increases downward)
    // All glyphs will be aligned to this common baseline
    float baseline_y = y;

    // Batched atlas path. Pre-atlas, this loop bound a texture, uploaded
    // a 4-vertex buffer, and issued a draw call PER GLYPH — a full menu
    // paid hundreds of driver round-trips per frame for text alone. Now
    // glyph quads accumulate into one vertex buffer and flush as a
    // single draw per atlas page (in practice: one per draw_text call —
    // a page holds hundreds of glyphs).
    //
    // Color uniform set once: it is constant across the call, and text
    // colors deliberately do NOT multiply RGB by ui_alpha_ (ui_alpha_ is
    // background transparency, not text dimming; alpha_multiplier is the
    // fade animation, which does apply).
    if (batch_enabled_) {
        // Same glyph walk as below, appended to the batch: one batch per
        // atlas page run, shared with the solids around it.
        const BatchColor bc{color.r / 255.0f, color.g / 255.0f,
                            color.b / 255.0f, (color.a / 255.0f) * alpha_multiplier};
        std::size_t bpos = 0;
        while (bpos < text.size()) {
            char32_t c = ::ui::decode_utf8(text, bpos);
            if (c == 0) break;
            if (c == U'\n') {
                int line_height = static_cast<int>(font_size * 1.2f);
                baseline_y += line_height;
                current_x = x;
                continue;
            }
            if (c == U' ') {
                ui::Glyph space_glyph = font_manager->get_glyph_at_size(U' ', font_size);
                current_x += space_glyph.advance;
                continue;
            }
            ui::Glyph glyph = font_manager->get_glyph_at_size(c, font_size);
            if (glyph.texture_id == 0) {
                current_x += glyph.advance;
                continue;
            }
            const float gx0 = current_x + glyph.bearing_x;
            const float gy0 = baseline_y - glyph.bearing_y;
            batch_reserve(glyph.texture_id);
            batch_.quad(glyph.texture_id, gx0, gy0, gx0 + glyph.width, gy0 + glyph.height,
                        glyph.u0, glyph.v0, glyph.u1, glyph.v1, bc);
            current_x += glyph.advance;
        }
        batch_done();
        return;
    }

    if (u_color_loc_ >= 0) {
        glUniform4f(u_color_loc_, color.r / 255.0f, color.g / 255.0f,
                    color.b / 255.0f, (color.a / 255.0f) * alpha_multiplier);
    }
    if (u_use_texture_loc_ >= 0) {
        glUniform1i(u_use_texture_loc_, 1);
    }

    // Reused scratch buffer (member) — clear() keeps its capacity so this
    // is allocation-free after warmup; reserve() only ever grows it.
    std::vector<float>& verts = text_scratch_verts_;
    verts.clear();
    verts.reserve(text.size() * 24);  // 6 verts x 4 floats per glyph
    uint32_t run_texture = 0;

    auto flush = [&]() {
        if (verts.empty() || run_texture == 0) return;
        glBindTexture(GL_TEXTURE_2D, run_texture);
        flush_ui_batch();
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                     verts.data(), GL_DYNAMIC_DRAW);
        glBindVertexArray(vao_);
        UI_DRAW_ARRAYS(GL_TRIANGLES, 0,
                     static_cast<GLsizei>(verts.size() / 4));
        glBindVertexArray(0);
        verts.clear();
    };

    std::size_t pos = 0;
    while (pos < text.size()) {
        char32_t c = ::ui::decode_utf8(text, pos);
        if (c == 0) break;

        if (c == U'\n') {
            // Calculate line height for this font size
            int line_height = static_cast<int>(font_size * 1.2f);  // Approximate line height
            baseline_y += line_height;
            current_x = x;
            continue;
        }

        // Skip spaces - they don't need to be rendered, just advance
        if (c == U' ') {
            // Get the space glyph just for its advance width
            ui::Glyph space_glyph = font_manager->get_glyph_at_size(U' ', font_size);
            current_x += space_glyph.advance;
            continue;
        }

        ui::Glyph glyph = font_manager->get_glyph_at_size(c, font_size);
        if (glyph.texture_id == 0) {
            // Glyph not available, just advance
            current_x += glyph.advance;
            continue;
        }

        // Rare: a glyph landed on a different atlas page than the run in
        // progress — flush the old run first.
        if (glyph.texture_id != run_texture) {
            flush();
            run_texture = glyph.texture_id;
        }

        // Same positioning math as the per-glyph path: bearing_y is the
        // distance from baseline to top of bitmap; screen Y grows down.
        const float gx0 = current_x + glyph.bearing_x;
        const float gy0 = baseline_y - glyph.bearing_y;
        const float gx1 = gx0 + glyph.width;
        const float gy1 = gy0 + glyph.height;

        // Two triangles per glyph, UVs from the glyph's atlas rect.
        const float quad[] = {
            gx0, gy0, glyph.u0, glyph.v0,
            gx1, gy0, glyph.u1, glyph.v0,
            gx0, gy1, glyph.u0, glyph.v1,
            gx1, gy0, glyph.u1, glyph.v0,
            gx1, gy1, glyph.u1, glyph.v1,
            gx0, gy1, glyph.u0, glyph.v1,
        };
        verts.insert(verts.end(), std::begin(quad), std::end(quad));

        current_x += glyph.advance;
    }

    flush();
    glBindTexture(GL_TEXTURE_2D, 0);
}

std::string Renderer::format_time(double seconds) {
    if (seconds < 0) seconds = 0;
    int total = static_cast<int>(seconds);
    int m = total / 60;
    int s = total % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    return std::string(buf);
}

} // namespace ui
