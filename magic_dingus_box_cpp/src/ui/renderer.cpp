// ui::Renderer — Renderer core: lifecycle (construction, initialize, reset_gl,
// cleanup), viewport/canvas sizing, and the immediate-mode
// primitives (quads, lines, textured quads, post-game fade).
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

Renderer::Renderer(uint32_t width, uint32_t height)
    : width_(width)
    , height_(height)
    , original_width_(width)
    , original_height_(height)
    , ui_alpha_(1.0f)
    , shader_program_(0)
    , crt_shader_program_(0)
    , crt_composite_shader_program_(0)
    , vao_(0)
    , vbo_(0)
    , logo_texture_id_(0)
    , logo_width_(0)
    , logo_height_(0)
{
    theme_ = std::make_unique<Theme>();
    title_font_manager_ = std::make_unique<FontManager>();
    body_font_manager_ = std::make_unique<FontManager>();
    batch_enabled_ = ui_batch_enabled_from_env(std::getenv("MDB_BATCH_UI"));
}

Renderer::~Renderer() {
    cleanup();
}

void Renderer::reset_gl() {
    // After an external app (like RetroArch) takes over the EGL context,
    // our GL resources are invalid. We need to delete and re-create them.
    std::cout << "UI Renderer: Resetting GL resources after external context takeover" << std::endl;
    // Batch objects belong to the dead context; pending geometry is dropped.
    destroy_batch_gl();
    
    // Delete old resources (they may be invalid but try anyway for cleanliness)
    // Program ids are being deleted (and will be recycled by GL) —
    // cached uniform locations keyed by those ids must go with them.
    uniform_loc_cache_.clear();
    if (shader_program_ != 0) {
        glDeleteProgram(shader_program_);
        shader_program_ = 0;
    }
    if (crt_shader_program_ != 0) {
        glDeleteProgram(crt_shader_program_);
        crt_shader_program_ = 0;
    }
    if (crt_composite_shader_program_ != 0) {
        glDeleteProgram(crt_composite_shader_program_);
        crt_composite_shader_program_ = 0;
    }
    if (bloom_downsample_shader_program_ != 0) {
        glDeleteProgram(bloom_downsample_shader_program_);
        bloom_downsample_shader_program_ = 0;
    }
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (vbo_ != 0) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (logo_texture_id_ != 0) {
        flush_ui_batch();
        glDeleteTextures(1, &logo_texture_id_);
        logo_texture_id_ = 0;
    }
    if (bezel_texture_id_ != 0) {
        flush_ui_batch();
        glDeleteTextures(1, &bezel_texture_id_);
        bezel_texture_id_ = 0;
        current_bezel_path_.clear();
    }
    if (marquee_frame_texture_id_ != 0) {
        flush_ui_batch();
        glDeleteTextures(1, &marquee_frame_texture_id_);
        marquee_frame_texture_id_ = 0;
        marquee_frame_loaded_path_.clear();
    }
    if (thumbnail_texture_id_ != 0) {
        flush_ui_batch();
        glDeleteTextures(1, &thumbnail_texture_id_);
        thumbnail_texture_id_ = 0;
        current_thumbnail_path_.clear();
    }
    // Tear down the scene FBO and bloom chain; they'll be lazily
    // re-created on next begin_scene_fbo() / first bloom-active frame
    // respectively. RetroArch (or any external GL takeover) may have
    // invalidated framebuffer object handles along with shader
    // programs, so we don't try to reuse them.
    destroy_scene_fbo();
    destroy_bloom_fbos();
    for (auto& pair : system_logo_cache_) {
        if (pair.second.texture_id != 0) {
            flush_ui_batch();
            glDeleteTextures(1, &pair.second.texture_id);
        }
    }
    system_logo_cache_.clear();
    // QR texture belongs to the dead context too — clear the cache key so
    // the next render_qr_code call rebuilds it in the fresh context.
    if (qr_cache_tex_ != 0) {
        flush_ui_batch();
        glDeleteTextures(1, &qr_cache_tex_);
        qr_cache_tex_ = 0;
    }
    qr_cache_url_.clear();
    // Allow one fresh bezel/marquee attempt per context rebuild — the
    // asset may have arrived since the failure was recorded.
    failed_bezel_path_.clear();
    failed_marquee_path_.clear();

    // Reset font manager GL resources (keep font data for re-rasterization)
    if (title_font_manager_) {
        title_font_manager_->reset_textures();
    }
    if (body_font_manager_) {
        body_font_manager_->reset_textures();
    }
    
    // Re-compile shaders
    if (!compile_shaders()) {
        std::cerr << "UI Renderer: Failed to re-compile shaders after reset" << std::endl;
    } else {
        std::cout << "UI Renderer: Shaders recompiled, program_id=" << shader_program_ << std::endl;
    }
    if (!compile_crt_shader()) {
        std::cerr << "UI Renderer: Failed to re-compile CRT shader after reset" << std::endl;
    } else {
        std::cout << "UI Renderer: CRT shader recompiled, program_id=" << crt_shader_program_ << std::endl;
    }
    if (!compile_crt_composite_shader()) {
        std::cerr << "UI Renderer: Failed to re-compile CRT composite shader after reset" << std::endl;
    } else {
        std::cout << "UI Renderer: CRT composite shader recompiled, program_id="
                  << crt_composite_shader_program_ << std::endl;
    }
    if (!compile_bloom_downsample_shader()) {
        std::cerr << "UI Renderer: Failed to re-compile bloom downsample shader after reset" << std::endl;
    } else {
        std::cout << "UI Renderer: Bloom downsample shader recompiled, program_id="
                  << bloom_downsample_shader_program_ << std::endl;
    }

    // Re-create VAO/VBO
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    
    glBindVertexArray(vao_);
    flush_ui_batch();
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    init_batch_gl();

    // Re-load logo texture using config paths
    unsigned char* data = nullptr;
    int channels;
    std::vector<std::string> logo_paths = config::get_logo_search_paths();
    for (const auto& logo_path : logo_paths) {
        data = stbi_load(logo_path.c_str(), &logo_width_, &logo_height_, &channels, 4);
        if (data) break;
    }
    
    if (data) {
        glGenTextures(1, &logo_texture_id_);
        glBindTexture(GL_TEXTURE_2D, logo_texture_id_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        flush_ui_batch();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, logo_width_, logo_height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        stbi_image_free(data);
    }
    
    // CRITICAL: Re-enable blending - RetroArch may have disabled it
    // Without this, all UI elements become invisible!
    flush_ui_batch();
    glEnable(GL_BLEND);
    flush_ui_batch();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    std::cout << "UI Renderer: GL resources reset complete (blending enabled)" << std::endl;
}

void Renderer::render_post_game_fade(float alpha) {
    if (alpha <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;

    // Same fullscreen idiom as render_bezel: bind the UI shader and use the
    // ORIGINAL screen dimensions, not the content-viewport ones.
    flush_ui_batch();
    glUseProgram(shader_program_);
    const float w = static_cast<float>(original_width_);
    const float h = static_cast<float>(original_height_);
    flush_ui_batch();
    glUniform2f(u_screen_size_loc_, w, h);

    float vertices[] = {
        0.0f, 0.0f, 0.0f, 0.0f,
        w,    0.0f, 1.0f, 0.0f,
        0.0f, h,    0.0f, 1.0f,
        w,    h,    1.0f, 1.0f,
    };
    flush_ui_batch();
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glUniform4f(u_color_loc_, 0.0f, 0.0f, 0.0f, alpha);
    glUniform1i(u_use_texture_loc_, 0);

    flush_ui_batch();
    glEnable(GL_BLEND);
    flush_ui_batch();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(vao_);
    UI_DRAW_ARRAYS(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

void Renderer::set_content_viewport(int width, int height) {
    // Temporarily override width and height for 4:3 rendering
    // This affects the projection matrix used by all render methods
    static bool logged = false;
    if (!logged) {
        std::cout << "UI Renderer: set_content_viewport(" << width << ", " << height << ") - was " << width_ << "x" << height_ << std::endl;
        logged = true;
    }
    width_ = static_cast<uint32_t>(width);
    height_ = static_cast<uint32_t>(height);
}

void Renderer::reset_content_viewport() {
    // Restore original screen dimensions
    width_ = original_width_;
    height_ = original_height_;
}


void Renderer::resize_screen(uint32_t width, uint32_t height) {
    original_width_ = width;
    original_height_ = height;
    reset_content_viewport();
}

void Renderer::set_framebuffer_size(uint32_t width, uint32_t height) {
    framebuffer_width_ = width;
    framebuffer_height_ = height;
}

bool Renderer::initialize(const std::string& title_font_path, const std::string& body_font_path) {
    std::cout << "  Initializing UI renderer..." << std::endl;
    std::cout << "    Title font path: " << title_font_path << std::endl;
    std::cout << "    Body font path: " << body_font_path << std::endl;
    if (!compile_shaders()) {
        return false;
    }
    
    if (!compile_crt_shader()) {
        std::cerr << "Warning: Failed to compile CRT shader, effects will be disabled" << std::endl;
    }
    if (!compile_crt_composite_shader()) {
        // Non-fatal — falling back means enhanced_crt_enabled requests
        // will silently behave like the legacy path. The legacy CRT
        // shader still works for procedural overlays.
        std::cerr << "Warning: Failed to compile CRT composite shader; "
                     "enhanced CRT pipeline disabled" << std::endl;
    }
    if (!compile_bloom_downsample_shader()) {
        // Also non-fatal — main composite will skip the bloom-sample
        // path when bloomIntensity > 0 but the chain isn't available.
        // (The composite shader itself doesn't crash on a missing
        // bloomTexture; the bind happens only when intensity > 0
        // AND the FBOs were successfully built.)
        std::cerr << "Warning: Failed to compile bloom downsample shader; "
                     "halation will be disabled" << std::endl;
    }

    // Load title font (Zen Dots) for title and heading
    if (!title_font_manager_->load_font(title_font_path, theme_->font_title_size)) {
        std::cerr << "Warning: Failed to load title font, falling back to body font" << std::endl;
        // Fallback to body font if title font fails
        if (!title_font_manager_->load_font(body_font_path, theme_->font_title_size)) {
            std::cerr << "ERROR: Failed to load any font" << std::endl;
            return false;
        }
    }
    
    // Load body font (mono) for playlist items and footer
    if (!body_font_manager_->load_font(body_font_path, theme_->font_medium_size)) {
        std::cerr << "Warning: Failed to load body font, text rendering may not work" << std::endl;
        return false;
    }
    
    // Set up VAO/VBO for quad rendering
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    
    glBindVertexArray(vao_);
    flush_ui_batch();
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    
    // Vertex attributes: position (2), texCoord (2)
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    glBindVertexArray(0);

    init_batch_gl();
    std::cout << "  UI batching: " << (batch_enabled_ ? "ON" : "OFF")
              << " (MDB_BATCH_UI=0 disables)" << std::endl;

    // Enable blending for transparency
    // Use standard alpha blending for consistent text rendering (same whether over video or not)
    flush_ui_batch();
    glEnable(GL_BLEND);
    flush_ui_batch();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Ensure smooth text rendering - disable dithering which can cause blockiness
    flush_ui_batch();
    glDisable(GL_DITHER);
    
    // Load Logo using config paths
    std::vector<std::string> logo_paths = config::get_logo_search_paths();

    unsigned char* data = nullptr;
    int channels;
    std::string loaded_logo_path;

    for (const auto& path : logo_paths) {
        data = stbi_load(path.c_str(), &logo_width_, &logo_height_, &channels, 4); // Force RGBA
        if (data) {
            loaded_logo_path = path;
            break;
        }
    }
    
    if (data) {
        glGenTextures(1, &logo_texture_id_);
        glBindTexture(GL_TEXTURE_2D, logo_texture_id_);
        
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        
        flush_ui_batch();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, logo_width_, logo_height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        // No glGenerateMipmap: min filter is GL_LINEAR, so the mip chain
        // could never be sampled — generating it only cost upload time.
        
        stbi_image_free(data);
        std::cout << "Loaded logo from: " << loaded_logo_path << " (" << logo_width_ << "x" << logo_height_ << ")" << std::endl;
    } else {
        std::cerr << "Failed to load logo from any location" << std::endl;
    }

    return true;
}

void Renderer::draw_quad(float x, float y, float w, float h, const ui::Color& color, float alpha_multiplier) {
    if (batch_enabled_) {
        float su = 0.0f, sv = 0.0f;
        const uint32_t tex = solid_texture(su, sv);
        batch_reserve(tex);
        batch_.quad(tex, x, y, x + w, y + h, su, sv, su, sv,
                    {color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
                     (color.a / 255.0f) * ui_alpha_ * alpha_multiplier});
        batch_done();
        return;
    }
    float vertices[] = {
        x, y,         0.0f, 0.0f,  // Top-left
        x + w, y,     1.0f, 0.0f,  // Top-right
        x, y + h,     0.0f, 1.0f,  // Bottom-left
        x + w, y + h, 1.0f, 1.0f   // Bottom-right
    };
    
    flush_ui_batch();
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    
    glUniform4f(u_color_loc_,
                color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, (color.a / 255.0f) * ui_alpha_ * alpha_multiplier);
    glUniform1i(u_use_texture_loc_, 0);
    
    glBindVertexArray(vao_);
    UI_DRAW_ARRAYS(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

void Renderer::draw_line(float x1, float y1, float x2, float y2, float width, const ui::Color& color, float alpha_multiplier) {
    // Draw line as a thin quad
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len == 0.0f) return;
    
    float perp_x = -dy / len * width / 2.0f;
    float perp_y = dx / len * width / 2.0f;
    
    float vertices[] = {
        x1 + perp_x, y1 + perp_y,  0.0f, 0.0f,
        x2 + perp_x, y2 + perp_y,  1.0f, 0.0f,
        x1 - perp_x, y1 - perp_y,  0.0f, 1.0f,
        x2 - perp_x, y2 - perp_y,  1.0f, 1.0f
    };

    if (batch_enabled_) {
        float su = 0.0f, sv = 0.0f;
        const uint32_t tex = solid_texture(su, sv);
        for (int i = 0; i < 4; ++i) {
            vertices[i * 4 + 2] = su;
            vertices[i * 4 + 3] = sv;
        }
        batch_reserve(tex);
        batch_.strip4(tex, vertices,
                      {color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
                       (color.a / 255.0f) * ui_alpha_ * alpha_multiplier});
        batch_done();
        return;
    }
    
    flush_ui_batch();
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    
    glUniform4f(u_color_loc_,
                color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, (color.a / 255.0f) * ui_alpha_ * alpha_multiplier);
    glUniform1i(u_use_texture_loc_, 0);
    
    glBindVertexArray(vao_);
    UI_DRAW_ARRAYS(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

void Renderer::draw_textured_quad(uint32_t tex_id, float x, float y,
                                  float w, float h, float alpha_multiplier) {
    // Same vertex layout as draw_quad, but switch the shader into
    // textured mode and bind the supplied texture. Mirrors the
    // render_title() logo-drawing idiom.
    if (batch_enabled_) {
        batch_reserve(tex_id);
        batch_.quad(tex_id, x, y, x + w, y + h, 0.0f, 0.0f, 1.0f, 1.0f,
                    {1.0f, 1.0f, 1.0f, ui_alpha_ * alpha_multiplier});
        batch_done();
        return;
    }

    float vertices[] = {
        x, y,         0.0f, 0.0f,
        x + w, y,     1.0f, 0.0f,
        x, y + h,     0.0f, 1.0f,
        x + w, y + h, 1.0f, 1.0f
    };

    flush_ui_batch();
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glUniform4f(u_color_loc_,
                1.0f, 1.0f, 1.0f, ui_alpha_ * alpha_multiplier);
    glUniform1i(u_use_texture_loc_, 1);

    glBindTexture(GL_TEXTURE_2D, tex_id);
    glBindVertexArray(vao_);
    UI_DRAW_ARRAYS(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    // Restore solid-color mode so the next draw_quad doesn't sample
    // whatever texture was bound.
    glUniform1i(u_use_texture_loc_, 0);
}

void Renderer::cleanup() {
    destroy_batch_gl();
    // Program ids are being deleted (and will be recycled by GL) —
    // cached uniform locations keyed by those ids must go with them.
    uniform_loc_cache_.clear();
    if (shader_program_ != 0) {
        glDeleteProgram(shader_program_);
        shader_program_ = 0;
    }
    if (crt_shader_program_ != 0) {
        glDeleteProgram(crt_shader_program_);
        crt_shader_program_ = 0;
    }
    if (crt_composite_shader_program_ != 0) {
        glDeleteProgram(crt_composite_shader_program_);
        crt_composite_shader_program_ = 0;
    }
    if (bloom_downsample_shader_program_ != 0) {
        glDeleteProgram(bloom_downsample_shader_program_);
        bloom_downsample_shader_program_ = 0;
    }
    destroy_scene_fbo();
    destroy_bloom_fbos();
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (vbo_ != 0) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (logo_texture_id_ != 0) {
        flush_ui_batch();
        glDeleteTextures(1, &logo_texture_id_);
        logo_texture_id_ = 0;
    }
    if (bezel_texture_id_ != 0) {
        flush_ui_batch();
        glDeleteTextures(1, &bezel_texture_id_);
        bezel_texture_id_ = 0;
    }
    if (marquee_frame_texture_id_ != 0) {
        flush_ui_batch();
        glDeleteTextures(1, &marquee_frame_texture_id_);
        marquee_frame_texture_id_ = 0;
    }
    if (thumbnail_texture_id_ != 0) {
        flush_ui_batch();
        glDeleteTextures(1, &thumbnail_texture_id_);
        thumbnail_texture_id_ = 0;
    }
    for (auto& pair : system_logo_cache_) {
        if (pair.second.texture_id != 0) {
            flush_ui_batch();
            glDeleteTextures(1, &pair.second.texture_id);
        }
    }
    system_logo_cache_.clear();
    if (qr_cache_tex_ != 0) {
        flush_ui_batch();
        glDeleteTextures(1, &qr_cache_tex_);
        qr_cache_tex_ = 0;
    }
    qr_cache_url_.clear();
    // The thumbnail decode worker publishes into members — join before
    // they die, and free any undelivered pixel buffer.
    if (thumb_worker_.joinable()) thumb_worker_.join();
    if (thumb_result_pixels_ != nullptr) {
        stbi_image_free(thumb_result_pixels_);
        thumb_result_pixels_ = nullptr;
    }
    if (title_font_manager_) {
        title_font_manager_->cleanup();
    }
    if (body_font_manager_) {
        body_font_manager_->cleanup();
    }
}

int Renderer::cached_uniform(uint32_t program, const char* name) {
    const auto key = std::make_pair(program, name);
    auto it = uniform_loc_cache_.find(key);
    if (it != uniform_loc_cache_.end()) return it->second;
    const int loc = glGetUniformLocation(program, name);
    uniform_loc_cache_.emplace(key, loc);
    return loc;
}

} // namespace ui
