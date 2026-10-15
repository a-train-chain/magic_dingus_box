// ui::Renderer — CRT look and display frame: the kiosk bezel, the legacy CRT overlay,
// and the enhanced-CRT scene FBO -> bloom -> composite pipeline.
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

bool Renderer::load_bezel(const std::string& path) {
    // Don't reload if already loaded
    if (path == current_bezel_path_ && bezel_texture_id_ != 0) {
        return true;
    }
    // Don't re-attempt a path that already failed — callers invoke this
    // per frame, and the failure path re-probed the disk and logged to
    // stderr 60x/second (see failed_bezel_path_ in the header).
    if (!path.empty() && path == failed_bezel_path_) {
        return false;
    }
    
    // Delete old texture if exists
    if (bezel_texture_id_ != 0) {
        flush_ui_batch();
        glDeleteTextures(1, &bezel_texture_id_);
        bezel_texture_id_ = 0;
    }
    
    current_bezel_path_ = path;
    
    if (path.empty()) {
        // No bezel requested
        return true;
    }
    
    // Try multiple paths using config
    std::vector<std::string> bezel_paths = {
        "../assets/bezels/" + path,
        "assets/bezels/" + path,
        config::get_bezels_dir() + "/" + path
    };
    
    unsigned char* data = nullptr;
    int channels;
    
    for (const auto& bezel_path : bezel_paths) {
        data = stbi_load(bezel_path.c_str(), &bezel_width_, &bezel_height_, &channels, 4);
        if (data) {
            std::cout << "Loaded bezel from: " << bezel_path << " (" << bezel_width_ << "x" << bezel_height_ << ")" << std::endl;
            break;
        }
    }
    
    if (!data) {
        std::cerr << "Failed to load bezel: " << path
                  << " (won't retry until GL reset)" << std::endl;
        failed_bezel_path_ = path;
        return false;
    }
    
    glGenTextures(1, &bezel_texture_id_);
    glBindTexture(GL_TEXTURE_2D, bezel_texture_id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    flush_ui_batch();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bezel_width_, bezel_height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    
    stbi_image_free(data);
    return true;
}

void Renderer::render_bezel() {
    if (bezel_texture_id_ == 0) return;
    
    // Bind our shader program and set up projection
    flush_ui_batch();
    glUseProgram(shader_program_);
    
    // Use ORIGINAL screen dimensions for bezel (fullscreen overlay)
    // Not width_/height_ which may be reduced for 4:3 content viewport
    float bezel_w = static_cast<float>(original_width_);
    float bezel_h = static_cast<float>(original_height_);
    
    // Set screenSize uniform for the shader (uses screen coords divider)
    flush_ui_batch();
    glUniform2f(u_screen_size_loc_, bezel_w, bezel_h);
    
    // Render bezel as fullscreen textured quad
    float x = 0.0f;
    float y = 0.0f;
    
    float vertices[] = {
        x, y,             0.0f, 0.0f,
        x + bezel_w, y,   1.0f, 0.0f,
        x, y + bezel_h,   0.0f, 1.0f,
        x + bezel_w, y + bezel_h, 1.0f, 1.0f
    };
    
    flush_ui_batch();
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    
    // Use white color with full alpha to render texture as-is
    glUniform4f(u_color_loc_, 1.0f, 1.0f, 1.0f, 1.0f);
    glUniform1i(u_use_texture_loc_, 1);
    
    // Ensure we are using Texture Unit 0 and tell the shader
    flush_ui_batch();
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(cached_uniform(shader_program_, "tex"), 0);
    
    // Enable blending for transparent areas of the bezel
    flush_ui_batch();
    glEnable(GL_BLEND);
    flush_ui_batch();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    glBindTexture(GL_TEXTURE_2D, bezel_texture_id_);
    
    glBindVertexArray(vao_);
    UI_DRAW_ARRAYS(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D, 0);
}

float Renderer::crt_time_uniform(std::chrono::steady_clock::time_point now) const {
    return crt_field_override_ >= 0 ? ui::crt_field_time(crt_field_override_)
                                    : ui::crt_shader_time(now);
}

void Renderer::destroy_scene_fbo() {
    if (scene_fbo_ != 0) {
        glDeleteFramebuffers(1, &scene_fbo_);
        scene_fbo_ = 0;
    }
    if (scene_color_tex_ != 0) {
        flush_ui_batch();
        glDeleteTextures(1, &scene_color_tex_);
        scene_color_tex_ = 0;
    }
    scene_fbo_width_ = 0;
    scene_fbo_height_ = 0;
}

void Renderer::ensure_scene_fbo(uint32_t fb_width, uint32_t fb_height) {
    // No-op if already at the right size.
    if (scene_fbo_ != 0 &&
        scene_fbo_width_ == fb_width &&
        scene_fbo_height_ == fb_height) {
        return;
    }

    // Size mismatch (display reconnect, mode change, first call) —
    // tear down and rebuild. This is rare; the screen rarely resizes.
    destroy_scene_fbo();

    if (fb_width == 0 || fb_height == 0) {
        return;  // Nothing sensible we can build at zero size.
    }

    glGenFramebuffers(1, &scene_fbo_);
    flush_ui_batch();
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_);

    glGenTextures(1, &scene_color_tex_);
    glBindTexture(GL_TEXTURE_2D, scene_color_tex_);
    // RGBA8 is the safe-everywhere format. The kiosk doesn't need HDR
    // headroom — all source content is sRGB-encoded 8-bit. Linear
    // filtering matters because the composite's sub-1px effects
    // (Phase 4 convergence offsets) sample at non-integer coords.
    flush_ui_batch();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 static_cast<GLsizei>(fb_width),
                 static_cast<GLsizei>(fb_height),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, scene_color_tex_, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "UI Renderer: scene FBO incomplete (status=0x"
                  << std::hex << status << std::dec << "); enhanced CRT "
                  << "pipeline disabled" << std::endl;
        // Restore default framebuffer so subsequent draws still show.
        flush_ui_batch();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        destroy_scene_fbo();
        return;
    }

    scene_fbo_width_ = fb_width;
    scene_fbo_height_ = fb_height;

    // Leave the FBO bound — the typical caller (begin_scene_fbo) wants
    // to draw into it next. Caller still has to set viewport. Keep
    // the texture unbound so Sampler unit 0 isn't accidentally pinned
    // to it during the scene render (we only want it bound during the
    // composite pass).
    glBindTexture(GL_TEXTURE_2D, 0);

    std::cout << "UI Renderer: scene FBO created " << fb_width << "x" << fb_height
              << " (fbo=" << scene_fbo_ << ", tex=" << scene_color_tex_ << ")"
              << std::endl;
}

bool Renderer::begin_scene_fbo(const app::AppState& state) {
    // Gate 1: feature flag must be on.
    if (!state.display_settings.enhanced_crt_enabled) return false;

    // Gate 2: composite shader must have compiled at startup.
    if (crt_composite_shader_program_ == 0) return false;

    // Gate 3: skip the Media Browser screen entirely. Its content is
    // 16:9 (movie posters / movie playback) and intentionally outside
    // the CRT-effects scope — same gating idea as the bezel skip in
    // main.cpp.
#ifdef MEDIA_BROWSER_ENABLED
    if (state.current_screen == app::AppScreen::MediaBrowser) return false;
#endif

    // Gate 4: skip when no effects are active. There's no point paying
    // the FBO indirection cost (~1ms on Pi 4) just to copy pixels
    // straight through. This matches the early-out in
    // render_crt_effects() and keeps the "all sliders off = direct
    // draw" property of the legacy path.
    const auto& s = state.display_settings;
    bool any_effect_active = (s.scanline_intensity > 0.0f ||
                              s.warmth_intensity > 0.0f ||
                              s.glow_intensity > 0.0f ||
                              s.rgb_mask_intensity > 0.0f ||
                              s.bloom_intensity > 0.0f ||
                              s.interlacing_intensity > 0.0f ||
                              s.flicker_intensity > 0.0f);
    if (!any_effect_active) return false;

    // Gate 5: lazily create the FBO (or recreate at new size).
    //
    // Size the FBO to match the actual HDMI framebuffer, not the
    // logical UI canvas. They're equal in Modern TV (so nothing
    // changes), but in CRT_NATIVE the logical canvas is 640×480
    // while the framebuffer is whatever HDMI negotiated (typically
    // 1280×720). Pre-fix, the FBO was 640×480 and the composite
    // viewport at the end of this pipeline rendered into the
    // bottom-left 640×480 of a 1280×720 framebuffer — visible on a
    // CRT as the entire UI scrunched into one corner with everything
    // else black. With the FBO at framebuffer size, UI projection
    // upscales 640×480 into the FBO the same way it upscales into
    // the default framebuffer on the legacy path, and the composite
    // is a 1:1 copy that fills the screen.
    //
    // Fallback: if main.cpp never called set_framebuffer_size() (or
    // we're in a state where it hasn't been called yet), framebuffer_*
    // is 0 — use original_* to preserve pre-fix Modern TV behavior.
    const uint32_t fbo_w = framebuffer_width_  ? framebuffer_width_  : original_width_;
    const uint32_t fbo_h = framebuffer_height_ ? framebuffer_height_ : original_height_;
    ensure_scene_fbo(fbo_w, fbo_h);
    if (scene_fbo_ == 0) return false;  // Creation failed; fall back to legacy.

    // Bind the FBO and clear it. Caller is expected to set its own
    // viewport for any letterboxed UI/video draw — those calls now
    // target our offscreen texture.
    flush_ui_batch();
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_);
    flush_ui_batch();
    glViewport(0, 0,
               static_cast<GLsizei>(scene_fbo_width_),
               static_cast<GLsizei>(scene_fbo_height_));
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    flush_ui_batch();
    glClear(GL_COLOR_BUFFER_BIT);

    scene_fbo_active_ = true;
    return true;
}

void Renderer::destroy_bloom_fbos() {
    if (bloom_a_fbo_ != 0) {
        glDeleteFramebuffers(1, &bloom_a_fbo_);
        bloom_a_fbo_ = 0;
    }
    if (bloom_a_tex_ != 0) {
        flush_ui_batch();
        glDeleteTextures(1, &bloom_a_tex_);
        bloom_a_tex_ = 0;
    }
    if (bloom_b_fbo_ != 0) {
        glDeleteFramebuffers(1, &bloom_b_fbo_);
        bloom_b_fbo_ = 0;
    }
    if (bloom_b_tex_ != 0) {
        flush_ui_batch();
        glDeleteTextures(1, &bloom_b_tex_);
        bloom_b_tex_ = 0;
    }
    bloom_a_width_ = bloom_a_height_ = 0;
    bloom_b_width_ = bloom_b_height_ = 0;
}

void Renderer::ensure_bloom_fbos(uint32_t base_w, uint32_t base_h) {
    // Target sizes: 1/2 and 1/4 of the scene FBO. Round down so we
    // don't wind up sampling past the source texture edges. Min 1px
    // to avoid GL errors at degenerate sizes.
    uint32_t a_w = (base_w >= 2) ? (base_w / 2) : 1;
    uint32_t a_h = (base_h >= 2) ? (base_h / 2) : 1;
    uint32_t b_w = (a_w >= 2) ? (a_w / 2) : 1;
    uint32_t b_h = (a_h >= 2) ? (a_h / 2) : 1;

    // Skip rebuild if dimensions unchanged and FBOs are alive.
    if (bloom_a_fbo_ != 0 && bloom_b_fbo_ != 0 &&
        bloom_a_width_ == a_w && bloom_a_height_ == a_h &&
        bloom_b_width_ == b_w && bloom_b_height_ == b_h) {
        return;
    }

    destroy_bloom_fbos();

    auto build_fbo = [](uint32_t w, uint32_t h, uint32_t& out_fbo, uint32_t& out_tex) -> bool {
        glGenFramebuffers(1, &out_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, out_fbo);

        glGenTextures(1, &out_tex);
        glBindTexture(GL_TEXTURE_2D, out_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                     static_cast<GLsizei>(w), static_cast<GLsizei>(h),
                     0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        // Bilinear filter is what gives the bloom its smooth upsample
        // when the main composite samples the quarter-res texture at
        // arbitrary screen-space coords. Clamp-to-edge avoids halo
        // smearing across the edge during corner sampling.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, out_tex, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindTexture(GL_TEXTURE_2D, 0);
        return status == GL_FRAMEBUFFER_COMPLETE;
    };

    if (!build_fbo(a_w, a_h, bloom_a_fbo_, bloom_a_tex_)) {
        std::cerr << "UI Renderer: bloom_a FBO incomplete; halation disabled" << std::endl;
        flush_ui_batch();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        destroy_bloom_fbos();
        return;
    }
    if (!build_fbo(b_w, b_h, bloom_b_fbo_, bloom_b_tex_)) {
        std::cerr << "UI Renderer: bloom_b FBO incomplete; halation disabled" << std::endl;
        flush_ui_batch();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        destroy_bloom_fbos();
        return;
    }

    flush_ui_batch();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    bloom_a_width_ = a_w; bloom_a_height_ = a_h;
    bloom_b_width_ = b_w; bloom_b_height_ = b_h;

    std::cout << "UI Renderer: bloom FBOs created — a=" << a_w << "x" << a_h
              << " (fbo=" << bloom_a_fbo_ << ", tex=" << bloom_a_tex_ << "), b="
              << b_w << "x" << b_h << " (fbo=" << bloom_b_fbo_
              << ", tex=" << bloom_b_tex_ << ")" << std::endl;
}

// Helper to render a fullscreen quad pass through the bloom downsample
// shader. Caller has already bound the destination FBO, set viewport,
// and bound the source texture on unit 0. We only set the per-pass
// uniforms, upload the quad geometry, and draw.
//
// File-scope static so it's invisible outside this translation unit
// and doesn't pollute the Renderer class header. Uses direct
// glGetUniformLocation (not Renderer::cached_uniform — no member access
// here): 4 lookups per bloom pass, a fraction of what the member-pass
// caching already saves.
namespace {
void bloom_pass_draw(uint32_t shader_program,
                     uint32_t vbo,
                     uint32_t vao,
                     uint32_t dst_w,
                     uint32_t dst_h,
                     uint32_t src_w,
                     uint32_t src_h,
                     float luma_threshold) {
    glUseProgram(shader_program);

    // Vertex shader screenSize maps screen-space coords to NDC; we set
    // it to the destination FBO dims so position 0..dst_w × 0..dst_h
    // covers the full target.
    glUniform2f(glGetUniformLocation(shader_program, "screenSize"),
                static_cast<float>(dst_w), static_cast<float>(dst_h));
    glUniform2f(glGetUniformLocation(shader_program, "srcTexelSize"),
                1.0f / static_cast<float>(src_w),
                1.0f / static_cast<float>(src_h));
    glUniform1f(glGetUniformLocation(shader_program, "lumaThreshold"),
                luma_threshold);
    glUniform1i(glGetUniformLocation(shader_program, "srcTexture"), 0);

    float vertices[] = {
        0.0f, 0.0f, 0.0f, 0.0f,
        static_cast<float>(dst_w), 0.0f, 1.0f, 0.0f,
        static_cast<float>(dst_w), static_cast<float>(dst_h), 1.0f, 1.0f,

        0.0f, 0.0f, 0.0f, 0.0f,
        static_cast<float>(dst_w), static_cast<float>(dst_h), 1.0f, 1.0f,
        0.0f, static_cast<float>(dst_h), 0.0f, 1.0f
    };

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}
}  // namespace

void Renderer::end_scene_fbo_and_composite(const app::AppState& state) {
    if (!scene_fbo_active_) return;
    scene_fbo_active_ = false;

    const auto& s = state.display_settings;

    // ===== Phase 5: build the halation chain (bloom_a → bloom_b) =====
    //
    // Run iff bloomIntensity > 0 AND the bloom shader compiled.
    // Each pass renders a fullscreen quad through the bloom downsample
    // shader: scene→bloom_a (with luma threshold), bloom_a→bloom_b
    // (no threshold). The composite below reads bloom_b as a softly-
    // blurred bright-only halation map.
    bool bloom_built = false;
    if (s.bloom_intensity > 0.0f && bloom_downsample_shader_program_ != 0) {
        ensure_bloom_fbos(scene_fbo_width_, scene_fbo_height_);
        if (bloom_a_fbo_ != 0 && bloom_b_fbo_ != 0) {
            // Pass 1: scene → bloom_a, with soft luma threshold (~0.7)
            //         so only bright pixels feed the bloom.
            flush_ui_batch();
            glBindFramebuffer(GL_FRAMEBUFFER, bloom_a_fbo_);
            flush_ui_batch();
            glViewport(0, 0,
                       static_cast<GLsizei>(bloom_a_width_),
                       static_cast<GLsizei>(bloom_a_height_));
            flush_ui_batch();
            glDisable(GL_BLEND);
            flush_ui_batch();
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, scene_color_tex_);
            bloom_pass_draw(bloom_downsample_shader_program_, vbo_, vao_,
                            bloom_a_width_, bloom_a_height_,
                            scene_fbo_width_, scene_fbo_height_,
                            /*luma_threshold=*/0.70f);

            // Pass 2: bloom_a → bloom_b, no threshold. Just a second
            //         Kawase 5-tap that further softens / widens the
            //         bloom radius.
            flush_ui_batch();
            glBindFramebuffer(GL_FRAMEBUFFER, bloom_b_fbo_);
            flush_ui_batch();
            glViewport(0, 0,
                       static_cast<GLsizei>(bloom_b_width_),
                       static_cast<GLsizei>(bloom_b_height_));
            flush_ui_batch();
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, bloom_a_tex_);
            bloom_pass_draw(bloom_downsample_shader_program_, vbo_, vao_,
                            bloom_b_width_, bloom_b_height_,
                            bloom_a_width_, bloom_a_height_,
                            /*luma_threshold=*/0.0f);

            bloom_built = true;
        }
    }

    // ===== Main composite: scene_fbo (+ bloom_b) → default FB =====
    //
    // Re-target the default framebuffer at full HDMI mode size. The
    // composite covers everything; bezel/toast follow on top.
    //
    // Use framebuffer_* (the actual HDMI mode dims), not original_*
    // (the logical UI canvas). In Modern TV these are equal and
    // behavior is unchanged. In CRT_NATIVE the logical canvas is
    // 640×480 but the framebuffer is 1280×720 — using original_*
    // here was the bug: it caused the composite to render only into
    // the bottom-left 640×480 of the screen. See begin_scene_fbo()
    // above for the matching fbo-size fix.
    //
    // Fallback path preserved for callers that haven't wired
    // set_framebuffer_size() yet (treat 0 as "unset" → original_*).
    const uint32_t fb_w = framebuffer_width_  ? framebuffer_width_  : original_width_;
    const uint32_t fb_h = framebuffer_height_ ? framebuffer_height_ : original_height_;
    flush_ui_batch();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    flush_ui_batch();
    glViewport(0, 0,
               static_cast<GLsizei>(fb_w),
               static_cast<GLsizei>(fb_h));

    flush_ui_batch();
    glUseProgram(crt_composite_shader_program_);

    // Uniform setup mirrors render_crt_effects, plus the scene + bloom textures.
    glUniform2f(cached_uniform(crt_composite_shader_program_, "screenSize"),
                static_cast<float>(original_width_),
                static_cast<float>(original_height_));

    auto now = std::chrono::steady_clock::now();
    // Wrapped, field-parity-preserving clock shared with the redraw gate
    // (ui/crt_time.h): raw uptime seconds lose sub-field precision in a
    // float after a few days up.
    float time = crt_time_uniform(now);
    glUniform1f(cached_uniform(crt_composite_shader_program_, "time"), time);

    float effective_scanline_intensity =
        last_scanlines_enabled_ ? s.scanline_intensity : 0.0f;
    glUniform1f(cached_uniform(crt_composite_shader_program_, "scanlineIntensity"),
                effective_scanline_intensity);
    glUniform1f(cached_uniform(crt_composite_shader_program_, "warmthIntensity"),
                s.warmth_intensity);
    glUniform1f(cached_uniform(crt_composite_shader_program_, "glowIntensity"),
                s.glow_intensity);
    glUniform1f(cached_uniform(crt_composite_shader_program_, "rgbMaskIntensity"),
                s.rgb_mask_intensity);
    // Bloom intensity gates BOTH the bloom build above and the
    // composite-side screen-blend. If the build failed (bloom_built
    // false) we force the composite-side intensity to 0 so the
    // shader's `if (bloomIntensity > 0.0)` branch doesn't try to
    // sample a stale or unbound bloom texture.
    glUniform1f(cached_uniform(crt_composite_shader_program_, "bloomIntensity"),
                bloom_built ? s.bloom_intensity : 0.0f);
    glUniform1f(cached_uniform(crt_composite_shader_program_, "interlacingIntensity"),
                s.interlacing_intensity);
    glUniform1f(cached_uniform(crt_composite_shader_program_, "flickerIntensity"),
                s.flicker_intensity);

    // Bind the scene texture on unit 0 for sampling.
    flush_ui_batch();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_color_tex_);
    glUniform1i(cached_uniform(crt_composite_shader_program_, "sceneTexture"), 0);

    // Bind the bloom (halation) texture on unit 1 if available; the
    // shader only reads this when its bloomIntensity uniform > 0,
    // which we've gated on bloom_built. When bloom is off we still
    // bind unit 1 to texture 0 (no-op default) to keep the sampler
    // valid on drivers that warn about unbound samplers.
    flush_ui_batch();
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, bloom_built ? bloom_b_tex_ : 0u);
    glUniform1i(cached_uniform(crt_composite_shader_program_, "bloomTexture"), 1);

    // Composite is opaque (the shader inlines the OVER blend against
    // sceneRGB). Disabling blend avoids accidental further compositing
    // against the default-FB clear color.
    flush_ui_batch();
    glDisable(GL_BLEND);

    // Fullscreen quad over the default-FB extent. The vertex shader
    // turns position/screenSize into NDC, so we use HDMI mode coords.
    float vertices[] = {
        0.0f, 0.0f, 0.0f, 0.0f,
        static_cast<float>(original_width_), 0.0f, 1.0f, 0.0f,
        static_cast<float>(original_width_), static_cast<float>(original_height_), 1.0f, 1.0f,

        0.0f, 0.0f, 0.0f, 0.0f,
        static_cast<float>(original_width_), static_cast<float>(original_height_), 1.0f, 1.0f,
        0.0f, static_cast<float>(original_height_), 0.0f, 1.0f
    };

    flush_ui_batch();
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glBindVertexArray(vao_);
    UI_DRAW_ARRAYS(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Restore the rest of the GL state subsequent overlay code expects:
    //   - default 2D shader bound (bezel/toast use it)
    //   - blending re-enabled with the standard alpha-blend func
    //   - no texture pinned to either active unit
    //   - active texture unit returned to 0 (most overlay code assumes
    //     unit 0 is current)
    flush_ui_batch();
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    flush_ui_batch();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    flush_ui_batch();
    glEnable(GL_BLEND);
    flush_ui_batch();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    flush_ui_batch();
    glUseProgram(shader_program_);
}

void Renderer::render_crt_effects(const app::AppState& state, bool scanlines_enabled) {
    if (crt_shader_program_ == 0) return;

    // Enhanced pipeline: skip the legacy procedural overlay. The same
    // effects will be applied (with extra fidelity in Phases 2+) by
    // end_scene_fbo_and_composite, which has access to the scene
    // texture. Drawing the overlay here on top of the FBO would
    // double-apply darkening/tinting once the composite runs.
    if (scene_fbo_active_) return;

    // Check if any effects are active
    const auto& s = state.display_settings;
    if (s.scanline_intensity <= 0.0f && s.warmth_intensity <= 0.0f && 
        s.glow_intensity <= 0.0f && s.rgb_mask_intensity <= 0.0f && 
        s.bloom_intensity <= 0.0f && s.interlacing_intensity <= 0.0f && 
        s.flicker_intensity <= 0.0f) {
        return;
    }
    
    flush_ui_batch();
    glUseProgram(crt_shader_program_);
    
    // Set uniforms
    glUniform2f(cached_uniform(crt_shader_program_, "screenSize"), static_cast<float>(width_), static_cast<float>(height_));
    
    auto now = std::chrono::steady_clock::now();
    // Wrapped, field-parity-preserving clock shared with the redraw gate
    // (ui/crt_time.h): raw uptime seconds lose sub-field precision in a
    // float after a few days up.
    float time = crt_time_uniform(now);
    glUniform1f(cached_uniform(crt_shader_program_, "time"), time);
    
    // Scanlines are only enabled if the UI is visible (scanlines_enabled flag)
    // OR if scanline intensity is set to a value > 0 and we want to force them?
    // User request: "except for the scan lines. Make these only present during the video UI."
    // So if scanlines_enabled is false, we force intensity to 0.
    float effective_scanline_intensity = scanlines_enabled ? s.scanline_intensity : 0.0f;
    glUniform1f(cached_uniform(crt_shader_program_, "scanlineIntensity"), effective_scanline_intensity);
    
    glUniform1f(cached_uniform(crt_shader_program_, "warmthIntensity"), s.warmth_intensity);
    glUniform1f(cached_uniform(crt_shader_program_, "glowIntensity"), s.glow_intensity);
    glUniform1f(cached_uniform(crt_shader_program_, "rgbMaskIntensity"), s.rgb_mask_intensity);
    glUniform1f(cached_uniform(crt_shader_program_, "bloomIntensity"), s.bloom_intensity);
    glUniform1f(cached_uniform(crt_shader_program_, "interlacingIntensity"), s.interlacing_intensity);
    glUniform1f(cached_uniform(crt_shader_program_, "flickerIntensity"), s.flicker_intensity);
    
    // Draw full screen quad
    // We reuse the existing VBO which has a quad from (-1,-1) to (1,1) in clip space?
    // Wait, the vertex shader expects 'position' and 'texCoord'.
    // The VBO setup in initialize() creates a quad for the whole screen.
    // Vertex shader:
    // in vec2 position;
    // in vec2 texCoord;
    // uniform vec2 screenSize;
    // normalizedPos = (position / screenSize) * 2.0 - 1.0;
    
    // So we need to pass position as screen coordinates (0..width, 0..height)
    // The VBO setup in initialize() creates a quad:
    // 0, 0, 0, 0
    // width, 0, 1, 0
    // width, height, 1, 1
    // 0, height, 0, 1
    // Wait, I need to check initialize() again to be sure about VBO content.
    // But draw_quad uses it.
    
    // Let's just use draw_quad? No, draw_quad uses shader_program_.
    // We need to manually draw using crt_shader_program_.
    
    // Re-upload quad data to VBO if needed?
    // draw_quad uploads data every time.
    // Let's do the same here for simplicity.
    
    float vertices[] = {
        0.0f, 0.0f, 0.0f, 0.0f,
        static_cast<float>(width_), 0.0f, 1.0f, 0.0f,
        static_cast<float>(width_), static_cast<float>(height_), 1.0f, 1.0f,
        
        0.0f, 0.0f, 0.0f, 0.0f,
        static_cast<float>(width_), static_cast<float>(height_), 1.0f, 1.0f,
        0.0f, static_cast<float>(height_), 0.0f, 1.0f
    };
    
    flush_ui_batch();
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    
    glBindVertexArray(vao_);
    UI_DRAW_ARRAYS(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    
    // Restore standard shader
    flush_ui_batch();
    glUseProgram(shader_program_);
}

} // namespace ui
