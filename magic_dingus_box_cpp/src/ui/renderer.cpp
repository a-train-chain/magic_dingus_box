#define STB_IMAGE_IMPLEMENTATION
#include "../utils/stb_image.h"

#include "renderer.h"

#include "theme.h"
#include "font_manager.h"
#include "settings_menu.h"
#include "virtual_keyboard.h" // Added for virtual keyboard rendering
#include "qrcodegen.hpp" // QR code generation
#include "../app/app_state.h"
#include "../app/playlist_loader.h"
#include "../app/settings_persistence.h" // For getting config if needed
#include "../utils/config.h"

#include <GLES3/gl3.h>
#include <iostream>
#include <sstream>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <vector>
#include <algorithm> // Added for std::min/max

namespace ui {

// Simple vertex shader for 2D rendering
static const char* vertex_shader_source = R"(
#version 300 es
precision mediump float;
in vec2 position;
in vec2 texCoord;
out vec2 vTexCoord;
uniform vec2 screenSize;

void main() {
    vec2 normalizedPos = (position / screenSize) * 2.0 - 1.0;
    normalizedPos.y = -normalizedPos.y;  // Flip Y
    gl_Position = vec4(normalizedPos, 0.0, 1.0);
    vTexCoord = texCoord;
}
)";

// Simple fragment shader
static const char* fragment_shader_source = R"(
#version 300 es
precision highp float;
in vec2 vTexCoord;
out vec4 fragColor;
uniform vec4 color;
uniform sampler2D tex;
uniform bool useTexture;

void main() {
    if (useTexture) {
        vec4 texColor = texture(tex, vTexCoord);
        // Standard alpha blending: multiply texture RGB by color RGB, multiply alphas
        // This ensures text looks the same whether over solid background or video
        fragColor = texColor * color;
    } else {
        fragColor = color;
    }
}
)";

// CRT Fragment Shader
static const char* crt_fragment_shader_source = R"(
#version 300 es
precision mediump float;
in vec2 vTexCoord;
out vec4 fragColor;

uniform float time;
uniform float scanlineIntensity;
uniform float warmthIntensity;
uniform float glowIntensity;
uniform float rgbMaskIntensity;
uniform float bloomIntensity;
uniform float interlacingIntensity;
uniform float flickerIntensity;
uniform vec2 screenSize;

// Helper for RGB mask
float rgb_mask(float x) {
    float m = mod(x, 3.0);
    if (m < 1.0) return 1.0; // Red
    if (m < 2.0) return 1.0; // Green
    return 1.0;              // Blue
    // Simplified: just return 1.0 for now, real RGB mask needs per-channel masking
}

void main() {
    vec4 color = vec4(0.0, 0.0, 0.0, 0.0); // Start transparent
    
    // 1. Scanlines (Horizontal)
    // Use gl_FragCoord.y for pixel-perfect lines, or vTexCoord.y for resolution-independent
    if (scanlineIntensity > 0.0) {
        float scanline = sin(vTexCoord.y * screenSize.y * 3.14159); // Simple sine wave
        // Map [-1, 1] to [1-intensity, 1]
        // We want dark lines, so we subtract alpha
        float line = 0.5 + 0.5 * scanline;
        color = vec4(0.0, 0.0, 0.0, scanlineIntensity * (1.0 - line));
    }
    
    // 2. Interlacing (Vertical jitter or alternate lines)
    if (interlacingIntensity > 0.0) {
        // Simple odd/even line darkening based on time
        float odd = mod(gl_FragCoord.y, 2.0);
        float flicker = mod(time * 30.0, 2.0); // 30Hz flicker
        if (abs(odd - flicker) < 0.5) {
            color.a = max(color.a, interlacingIntensity * 0.5);
        }
    }
    
    // 3. RGB Mask (Vertical stripes)
    if (rgbMaskIntensity > 0.0) {
        float x = gl_FragCoord.x;
        float m = mod(x, 3.0);
        // We want to darken 2 of the 3 subpixels slightly to simulate the mask
        // This is a "black overlay" approach
        // If we are on Red pixel, we darken Green and Blue? 
        // No, we are drawing black lines between subpixels?
        // Let's just draw thin vertical lines
        if (m > 2.0) { // Every 3rd pixel
            color.a = max(color.a, rgbMaskIntensity);
        }
    }
    
    // 4. Phosphor Glow (Vignette)
    if (glowIntensity > 0.0) {
        vec2 uv = vTexCoord * 2.0 - 1.0; // [-1, 1]
        float dist = length(uv);
        // Darken corners
        float vignette = smoothstep(0.5, 1.5, dist);
        color.a = max(color.a, glowIntensity * vignette);
    }
    
    // 5. Warmth (Orange/Red tint)
    vec4 warmthColor = vec4(1.0, 0.9, 0.8, 0.0); // Warm tint
    if (warmthIntensity > 0.0) {
        // We can't easily "tint" the underlying video with a black overlay
        // We need to draw a colored overlay with alpha
        // But 'color' variable so far is black overlay.
        // We need to mix in the warmth.
        // This shader is getting complicated because we are mixing "darkening" (scanlines) and "tinting" (warmth).
        // Let's output the warmth as a separate component or just mix it into fragColor.
        // For now, let's just add a warm overlay.
        // We'll handle this by outputting a color that isn't just black.
    }
    
    // 6. Flicker (Global brightness modulation)
    if (flickerIntensity > 0.0) {
        float f = sin(time * 10.0) * sin(time * 23.0);
        // Random-ish flicker
        color.a = max(color.a, flickerIntensity * 0.1 * (f + 1.0));
    }

    // Final composition
    // We are drawing a "filter" layer. 
    // Most effects are "darkening" (scanlines, vignette, mask).
    // Warmth is "tinting".
    // Bloom is "brightening".
    
    // If we want to support all, we might need to change blend mode or do multiple passes.
    // For "Low Taxing", we stick to one pass.
    // Standard alpha blending: src * alpha + dst * (1-alpha)
    // If src is black (0,0,0), we darken.
    // If src is orange, we tint.
    
    vec3 finalRGB = vec3(0.0, 0.0, 0.0); // Default to black (darkening)
    float finalAlpha = color.a;
    
    if (warmthIntensity > 0.0) {
        // Add warmth: reddish color, low alpha
        finalRGB += vec3(1.0, 0.6, 0.2) * warmthIntensity; // Orange-ish
        finalAlpha = max(finalAlpha, warmthIntensity * 0.2);
    }
    
    if (bloomIntensity > 0.0) {
        // Bloom: simplified as a center glow?
        // Or just a general brightness boost?
        // We can't boost brightness with standard alpha blending over opaque background easily without additive blending.
        // But we are in standard alpha blending mode.
        // Let's skip bloom for now or make it a white overlay (foggy).
        // Let's make it a subtle white glow in the center
        vec2 uv = vTexCoord * 2.0 - 1.0;
        float dist = length(uv);
        float centerGlow = 1.0 - smoothstep(0.0, 1.0, dist);
        finalRGB += vec3(1.0, 1.0, 1.0) * bloomIntensity * centerGlow * 0.2;
        finalAlpha = max(finalAlpha, bloomIntensity * centerGlow * 0.1);
    }
    
    fragColor = vec4(finalRGB, finalAlpha);
}
)";

Renderer::Renderer(uint32_t width, uint32_t height)
    : width_(width)
    , height_(height)
    , original_width_(width)
    , original_height_(height)
    , ui_alpha_(1.0f)
    , shader_program_(0)
    , crt_shader_program_(0)
    , vao_(0)
    , vbo_(0)
    , logo_texture_id_(0)
    , logo_width_(0)
    , logo_height_(0)
{
    theme_ = std::make_unique<Theme>();
    title_font_manager_ = std::make_unique<FontManager>();
    body_font_manager_ = std::make_unique<FontManager>();
}

Renderer::~Renderer() {
    cleanup();
}

void Renderer::reset_gl() {
    // After an external app (like RetroArch) takes over the EGL context,
    // our GL resources are invalid. We need to delete and re-create them.
    std::cout << "UI Renderer: Resetting GL resources after external context takeover" << std::endl;
    
    // Delete old resources (they may be invalid but try anyway for cleanliness)
    if (shader_program_ != 0) {
        glDeleteProgram(shader_program_);
        shader_program_ = 0;
    }
    if (crt_shader_program_ != 0) {
        glDeleteProgram(crt_shader_program_);
        crt_shader_program_ = 0;
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
        glDeleteTextures(1, &logo_texture_id_);
        logo_texture_id_ = 0;
    }
    
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
    
    // Re-create VAO/VBO
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    glBindVertexArray(0);
    
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
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, logo_width_, logo_height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        stbi_image_free(data);
    }
    
    // CRITICAL: Re-enable blending - RetroArch may have disabled it
    // Without this, all UI elements become invisible!
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    std::cout << "UI Renderer: GL resources reset complete (blending enabled)" << std::endl;
}

bool Renderer::load_bezel(const std::string& path) {
    // Don't reload if already loaded
    if (path == current_bezel_path_ && bezel_texture_id_ != 0) {
        return true;
    }
    
    // Delete old texture if exists
    if (bezel_texture_id_ != 0) {
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
        std::cerr << "Failed to load bezel: " << path << std::endl;
        return false;
    }
    
    glGenTextures(1, &bezel_texture_id_);
    glBindTexture(GL_TEXTURE_2D, bezel_texture_id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bezel_width_, bezel_height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    
    stbi_image_free(data);
    return true;
}

void Renderer::render_bezel() {
    if (bezel_texture_id_ == 0) return;
    
    // Bind our shader program and set up projection
    glUseProgram(shader_program_);
    
    // Use ORIGINAL screen dimensions for bezel (fullscreen overlay)
    // Not width_/height_ which may be reduced for 4:3 content viewport
    float bezel_w = static_cast<float>(original_width_);
    float bezel_h = static_cast<float>(original_height_);
    
    // Set screenSize uniform for the shader (uses screen coords divider)
    glUniform2f(glGetUniformLocation(shader_program_, "screenSize"), bezel_w, bezel_h);
    
    // Render bezel as fullscreen textured quad
    float x = 0.0f;
    float y = 0.0f;
    
    float vertices[] = {
        x, y,             0.0f, 0.0f,
        x + bezel_w, y,   1.0f, 0.0f,
        x, y + bezel_h,   0.0f, 1.0f,
        x + bezel_w, y + bezel_h, 1.0f, 1.0f
    };
    
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    
    // Use white color with full alpha to render texture as-is
    glUniform4f(glGetUniformLocation(shader_program_, "color"), 1.0f, 1.0f, 1.0f, 1.0f);
    glUniform1i(glGetUniformLocation(shader_program_, "useTexture"), 1);
    
    // Ensure we are using Texture Unit 0 and tell the shader
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(shader_program_, "tex"), 0);
    
    // Enable blending for transparent areas of the bezel
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    glBindTexture(GL_TEXTURE_2D, bezel_texture_id_);
    
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    
    glBindTexture(GL_TEXTURE_2D, 0);
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
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    
    // Vertex attributes: position (2), texCoord (2)
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    glBindVertexArray(0);
    
    // Enable blending for transparency
    // Use standard alpha blending for consistent text rendering (same whether over video or not)
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Ensure smooth text rendering - disable dithering which can cause blockiness
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
        
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, logo_width_, logo_height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);
        
        stbi_image_free(data);
        std::cout << "Loaded logo from: " << loaded_logo_path << " (" << logo_width_ << "x" << logo_height_ << ")" << std::endl;
    } else {
        std::cerr << "Failed to load logo from any location" << std::endl;
    }

    return true;
}

void Renderer::render(const app::AppState& state) {
    // Debug logging removed for performance - only log errors
    
    // CRITICAL: Don't render UI at all when intro video is showing (even if not ready yet)
    // This prevents UI from briefly appearing before intro video starts
    // Also don't render if intro is loading but not ready yet
    // BUT render if we are fading out (transition to UI)
    if (state.showing_intro_video && !state.intro_fading_out) {
        return;  // Don't render UI during intro video playback (unless fading out)
    }
    
    // NOTE: glViewport is set by the caller (main.cpp) for proper 4:3 centering in Modern TV mode
    // Do NOT set glViewport here as it would override the centered position
    
    // Conditional clearing based on video state
    // NOTE: When video is active, mpv has already rendered to the framebuffer,
    // so we should NOT clear - we want to preserve the video frame
    // Also, if ui_visible_when_playing is true, a video was just started, so don't clear
    // Also, if we have a valid playlist index, we're transitioning between videos, so don't clear
    // IMPORTANT: After intro video completes, we want to clear the screen to show clean UI
    bool is_transitioning = (state.current_playlist_index >= 0 && state.current_item_index >= 0);
    bool should_clear = (!state.video_active && !state.ui_visible_when_playing && !is_transitioning) || 
                        (state.intro_complete && !state.video_active);  // Clear after intro completes
    if (should_clear) {
        // No video and no video loading: clear with background color (UI should always show when no video)
        // Also clear after intro video completes to remove any lingering video frames
        glClearColor(
            theme_->bg.r / 255.0f,
            theme_->bg.g / 255.0f,
            theme_->bg.b / 255.0f,
            1.0f
        );
        glClear(GL_COLOR_BUFFER_BIT);
    }
    // When video is active or loading, we don't clear - mpv already rendered (or will render) the video frame
    
    // Calculate UI overlay alpha based on fade state
    float ui_overlay_alpha = 1.0f;
    
    // If we're transitioning between videos, don't show UI at all
    if (is_transitioning && !state.video_active) {
        return;  // Don't render UI during video transitions
    }
    
    // Handle fade animation (works for both video active and intro fade-in cases)
    if (state.is_fading) {
        // Calculate fade progress
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.fade_start_time);
        if (elapsed < state.fade_duration) {
            float fade_progress = static_cast<float>(elapsed.count()) / static_cast<float>(state.fade_duration.count());
            fade_progress = std::min(1.0f, std::max(0.0f, fade_progress));  // Clamp to [0, 1]
            
            if (state.fade_target_ui_visible) {
                // Fading in: alpha from 0 to 1
                ui_overlay_alpha = fade_progress;
            } else {
                // Fading out: alpha from 1 to 0
                ui_overlay_alpha = 1.0f - fade_progress;
            }
        } else {
            // Fade complete
            ui_overlay_alpha = state.fade_target_ui_visible ? 1.0f : 0.0f;
        }
    } else if (state.video_active) {
        // Video active but not fading - use current visibility state
        ui_overlay_alpha = state.ui_visible_when_playing ? 1.0f : 0.0f;
        
        // If overlay alpha is 0, don't render UI (video shows through)
        // BUT we still want to render CRT effects over the video, so don't return early!
        // if (ui_overlay_alpha <= 0.0f) {
        //    return;  // Video will show through, no UI overlay
        // }
    }
    // Otherwise (no video active), render UI normally (ui_overlay_alpha = 1.0f from initial value)
    
    // CRITICAL: Reset OpenGL state after mpv renders to ensure consistent text rendering
    // mpv may change blending, texture state, etc. that affects UI rendering
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DITHER);
    glActiveTexture(GL_TEXTURE0);  // Ensure we're using texture unit 0
    
    glUseProgram(shader_program_);
    if (shader_program_ == 0) {
        std::cerr << "ERROR: Shader program is 0!" << std::endl;
        return;
    }
    
    // Set screen size uniform
    GLint screenSizeLoc = glGetUniformLocation(shader_program_, "screenSize");
    if (screenSizeLoc < 0) {
        std::cerr << "Warning: screenSize uniform not found" << std::endl;
    } else {
        static bool logged_screensize = false;
        if (!logged_screensize) {
            std::cout << "UI Renderer: render() screenSize=" << width_ << "x" << height_ << std::endl;
            logged_screensize = true;
        }
        glUniform2f(screenSizeLoc, static_cast<float>(width_), static_cast<float>(height_));
    }
    
    // Determine alpha multipliers based on video state and fade
    // When video is active and UI is visible: text fully opaque (1.0), backgrounds 50% transparent (0.5)
    // When video is not active: everything fully opaque (1.0)
    // Text alpha is controlled by fade animation (fades in/out with overlay)
    float text_alpha = ui_overlay_alpha;  // Text fades in/out with overlay
    
    // Handle intro video fade-out: draw black overlay that fades in over the video
    if (state.intro_fading_out && state.video_active) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.intro_fade_out_start_time);
        std::chrono::milliseconds fade_out_duration(300);
        
        float fade_out_progress = 1.0f;
        if (elapsed < fade_out_duration) {
            fade_out_progress = static_cast<float>(elapsed.count()) / static_cast<float>(fade_out_duration.count());
            fade_out_progress = std::min(1.0f, std::max(0.0f, fade_out_progress));  // Clamp to [0, 1]
        }
        
        // Draw black overlay that fades in (from transparent to opaque) over the video
        ui::Color black_overlay = {0, 0, 0, static_cast<uint8_t>(255 * fade_out_progress)};  // Fade from 0 to 100% opacity
        draw_quad(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), black_overlay, 1.0f);
    }
    
    // Only render UI components if they are visible
    if (ui_overlay_alpha > 0.0f) {
        // When UI overlay should be visible, draw dark overlay behind text
        // Draw overlay first so it's behind all text elements
        if (state.video_active && !state.intro_fading_out) {
            // Draw a semi-transparent black overlay over the entire screen
            // This darkens the video background while still allowing it to show through
            // The overlay is drawn first so text renders on top of it
            // Alpha is controlled by fade animation
            ui::Color dark_overlay = {0, 0, 0, static_cast<uint8_t>(128 * ui_overlay_alpha)};  // 50% opacity black, scaled by fade
            draw_quad(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), dark_overlay, 1.0f);
        }
        float background_alpha = (state.video_active && state.ui_visible_when_playing) ? 0.5f : 1.0f;
        
        // Render UI components
        render_title(text_alpha, state.video_active, state.ui_visible_when_playing);
        render_playlist_list(state.playlists, state.selected_index, state.video_active, state.ui_visible_when_playing);
        render_footer(state, text_alpha, state.video_active, state.ui_visible_when_playing);
        
        // Render loading overlay if needed
        if (state.is_loading_game) {
            render_loading_overlay(state);
        }
        
        // background_alpha is calculated but currently not used in individual render functions
        // It's available for future use if needed for background transparency
        (void)background_alpha;  // Suppress unused variable warning
    }
    
    // Render volume overlay (always on top if active)
    render_volume_overlay(state);
            
    // Render settings menu if active (on top of everything, before scanlines)
    if (state.settings_menu && state.settings_menu->is_active()) {
        render_settings_menu(state.settings_menu, state.game_playlists, state.video_active, state.ui_visible_when_playing);
        
        // Render QR code when Info submenu is active
        if (state.settings_menu->get_current_submenu() == ui::MenuSection::INFO && 
            !state.content_manager_url.empty()) {
            // Position QR code centered in the menu panel, below the menu items
            uint32_t menu_width = width_ / 2;
            float menu_x = static_cast<float>(width_) - menu_width;
            float qr_size = 140.0f;  // Larger QR code for easier scanning
            
            // Calculate Y position: menu items take ~280px (header ~70px + 4 items * ~50px + spacing)
            // Place QR code in the middle of remaining space
            float menu_items_end = 320.0f;  // Approximate end of menu items
            float footer_start = static_cast<float>(height_) - 70.0f;  // Footer hint area (increased margin)
            float available_space = footer_start - menu_items_end;
            
            // Center QR code + hint text in available space
            float qr_with_hint_height = qr_size + 35.0f;  // QR + padding + hint text
            float qr_y = menu_items_end + (available_space - qr_with_hint_height) / 2.0f;
            float qr_x = menu_x + (static_cast<float>(menu_width) - qr_size) / 2.0f;
            
            render_qr_code(state.content_manager_url, qr_x, qr_y, qr_size, 1.0f);
            
            // Draw helper text below QR code
            std::string qr_hint = "Scan with phone camera";
            int hint_width = body_font_manager_->get_text_width(qr_hint, theme_->font_small_size);
            float hint_x = menu_x + (static_cast<float>(menu_width) - hint_width) / 2.0f;
            float hint_y = qr_y + qr_size + 15.0f + body_font_manager_->get_baseline_at_size(theme_->font_small_size);
            draw_text(qr_hint, hint_x, hint_y, theme_->font_small_size, theme_->fg, false, 1.0f);
        }
    }
    
    // Virtual Keyboard overlay
    if (state.keyboard && state.keyboard->is_active()) {
        render_virtual_keyboard(*state.keyboard);
    }
        
    // Apply CRT effects (scanlines, warmth, glow, etc.)
    // These are rendered as an overlay on top of everything
    // Pass ui_overlay_alpha > 0.0f to enable/disable scanlines specifically
    render_crt_effects(state, ui_overlay_alpha > 0.0f);
    
    // Check for errors after rendering
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::cerr << "OpenGL error after render: " << err << std::endl;
    }
}

void Renderer::draw_quad(float x, float y, float w, float h, const ui::Color& color, float alpha_multiplier) {
    float vertices[] = {
        x, y,         0.0f, 0.0f,  // Top-left
        x + w, y,     1.0f, 0.0f,  // Top-right
        x, y + h,     0.0f, 1.0f,  // Bottom-left
        x + w, y + h, 1.0f, 1.0f   // Bottom-right
    };
    
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    
    glUniform4f(glGetUniformLocation(shader_program_, "color"),
                color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, (color.a / 255.0f) * ui_alpha_ * alpha_multiplier);
    glUniform1i(glGetUniformLocation(shader_program_, "useTexture"), 0);
    
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

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
    
    for (char c : text) {
        if (c == '\n') {
            // Calculate line height for this font size
            int line_height = static_cast<int>(font_size * 1.2f);  // Approximate line height
            baseline_y += line_height;
            current_x = x;
            continue;
        }
        
        // Skip spaces - they don't need to be rendered, just advance
        if (c == ' ') {
            // Get the space glyph just for its advance width
            ui::Glyph space_glyph = font_manager->get_glyph_at_size(static_cast<char32_t>(' '), font_size);
            current_x += space_glyph.advance;
            continue;
        }
        
        ui::Glyph glyph = font_manager->get_glyph_at_size(static_cast<char32_t>(c), font_size);
        if (glyph.texture_id == 0) {
            // Glyph not available, just advance
            current_x += glyph.advance;
            continue;
        }
        
        // Draw glyph as a textured quad
        // bearing_y is the distance from baseline to top of bitmap (positive = above baseline)
        // In screen coords (Y down): top_of_bitmap = baseline_y - bearing_y
        float glyph_x = current_x + glyph.bearing_x;
        float glyph_y = baseline_y - glyph.bearing_y;  // Top of glyph bitmap
        
        glBindTexture(GL_TEXTURE_2D, glyph.texture_id);
        
        // Ensure smooth texture filtering for crisp text (not blocky)
        // Re-apply texture parameters to ensure they're set correctly
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        
        // Ensure texture is not using nearest neighbor filtering
        GLint min_filter, mag_filter;
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &min_filter);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mag_filter);
        if (min_filter != GL_LINEAR || mag_filter != GL_LINEAR) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        
        // Set color uniform
        // IMPORTANT: For text, we want colors to stay vibrant, so we DON'T multiply RGB by ui_alpha_
        // ui_alpha_ is only for background transparency, not text dimming
        // alpha_multiplier controls fade in/out animation, which we do want
        GLint colorLoc = glGetUniformLocation(shader_program_, "color");
        if (colorLoc >= 0) {
            glUniform4f(colorLoc, color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, (color.a / 255.0f) * alpha_multiplier);
        }
        
        GLint useTextureLoc = glGetUniformLocation(shader_program_, "useTexture");
        if (useTextureLoc >= 0) {
            glUniform1i(useTextureLoc, 1);
        }
        
        // Draw quad for glyph
        // The shader flips Y coordinate (normalizedPos.y = -normalizedPos.y)
        // So in screen space (Y increases downward), we position glyphs normally
        // glyph_y is the top of the glyph bitmap
        // Texture coordinates: (0,0) is top-left of texture, (1,1) is bottom-right
        float vertices[] = {
            glyph_x, glyph_y,                     0.0f, 0.0f,  // Top-left (screen and texture)
            glyph_x + glyph.width, glyph_y,       1.0f, 0.0f,  // Top-right
            glyph_x, glyph_y + glyph.height,      0.0f, 1.0f,  // Bottom-left
            glyph_x + glyph.width, glyph_y + glyph.height, 1.0f, 1.0f  // Bottom-right
        };
        
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
        
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        
        glBindTexture(GL_TEXTURE_2D, 0);
        
        current_x += glyph.advance;
    }
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
    
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    
    glUniform4f(glGetUniformLocation(shader_program_, "color"),
                color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, (color.a / 255.0f) * ui_alpha_ * alpha_multiplier);
    glUniform1i(glGetUniformLocation(shader_program_, "useTexture"), 0);
    
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

void Renderer::render_virtual_keyboard(const VirtualKeyboard& keyboard) {
    // Darken background
    draw_quad(0, 0, width_, height_, theme_->bg, 0.8f);
    
    float kb_width = width_ * 0.8f;
    float kb_height = height_ * 0.5f;
    float start_x = (width_ - kb_width) / 2.0f;
    float start_y = (height_ - kb_height) / 2.0f + 50.0f;
    
    // Background panel
    draw_quad(start_x - 20, start_y - 80, kb_width + 40, kb_height + 100, theme_->bg, 1.0f);
    draw_line(start_x - 20, start_y - 80, start_x + kb_width + 20, start_y - 80, 2.0f, theme_->accent2);
    draw_line(start_x - 20, start_y + kb_height + 20, start_x + kb_width + 20, start_y + kb_height + 20, 2.0f, theme_->accent2);
    
    // Title
    draw_text(keyboard.get_title(), start_x, start_y - 50, 24, theme_->fg, true);
    
    // Text buffer (Input box)
    float input_box_height = 40.0f;
    draw_quad(start_x, start_y - 20, kb_width, input_box_height, theme_->bg, 1.0f);
    draw_line(start_x, start_y - 20 + input_box_height, start_x + kb_width, start_y - 20 + input_box_height, 2.0f, theme_->accent);
    
    // Text cursor blinking
    std::string display_text = keyboard.get_text();
    // Simple cursor visualization
    if (static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 500) % 2 == 0) {
        display_text += "_";
    }
    draw_text(display_text, start_x + 10, start_y - 12, 20, theme_->fg);
    
    // Keys
    const auto& layout = keyboard.get_layout();
    float key_margin = 5.0f;
    float keys_area_height = kb_height - 20; // approximate
    float row_height = keys_area_height / layout.size();
    
    for (int r = 0; r < (int)layout.size(); ++r) {
        const auto& row = layout[r];
        float row_width = kb_width;
        float key_width = row_width / row.size();
        
        for (int c = 0; c < (int)row.size(); ++c) {
            float kx = start_x + c * key_width;
            float ky = start_y + 40 + r * row_height;
            float kw = key_width - key_margin;
            float kh = row_height - key_margin;
            
            bool selected = (r == keyboard.get_selected_row() && c == keyboard.get_selected_col());
            
            ui::Color bg_color = selected ? theme_->accent : theme_->action;
            ui::Color text_color = selected ? theme_->bg : theme_->fg;
            
            draw_quad(kx, ky, kw, kh, bg_color);
            
            // Center text
            std::string label = row[c];
            // Adjust special labels if needed
            int font_size = 20;
            if (label.length() > 1) font_size = 16;
            
            // Simple centering (approximate)
            float text_width = label.length() * (font_size * 0.6f); 
            float tx = kx + (kw - text_width) / 2.0f + (font_size * 0.2f); // minor adjustment
            // Center text vertically
            // draw_text uses y as baseline. To center vertically, baseline should be lower.
            // approx baseline = top + (height + font_size/2) / 2
            float ty = ky + (kh + font_size * 0.6f) / 2.0f;
            
            draw_text(label, tx, ty, font_size, text_color);
        }
    }
}


// ... (existing includes)



// ...

void Renderer::render_title(float text_alpha, bool /* video_active */, bool /* ui_visible_when_playing */) {
    // Render Logo instead of text
    if (logo_texture_id_ != 0) {
        // Calculate centered position
        // User requested "low enough... not being chopped off"
        float logo_y = 20.0f; // Top margin
        float logo_x = (width_ - logo_width_) / 2.0f;
        
        // Draw logo quad
        // We need to bind the texture and draw a quad
        // We can reuse draw_quad but it takes a color, we need a textured quad method or modify draw_quad
        // Actually draw_text does textured quads.
        // Let's manually draw it here to be safe and simple
        
        float x = logo_x;
        float y = logo_y;
        float w = static_cast<float>(logo_width_);
        float h = static_cast<float>(logo_height_);
        
        float vertices[] = {
            x, y,         0.0f, 0.0f,
            x + w, y,     1.0f, 0.0f,
            x, y + h,     0.0f, 1.0f,
            x + w, y + h, 1.0f, 1.0f
        };
        
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
        
        // Use white color to render texture as-is (multiplied by alpha)
        glUniform4f(glGetUniformLocation(shader_program_, "color"),
                    1.0f, 1.0f, 1.0f, ui_alpha_ * text_alpha);
        glUniform1i(glGetUniformLocation(shader_program_, "useTexture"), 1); // Enable texture
        
        glBindTexture(GL_TEXTURE_2D, logo_texture_id_);
        
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        
        glBindTexture(GL_TEXTURE_2D, 0);
        
    } else {
        // Fallback to text if logo failed to load
        std::string product_title = "Magic Dingus Box";
        int title_width = title_font_manager_->get_text_width(product_title);
        float title_x = (static_cast<float>(width_) - title_width) / 2.0f;
        int title_baseline_offset = title_font_manager_->get_baseline_at_size(theme_->font_title_size);
        float title_baseline = 8.0f + title_baseline_offset;
        
        draw_text(product_title, title_x, title_baseline, theme_->font_title_size, theme_->accent, true, text_alpha);
        
        float underline_y = title_baseline + 10.0f;
        draw_line(title_x, underline_y, title_x + title_width, underline_y, 2.0f, theme_->accent2, text_alpha);
    }
    
    // PLAYLISTS section header (use title font)
    // Calculate proper spacing based on line heights
    // If logo is used, base it on logo height
    float header_baseline;
    if (logo_texture_id_ != 0) {
        header_baseline = 20.0f + logo_height_ + 40.0f; // Logo Y + Height + Spacing
    } else {
        int title_baseline_offset = title_font_manager_->get_baseline_at_size(theme_->font_title_size);
        float title_baseline = 8.0f + title_baseline_offset;
        int title_line_height = static_cast<int>(theme_->font_title_size * 1.2f);
        header_baseline = title_baseline + title_line_height + 24.0f;
    }

    std::string header = "Playlists";
    // Use the correct font size for width calculation to match rendering
    int header_width = title_font_manager_->get_text_width(header, theme_->font_heading_size);
    
    // Align the header with the playlist titles (offset by 36.0f to skip numbers)
    float header_x = static_cast<float>(theme_->margin_x) + 36.0f;
    
    draw_text(header, header_x, header_baseline, theme_->font_heading_size, theme_->accent2, true, text_alpha);
    
    // Underline for playlists header - matching text width
    float header_line_y = header_baseline + 10.0f;
    draw_line(header_x, header_line_y,
              header_x + header_width, header_line_y, 2.0f, theme_->accent2, text_alpha);
}

void Renderer::render_playlist_list(const std::vector<app::Playlist>& playlists, int selected_index, bool video_active, bool ui_visible_when_playing) {
    // Debug: Log playlist rendering
    static int playlist_render_count = 0;
    if (playlist_render_count < 2) {
        std::cout << "    Rendering playlist list: " << playlists.size() << " playlists, selected=" << selected_index << std::endl;
        playlist_render_count++;
    }
    
    // Start after title and playlists header
    // Calculate proper positions using font-size-specific baselines
    int title_baseline_offset = title_font_manager_->get_baseline_at_size(theme_->font_title_size);
    float title_baseline = 8.0f + title_baseline_offset;
    int title_line_height = static_cast<int>(theme_->font_title_size * 1.2f);
    
    float header_baseline = title_baseline + title_line_height + 24.0f;
    int header_line_height = static_cast<int>(theme_->font_heading_size * 1.2f);
    
    // Increased spacing below header line (was 20.0f, now 40.0f)
    float y = header_baseline + header_line_height + 4.0f + 40.0f;
    
    // Determine alpha multipliers based on video state
    // When video is active and UI is visible: text fully opaque (1.0), backgrounds 50% transparent (0.5)
    // When video is not active: everything fully opaque (1.0)
    float text_alpha = (video_active && ui_visible_when_playing) ? 1.0f : 1.0f;
    // background_alpha is no longer used in this function (highlight bar uses fixed alpha)
    // Removed to eliminate unused variable warning
    
    // Get current time for blinking indicator (time-based, matching Python: 500ms)
    static auto last_blink_time = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_blink_time).count();
    bool indicator_visible = (elapsed_ms / 500) % 2 == 0;  // Blink every 500ms (matching Python)
    
    for (size_t i = 0; i < playlists.size() && i < 12; i++) {
        const auto& pl = playlists[i];
        bool selected = (static_cast<int>(i) == selected_index);
        
        // Selection highlight bar (subtle background - very transparent so it doesn't block text)
        if (selected) {
            float highlight_x = static_cast<float>(theme_->margin_x) + 32.0f;
            float highlight_y = y - 2.0f;
            float highlight_w = static_cast<float>(width_) - highlight_x - static_cast<float>(theme_->margin_x);
            float highlight_h = static_cast<float>(theme_->playlist_item_height) + 4.0f;
            
            ui::Color highlight_color = theme_->accent2;
            highlight_color.a = 20;  // Very subtle alpha (reduced from 40) so text is clearly visible
            // Don't apply background_alpha multiplier to highlight - keep it consistently subtle
            draw_quad(highlight_x, highlight_y, highlight_w, highlight_h, highlight_color, 1.0f);
        }
        
        // Channel number (01., 02., etc.)
        char channel_buf[16];
        snprintf(channel_buf, sizeof(channel_buf), "%02zu.", i + 1);
        std::string channel_num = channel_buf;
        
        // Playlist title only (curator removed per user request)
        std::string text = pl.title;
        
        int font_size = selected ? theme_->font_large_size : theme_->font_medium_size;
        ui::Color text_color = selected ? theme_->accent2 : theme_->fg;
        
        // Use a common baseline for all text on this line (based on the larger font)
        int item_baseline_offset = body_font_manager_->get_baseline_at_size(font_size);
        float item_baseline = y + item_baseline_offset;
        
        // Draw channel number (will align to baseline using its own bearing_y) - use body font
        draw_text(channel_num, static_cast<float>(theme_->margin_x), item_baseline, theme_->font_small_size, theme_->dim, false, text_alpha);
        
        // Draw playlist text on the same baseline - use body font
        float text_x = static_cast<float>(theme_->margin_x) + 36.0f;
        draw_text(text, text_x, item_baseline, font_size, text_color, false, text_alpha);
        
        // Blinking selection indicator (triangle pointing LEFT toward text, at end of text)
        if (selected && indicator_visible) {
            // Position triangle at the end of the text with spacing
            // IMPORTANT: Use the same font size as the text being rendered
            float text_width = body_font_manager_->get_text_width(text, font_size);
            float indicator_x = text_x + text_width + 16.0f;
            // Center triangle on the visual center of the text line (middle of line height)
            // Line height is approximately font_size * 1.2, so center is at y + (line_height / 2)
            float line_height = static_cast<float>(font_size) * 1.2f;
            float indicator_y = y + (line_height / 2.0f);  // Visual center of line, not baseline
            float size = 6.0f;
            
            // Draw filled triangle pointing LEFT (toward text) - matching Python version
            // Python: points = [(indicator_x, cy - size), (indicator_x, cy + size), (indicator_x - int(size * 1.2), cy)]
            // So: top point, bottom point, left point (pointing left)
            float top_y = indicator_y - size;
            float bottom_y = indicator_y + size;
            float left_x = indicator_x - size * 1.2f;  // Point to the left
            
            // Draw triangle as 3 vertices
            float triangle_vertices[] = {
                indicator_x, top_y,       0.0f, 0.0f,  // Top point (right side)
                indicator_x, bottom_y,    1.0f, 0.0f,  // Bottom point (right side)
                left_x, indicator_y,      1.0f, 1.0f   // Left point (pointing left)
            };
            
            glBindBuffer(GL_ARRAY_BUFFER, vbo_);
            glBufferData(GL_ARRAY_BUFFER, sizeof(triangle_vertices), triangle_vertices, GL_DYNAMIC_DRAW);
            
            GLint colorLoc = glGetUniformLocation(shader_program_, "color");
            if (colorLoc >= 0) {
                glUniform4f(colorLoc, theme_->accent2.r / 255.0f, theme_->accent2.g / 255.0f, 
                           theme_->accent2.b / 255.0f, (theme_->accent2.a / 255.0f) * ui_alpha_ * text_alpha);
            }
            GLint useTextureLoc = glGetUniformLocation(shader_program_, "useTexture");
            if (useTextureLoc >= 0) {
                glUniform1i(useTextureLoc, 0);  // No texture, solid color
            }
            
            glBindVertexArray(vao_);
            glDrawArrays(GL_TRIANGLES, 0, 3);  // Draw as triangle
            glBindVertexArray(0);
        }
        
        y += static_cast<float>(theme_->playlist_item_height);
    }
}

std::string Renderer::format_time(double seconds) {
    if (seconds < 0) {
        std::ostringstream oss;
        int minutes = static_cast<int>(seconds) / 60;
        int secs = static_cast<int>(seconds) % 60;
        oss << minutes << ":" << std::setfill('0') << std::setw(2) << std::abs(secs); // Use abs for seconds
        return oss.str();
    }
    int total = static_cast<int>(seconds);
    int m = total / 60;
    int s = total % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    return std::string(buf);
}

void Renderer::render_loading_overlay(const app::AppState& state) {
    if (!state.is_loading_game) return;
    
    // Draw semi-transparent background
    draw_quad(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 
              ui::Color(0, 0, 0, 200));
              
    // Calculate pulsing alpha for text
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    float pulse = (sin(duration * 0.005f) + 1.0f) * 0.5f; // 0.0 to 1.0
    float text_alpha = 0.5f + (pulse * 0.5f); // 0.5 to 1.0
    
    // Draw "Loading..." text
    std::string text = "Loading...";
    int font_size = theme_->font_large_size;
    
    // Get text metrics for proper centering
    float text_width = body_font_manager_->get_text_width(text, font_size);
    float baseline_offset = body_font_manager_->get_baseline_at_size(font_size);
    
    // Center text horizontally
    float x = (width_ - text_width) / 2.0f;
    
    // Center text vertically (accounting for baseline)
    // Visual center of text should be at screen center
    float y = (height_ / 2.0f) - (font_size / 2.0f) + baseline_offset;
    
    // Use body font (false) to match rest of UI
    draw_text(text, x, y, font_size, theme_->accent, false, text_alpha);
    
    // Draw animated spinner (rotating square)
    float spinner_size = 40.0f;
    float spinner_x = width_ / 2.0f;
    // Position spinner below centered text
    float spinner_y = y + (font_size / 2.0f) + 30.0f; // 30px below text center
    
    float rotation = duration * 0.005f; // Radians
    
    // Calculate rotated vertices
    float half_size = spinner_size / 2.0f;
    float cos_r = cos(rotation);
    float sin_r = sin(rotation);
    
    // 4 corners relative to center
    struct Point { float x, y; };
    Point corners[4] = {
        { -half_size, -half_size },
        { half_size, -half_size },
        { half_size, half_size },
        { -half_size, half_size }
    };
    
    // Rotate and translate
    // We'll use 2 triangles: 0-1-2 and 0-2-3
    
    Point rotated[4];
    for (int i = 0; i < 4; ++i) {
        rotated[i].x = spinner_x + (corners[i].x * cos_r - corners[i].y * sin_r);
        rotated[i].y = spinner_y + (corners[i].x * sin_r + corners[i].y * cos_r);
    }
    
    float triangle_vertices[] = {
        rotated[0].x, rotated[0].y, 0.0f, 0.0f,
        rotated[1].x, rotated[1].y, 1.0f, 0.0f,
        rotated[2].x, rotated[2].y, 1.0f, 1.0f,
        
        rotated[0].x, rotated[0].y, 0.0f, 0.0f,
        rotated[2].x, rotated[2].y, 1.0f, 1.0f,
        rotated[3].x, rotated[3].y, 0.0f, 1.0f
    };
    
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(triangle_vertices), triangle_vertices, GL_DYNAMIC_DRAW);

    GLint colorLoc = glGetUniformLocation(shader_program_, "color");
    if (colorLoc >= 0) {
        glUniform4f(colorLoc, theme_->accent.r / 255.0f, theme_->accent.g / 255.0f,
                   theme_->accent.b / 255.0f, 1.0f);
    }
    GLint useTextureLoc = glGetUniformLocation(shader_program_, "useTexture");
    if (useTextureLoc >= 0) {
        glUniform1i(useTextureLoc, 0);
    }

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

void Renderer::render_footer(const app::AppState& state, float text_alpha, bool video_active, bool ui_visible_when_playing) {
    // Footer baseline position (bottom-right, MTV-style)
    // Position baseline so text appears at height - 60
    float footer_baseline = static_cast<float>(height_ - 60) + body_font_manager_->get_baseline_at_size(theme_->font_small_size);
    
    // When video is active and UI overlay is visible, show title, artist, and duration
    if (video_active && ui_visible_when_playing && state.current_playlist_index >= 0 && state.current_item_index >= 0) {
        // Get current playing item
        if (state.current_playlist_index < static_cast<int>(state.playlists.size())) {
            const auto& pl = state.playlists[state.current_playlist_index];
            if (state.current_item_index < static_cast<int>(pl.items.size())) {
                const auto& item = pl.items[state.current_item_index];
                
                // Line 1: Title
                std::string title = item.title.empty() ? "Untitled" : item.title;
                int title_width = body_font_manager_->get_text_width(title, theme_->font_small_size);
                float title_x = static_cast<float>(width_) - title_width - 80.0f;
                draw_text(title, title_x, footer_baseline, theme_->font_small_size, theme_->fg, false, text_alpha);
                
                // Line 2: Artist (if available)
                if (!item.artist.empty()) {
                    float artist_baseline = footer_baseline + 18.0f;
                    int artist_width = body_font_manager_->get_text_width(item.artist, theme_->font_small_size);
                    float artist_x = static_cast<float>(width_) - artist_width - 80.0f;
                    draw_text(item.artist, artist_x, artist_baseline, theme_->font_small_size, theme_->fg, false, text_alpha);
                }
                
                // Line 3: Video progress (elapsed / duration)
                if (state.duration > 0) {
                    std::string progress_text = format_time(state.position) + " / " + format_time(state.duration);
                    float progress_baseline = footer_baseline + (item.artist.empty() ? 18.0f : 36.0f);
                    int progress_width = body_font_manager_->get_text_width(progress_text, theme_->font_small_size);
                    float progress_x = static_cast<float>(width_) - progress_width - 80.0f;
                    draw_text(progress_text, progress_x, progress_baseline, theme_->font_small_size, theme_->fg, false, text_alpha);
                }
                
                return;  // Don't show status text when showing video info
            }
        }
    }
    
    // Default footer: Status text and time (when not showing video info)
    std::string status = state.status_text;
    if (!status.empty()) {
        int status_width = body_font_manager_->get_text_width(status);
        float status_x = static_cast<float>(width_) - status_width - 80.0f;
        draw_text(status, status_x, footer_baseline, theme_->font_small_size, theme_->fg, false, text_alpha);
    }
    
    // Time display below status (elapsed / duration)
    if (state.position > 0 || state.duration > 0) {
        std::string time_text = format_time(state.position);
        if (state.duration > 0) {
            time_text += " / " + format_time(state.duration);
        }
        int time_width = body_font_manager_->get_text_width(time_text);
        float time_x = static_cast<float>(width_) - time_width - 80.0f;
        float time_baseline = footer_baseline + 18.0f;
        draw_text(time_text, time_x, time_baseline, theme_->font_small_size, theme_->fg, false, text_alpha);
    }
}



void Renderer::render_settings_menu(ui::SettingsMenuManager* menu, const std::vector<app::Playlist>& game_playlists, bool video_active, bool ui_visible_when_playing) {
    (void)ui_visible_when_playing; // Suppress unused parameter warning
    (void)video_active; // Suppress unused parameter warning
    if (!menu || (!menu->is_active() && !menu->is_closing())) {
        return;
    }
    
    // Determine alpha multipliers based on video state
    // Settings menu text is always fully opaque
    // Settings menu background is always fully opaque (not transparent) so video doesn't show through
    float text_alpha = 1.0f;
    float background_alpha = 1.0f;  // Always fully opaque for settings menu
    
    float progress = menu->get_animation_progress();
    uint32_t menu_width = width_ / 2;  // Half screen width
    float slide_offset = menu_width * (1.0f - progress);
    float menu_x = static_cast<float>(width_) - menu_width + slide_offset;
    
    // Get section color
    // Top-level "Settings" menu uses green, sub-sections use their original colors
    ui::MenuSection current_submenu = menu->get_current_submenu();
    ui::Color section_color;
    if (current_submenu == ui::MenuSection::BACK) {
        // Top-level Settings menu: green
        section_color = theme_->highlight1;
    } else if (current_submenu == ui::MenuSection::VIDEO_GAMES) {
        section_color = theme_->highlight1;  // Green
    } else if (current_submenu == ui::MenuSection::DISPLAY) {
        section_color = theme_->highlight3;  // Gold
    } else if (current_submenu == ui::MenuSection::AUDIO) {
        section_color = theme_->action;  // Blue
    } else if (current_submenu == ui::MenuSection::SYSTEM) {
        section_color = theme_->highlight2;  // Red
    } else {
        section_color = theme_->highlight1;  // Default to green
    }
    
    // Draw menu background panel
    draw_quad(menu_x, 0.0f, static_cast<float>(menu_width), static_cast<float>(height_), theme_->bg, background_alpha);
    
    // Draw left border accent
    draw_quad(menu_x, 0.0f, 4.0f, static_cast<float>(height_), section_color, background_alpha);
    
    // Check if we're in game browser mode
    if (menu->is_game_browser_active()) {
        render_game_browser(menu, game_playlists, menu_x, menu_width, section_color, text_alpha, background_alpha);
        return;
    }
    
    // Header
    std::string header_text = "Settings";
    if (current_submenu != ui::MenuSection::BACK) {
        if (current_submenu == ui::MenuSection::VIDEO_GAMES) {
            header_text = "Video games";
        } else if (current_submenu == ui::MenuSection::DISPLAY) {
            header_text = "Display";
        } else if (current_submenu == ui::MenuSection::AUDIO) {
            header_text = "Audio";
        } else if (current_submenu == ui::MenuSection::SYSTEM) {
            header_text = "System info";
        } else if (current_submenu == ui::MenuSection::WIFI) {
            header_text = "Wi-Fi";
        } else if (current_submenu == ui::MenuSection::WIFI_NETWORKS) {
            header_text = "Wi-Fi Networks";
        } else if (current_submenu == ui::MenuSection::INFO) {
            header_text = "Content Manager";
        }
    }
    
    int header_width = title_font_manager_->get_text_width(header_text, theme_->font_heading_size);
    float header_x = menu_x + (static_cast<float>(menu_width) - header_width) / 2.0f;
    int header_baseline_offset = title_font_manager_->get_baseline_at_size(theme_->font_heading_size);
    float header_baseline = 8.0f + header_baseline_offset;
    draw_text(header_text, header_x, header_baseline, theme_->font_heading_size, section_color, true, text_alpha);
    
    // Underline - position below the text with enough space for descenders (like 'g', 'y', 'p')
    // Match the spacing used in render_title and render_playlist_list (10.0f instead of 4.0f)
    float underline_y = header_baseline + 10.0f;
    // Underline only spans the word - use exact text width
    draw_line(header_x, underline_y, header_x + header_width, underline_y, 2.0f, section_color, text_alpha);
    
    // Menu items
    const std::vector<ui::MenuItem>& items = current_submenu == ui::MenuSection::BACK ? 
        menu->get_menu_items() : menu->get_submenu_items();
    
    // Safety check: ensure items vector is not empty
    if (items.empty()) {
        return;  // Don't render if no items
    }
    
    float start_y = underline_y + 30.0f;
    int item_height = 60;
    int max_visible = 7;
    int scroll_offset = menu->get_scroll_offset();
    int selected_index = menu->get_selected_index();
    
    // Clamp scroll_offset and selected_index to valid range
    if (scroll_offset < 0) scroll_offset = 0;
    if (scroll_offset >= static_cast<int>(items.size())) scroll_offset = std::max(0, static_cast<int>(items.size()) - 1);
    if (selected_index < 0) selected_index = 0;
    if (selected_index >= static_cast<int>(items.size())) selected_index = static_cast<int>(items.size()) - 1;
    
    // Render visible items
    for (int i = 0; i < max_visible && (scroll_offset + i) < static_cast<int>(items.size()); i++) {
        int idx = scroll_offset + i;
        if (idx < 0 || idx >= static_cast<int>(items.size())) {
            continue;  // Safety check
        }
        const ui::MenuItem& item = items[idx];
        bool is_selected = (idx == selected_index);
        float y = start_y + i * item_height;
        
        // Selection highlight
        if (is_selected) {
            ui::Color highlight = section_color;
            highlight.a = 40;
            draw_quad(menu_x + 10.0f, y - 5.0f, static_cast<float>(menu_width) - 20.0f, 
                     static_cast<float>(item_height), highlight, background_alpha);
            
            // Selection indicator (triangle pointing RIGHT, at beginning of text - matching Python)
            // Python: points = [(indicator_x, cy - size), (indicator_x, cy + size), (indicator_x + int(size * 1.2), cy)]
            float indicator_x = menu_x + 15.0f;
            float indicator_y = y + item_height / 2.0f - 5.0f;
            float size = 8.0f;
            float right_x = indicator_x + size * 1.2f;  // Point to the right
            float triangle_vertices[] = {
                indicator_x, indicator_y - size,   0.0f, 0.0f,  // Top point (left side)
                indicator_x, indicator_y + size,   1.0f, 0.0f,  // Bottom point (left side)
                right_x, indicator_y,               1.0f, 1.0f   // Right point (pointing right)
            };
            
            glBindBuffer(GL_ARRAY_BUFFER, vbo_);
            glBufferData(GL_ARRAY_BUFFER, sizeof(triangle_vertices), triangle_vertices, GL_DYNAMIC_DRAW);
            
            GLint colorLoc = glGetUniformLocation(shader_program_, "color");
            if (colorLoc >= 0) {
                glUniform4f(colorLoc, section_color.r / 255.0f, section_color.g / 255.0f,
                           section_color.b / 255.0f, (section_color.a / 255.0f) * ui_alpha_ * text_alpha);
            }
            GLint useTextureLoc = glGetUniformLocation(shader_program_, "useTexture");
            if (useTextureLoc >= 0) {
                glUniform1i(useTextureLoc, 0);
            }
            
            glBindVertexArray(vao_);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
        }
        
        // Label
        int font_size = is_selected ? theme_->font_medium_size : theme_->font_small_size;
        ui::Color text_color = is_selected ? section_color : theme_->fg;
        float text_x = menu_x + 35.0f;
        int item_baseline_offset = body_font_manager_->get_baseline_at_size(font_size);
        float item_baseline = y + item_baseline_offset;
        draw_text(item.label, text_x, item_baseline, font_size, text_color, false, text_alpha);
        
        // Sublabel
        if (!item.sublabel.empty()) {
            float sublabel_baseline = item_baseline + font_size + 4.0f;
            draw_text(item.sublabel, text_x, sublabel_baseline, theme_->font_small_size, theme_->dim, false, text_alpha);
        }
    }
    
    // Footer hint - ensure it fits within the menu panel
    // Use a simple separator that renders reliably
    std::string hint = "SELECT to choose | Button 4 to close";
    int hint_width = body_font_manager_->get_text_width(hint, theme_->font_small_size);
    
    // Check if text fits within menu width (with padding)
    float menu_padding = 20.0f;  // Padding on each side
    float available_width = static_cast<float>(menu_width) - (menu_padding * 2.0f);
    
    // If text is too wide, use a shorter version or split it
    if (hint_width > available_width) {
        // Use shorter text that fits
        hint = "SELECT | Button 4 to close";
        hint_width = body_font_manager_->get_text_width(hint, theme_->font_small_size);
    }
    
    // Center the hint within the menu panel (not the entire screen)
    float hint_x = menu_x + menu_padding + (available_width - hint_width) / 2.0f;
    // Ensure it doesn't go past the menu edge
    if (hint_x + hint_width > menu_x + static_cast<float>(menu_width) - menu_padding) {
        hint_x = menu_x + menu_padding;  // Left-align if it still doesn't fit
    }
    
    float hint_baseline = static_cast<float>(height_ - 30) + body_font_manager_->get_baseline_at_size(theme_->font_small_size);
    draw_text(hint, hint_x, hint_baseline, theme_->font_small_size, theme_->dim, false, text_alpha);
}

void Renderer::render_volume_overlay(const app::AppState& state) {
    if (!state.show_volume_slider) return;

    // Overlay background
    float overlay_width = 400.0f;
    float overlay_height = 80.0f;
    float x = (width_ - overlay_width) / 2.0f;
    float y = height_ - 120.0f; // Bottom center
    
    ui::Color bg_color = theme_->bg;
    bg_color.a = 230; // Mostly opaque
    draw_quad(x, y, overlay_width, overlay_height, bg_color);
    
    // Border
    draw_line(x, y, x + overlay_width, y, 2.0f, theme_->accent);
    draw_line(x, y + overlay_height, x + overlay_width, y + overlay_height, 2.0f, theme_->accent);
    draw_line(x, y, x, y + overlay_height, 2.0f, theme_->accent);
    draw_line(x + overlay_width, y, x + overlay_width, y + overlay_height, 2.0f, theme_->accent);
    
    // Label
    std::string label = "MASTER VOLUME: " + std::to_string(state.master_volume) + "%";
    int font_size = theme_->font_medium_size;
    int label_width = body_font_manager_->get_text_width(label, font_size);
    float label_x = x + (overlay_width - label_width) / 2.0f;
    float label_y = y + 25.0f;
    draw_text(label, label_x, label_y, font_size, theme_->fg, false);
    
    // Slider bar background
    float bar_width = 300.0f;
    float bar_height = 10.0f;
    float bar_x = x + (overlay_width - bar_width) / 2.0f;
    float bar_y = y + 50.0f;
    ui::Color bar_bg = theme_->dim;
    draw_quad(bar_x, bar_y, bar_width, bar_height, bar_bg);
    
    // Slider fill
    float fill_width = (state.master_volume / 100.0f) * bar_width;
    if (fill_width > 0) {
        draw_quad(bar_x, bar_y, fill_width, bar_height, theme_->accent);
    }
}

void Renderer::render_game_browser(ui::SettingsMenuManager* menu, const std::vector<app::Playlist>& game_playlists, float menu_x, uint32_t menu_width, const ui::Color& section_color, float text_alpha, float background_alpha) {
    // Render game browser header
    std::string header_text = "Select Game Library";
    if (menu->is_viewing_games_in_playlist()) {
        int playlist_idx = menu->get_current_game_playlist_index();
        if (playlist_idx >= 0 && playlist_idx < static_cast<int>(game_playlists.size())) {
            header_text = game_playlists[playlist_idx].title;
        }
    }

    int header_width = title_font_manager_->get_text_width(header_text, theme_->font_heading_size);
    float header_x = menu_x + (static_cast<float>(menu_width) - header_width) / 2.0f;
    int header_baseline_offset = title_font_manager_->get_baseline_at_size(theme_->font_heading_size);
    float header_baseline = 8.0f + header_baseline_offset;
    draw_text(header_text, header_x, header_baseline, theme_->font_heading_size, section_color, true, text_alpha);

    // Underline
    float underline_y = header_baseline + 10.0f;
    draw_line(header_x, underline_y, header_x + header_width, underline_y, 2.0f, section_color, text_alpha);

    float start_y = underline_y + 30.0f;
    int item_height = 60;
    int max_visible = 7;

    if (menu->is_viewing_games_in_playlist()) {
        // Show games in current playlist
        int playlist_idx = menu->get_current_game_playlist_index();
        if (playlist_idx >= 0 && playlist_idx < static_cast<int>(game_playlists.size())) {
            const auto& playlist = game_playlists[playlist_idx];
            int selected_game = menu->get_selected_game_in_playlist();

            // Render game list (plus Back button)
            int total_items = static_cast<int>(playlist.items.size()) + 1; // +1 for Back button
            
            // Calculate scroll offset based on selected item
            // We need to access the menu's scroll offset logic, but it's internal to SettingsMenuManager
            // However, SettingsMenuManager::navigate updates scroll_offset_ for standard menus.
            // For game browser, we need to implement our own scrolling or expose it.
            // Looking at SettingsMenuManager::navigate, it updates scroll_offset_ but only for standard menus.
            // For game browser, it just updates the selected index.
            // So we need to calculate a local scroll offset here.
            
            static int game_scroll_offset = 0;
            // Update scroll offset to keep selected item in view
            if (selected_game < game_scroll_offset) {
                game_scroll_offset = selected_game;
            } else if (selected_game >= game_scroll_offset + max_visible) {
                game_scroll_offset = selected_game - max_visible + 1;
            }
            // Clamp
            if (game_scroll_offset < 0) game_scroll_offset = 0;
            if (game_scroll_offset > total_items - max_visible) game_scroll_offset = std::max(0, total_items - max_visible);
            
            for (int i = 0; i < max_visible; ++i) {
                int idx = game_scroll_offset + i;
                if (idx >= total_items) break;
                
                bool is_selected = (idx == selected_game);
                float y = start_y + static_cast<float>(i) * item_height;

                // Selection highlight
                if (is_selected) {
                    ui::Color highlight = section_color;
                    highlight.a = 40;
                    draw_quad(menu_x + 10.0f, y - 5.0f, static_cast<float>(menu_width) - 20.0f,
                             static_cast<float>(item_height), highlight, background_alpha);

                    // Selection indicator
                    float indicator_x = menu_x + 15.0f;
                    float indicator_y = y + item_height / 2.0f - 5.0f;
                    float size = 8.0f;
                    float right_x = indicator_x + size * 1.2f;
                    float triangle_vertices[] = {
                        indicator_x, indicator_y - size,   0.0f, 0.0f,
                        indicator_x, indicator_y + size,   1.0f, 0.0f,
                        right_x, indicator_y,               1.0f, 1.0f
                    };

                    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
                    glBufferData(GL_ARRAY_BUFFER, sizeof(triangle_vertices), triangle_vertices, GL_DYNAMIC_DRAW);

                    GLint colorLoc = glGetUniformLocation(shader_program_, "color");
                    if (colorLoc >= 0) {
                        glUniform4f(colorLoc, section_color.r / 255.0f, section_color.g / 255.0f,
                                   section_color.b / 255.0f, (section_color.a / 255.0f) * ui_alpha_ * text_alpha);
                    }
                    GLint useTextureLoc = glGetUniformLocation(shader_program_, "useTexture");
                    if (useTextureLoc >= 0) {
                        glUniform1i(useTextureLoc, 0);
                    }

                    glBindVertexArray(vao_);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                    glBindVertexArray(0);
                }

                // Render item text
                int font_size = is_selected ? theme_->font_medium_size : theme_->font_small_size;
                ui::Color text_color = is_selected ? section_color : theme_->fg;
                float text_x = menu_x + 35.0f;
                int item_baseline_offset = body_font_manager_->get_baseline_at_size(font_size);
                float item_baseline = y + item_baseline_offset;

                if (idx < static_cast<int>(playlist.items.size())) {
                    // Game item
                    draw_text(playlist.items[idx].title, text_x, item_baseline, font_size, text_color, false, text_alpha);

                    // System name as sublabel
                    if (!playlist.items[idx].emulator_system.empty()) {
                        float sublabel_baseline = item_baseline + font_size + 4.0f;
                        draw_text(playlist.items[idx].emulator_system, text_x, sublabel_baseline, theme_->font_small_size, theme_->dim, false, text_alpha);
                    }
                } else {
                    // Back button
                    draw_text("Back", text_x, item_baseline, font_size, text_color, false, text_alpha);
                }
            }
        }
    } else {
        // Show game playlists
        int selected_playlist = menu->get_game_browser_selected();

        // Render playlist list (plus Back button)
        int total_items = static_cast<int>(game_playlists.size()) + 1; // +1 for Back button
        
        static int playlist_scroll_offset = 0;
        // Update scroll offset
        if (selected_playlist < playlist_scroll_offset) {
            playlist_scroll_offset = selected_playlist;
        } else if (selected_playlist >= playlist_scroll_offset + max_visible) {
            playlist_scroll_offset = selected_playlist - max_visible + 1;
        }
        // Clamp
        if (playlist_scroll_offset < 0) playlist_scroll_offset = 0;
        if (playlist_scroll_offset > total_items - max_visible) playlist_scroll_offset = std::max(0, total_items - max_visible);

        for (int i = 0; i < max_visible; ++i) {
            int idx = playlist_scroll_offset + i;
            if (idx >= total_items) break;
            
            bool is_selected = (idx == selected_playlist);
            float y = start_y + static_cast<float>(i) * item_height;

            // Selection highlight
            if (is_selected) {
                ui::Color highlight = section_color;
                highlight.a = 40;
                draw_quad(menu_x + 10.0f, y - 5.0f, static_cast<float>(menu_width) - 20.0f,
                         static_cast<float>(item_height), highlight, background_alpha);

                // Selection indicator
                float indicator_x = menu_x + 15.0f;
                float indicator_y = y + item_height / 2.0f - 5.0f;
                float size = 8.0f;
                float right_x = indicator_x + size * 1.2f;
                float triangle_vertices[] = {
                    indicator_x, indicator_y - size,   0.0f, 0.0f,
                    indicator_x, indicator_y + size,   1.0f, 0.0f,
                    right_x, indicator_y,               1.0f, 1.0f
                };

                glBindBuffer(GL_ARRAY_BUFFER, vbo_);
                glBufferData(GL_ARRAY_BUFFER, sizeof(triangle_vertices), triangle_vertices, GL_DYNAMIC_DRAW);

                GLint colorLoc = glGetUniformLocation(shader_program_, "color");
                if (colorLoc >= 0) {
                    glUniform4f(colorLoc, section_color.r / 255.0f, section_color.g / 255.0f,
                               section_color.b / 255.0f, (section_color.a / 255.0f) * ui_alpha_ * text_alpha);
                }
                GLint useTextureLoc = glGetUniformLocation(shader_program_, "useTexture");
                if (useTextureLoc >= 0) {
                    glUniform1i(useTextureLoc, 0);
                }

                glBindVertexArray(vao_);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindVertexArray(0);
            }

            // Render item text
            int font_size = is_selected ? theme_->font_medium_size : theme_->font_small_size;
            ui::Color text_color = is_selected ? section_color : theme_->fg;
            float text_x = menu_x + 35.0f;
            int item_baseline_offset = body_font_manager_->get_baseline_at_size(font_size);
            float item_baseline = y + item_baseline_offset;

            if (idx < static_cast<int>(game_playlists.size())) {
                // Playlist item
                draw_text(game_playlists[idx].title, text_x, item_baseline, font_size, text_color, false, text_alpha);

                // Game count as sublabel
                std::string count_text = std::to_string(game_playlists[idx].items.size()) + " games";
                float sublabel_baseline = item_baseline + font_size + 4.0f;
                draw_text(count_text, text_x, sublabel_baseline, theme_->font_small_size, theme_->dim, false, text_alpha);
            } else {
                // Back button
                draw_text("Back", text_x, item_baseline, font_size, text_color, false, text_alpha);
            }
        }
    }
}

bool Renderer::compile_shaders() {
    uint32_t vertex_shader = compile_shader(vertex_shader_source, GL_VERTEX_SHADER);
    uint32_t fragment_shader = compile_shader(fragment_shader_source, GL_FRAGMENT_SHADER);
    
    if (vertex_shader == 0 || fragment_shader == 0) {
        return false;
    }
    
    shader_program_ = glCreateProgram();
    glAttachShader(shader_program_, vertex_shader);
    glAttachShader(shader_program_, fragment_shader);
    glLinkProgram(shader_program_);
    
    GLint success;
    glGetProgramiv(shader_program_, GL_LINK_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetProgramInfoLog(shader_program_, 512, nullptr, info_log);
        std::cerr << "Shader program linking failed: " << info_log << std::endl;
        return false;
    }
    
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    
    return true;
}

uint32_t Renderer::compile_shader(const std::string& source, uint32_t type) {
    uint32_t shader = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetShaderInfoLog(shader, 512, nullptr, info_log);
        std::cerr << "Shader compilation failed: " << info_log << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    
    return shader;
}


bool Renderer::compile_crt_shader() {
    uint32_t vertex_shader = compile_shader(vertex_shader_source, GL_VERTEX_SHADER);
    uint32_t fragment_shader = compile_shader(crt_fragment_shader_source, GL_FRAGMENT_SHADER);
    
    if (vertex_shader == 0 || fragment_shader == 0) {
        return false;
    }
    
    crt_shader_program_ = glCreateProgram();
    glAttachShader(crt_shader_program_, vertex_shader);
    glAttachShader(crt_shader_program_, fragment_shader);
    glLinkProgram(crt_shader_program_);
    
    GLint success;
    glGetProgramiv(crt_shader_program_, GL_LINK_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetProgramInfoLog(crt_shader_program_, 512, nullptr, info_log);
        std::cerr << "CRT Shader program linking failed: " << info_log << std::endl;
        return false;
    }
    
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    
    return true;
}

void Renderer::render_crt_effects(const app::AppState& state, bool scanlines_enabled) {
    if (crt_shader_program_ == 0) return;
    
    // Check if any effects are active
    const auto& s = state.display_settings;
    if (s.scanline_intensity <= 0.0f && s.warmth_intensity <= 0.0f && 
        s.glow_intensity <= 0.0f && s.rgb_mask_intensity <= 0.0f && 
        s.bloom_intensity <= 0.0f && s.interlacing_intensity <= 0.0f && 
        s.flicker_intensity <= 0.0f) {
        return;
    }
    
    glUseProgram(crt_shader_program_);
    
    // Set uniforms
    glUniform2f(glGetUniformLocation(crt_shader_program_, "screenSize"), static_cast<float>(width_), static_cast<float>(height_));
    
    auto now = std::chrono::steady_clock::now();
    float time = std::chrono::duration<float>(now.time_since_epoch()).count();
    glUniform1f(glGetUniformLocation(crt_shader_program_, "time"), time);
    
    // Scanlines are only enabled if the UI is visible (scanlines_enabled flag)
    // OR if scanline intensity is set to a value > 0 and we want to force them?
    // User request: "except for the scan lines. Make these only present during the video UI."
    // So if scanlines_enabled is false, we force intensity to 0.
    float effective_scanline_intensity = scanlines_enabled ? s.scanline_intensity : 0.0f;
    glUniform1f(glGetUniformLocation(crt_shader_program_, "scanlineIntensity"), effective_scanline_intensity);
    
    glUniform1f(glGetUniformLocation(crt_shader_program_, "warmthIntensity"), s.warmth_intensity);
    glUniform1f(glGetUniformLocation(crt_shader_program_, "glowIntensity"), s.glow_intensity);
    glUniform1f(glGetUniformLocation(crt_shader_program_, "rgbMaskIntensity"), s.rgb_mask_intensity);
    glUniform1f(glGetUniformLocation(crt_shader_program_, "bloomIntensity"), s.bloom_intensity);
    glUniform1f(glGetUniformLocation(crt_shader_program_, "interlacingIntensity"), s.interlacing_intensity);
    glUniform1f(glGetUniformLocation(crt_shader_program_, "flickerIntensity"), s.flicker_intensity);
    
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
    
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    
    // Restore standard shader
    glUseProgram(shader_program_);
}

void Renderer::cleanup() {
    if (shader_program_ != 0) {
        glDeleteProgram(shader_program_);
        shader_program_ = 0;
    }
    if (crt_shader_program_ != 0) {
        glDeleteProgram(crt_shader_program_);
        crt_shader_program_ = 0;
    }
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (vbo_ != 0) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (title_font_manager_) {
        title_font_manager_->cleanup();
    }
    if (body_font_manager_) {
        body_font_manager_->cleanup();
    }
}

void Renderer::render_qr_code(const std::string& url, float x, float y, float size, float alpha_multiplier) {
    if (url.empty()) return;
    
    try {
        // Generate QR code from URL
        qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        int qr_size = qr.getSize();
        if (qr_size <= 0) return;
        
        // Calculate module (cell) size
        float module_size = size / static_cast<float>(qr_size);
        
        // Add quiet zone (border) around QR code - standard is 4 modules
        int quiet_zone = 2;  // Use 2 for compact display
        float total_size = size + (quiet_zone * 2 * module_size);
        
        // Draw white background for QR code (including quiet zone)
        ui::Color white(255, 255, 255, 255);
        ui::Color black(0, 0, 0, 255);
        draw_quad(x - quiet_zone * module_size, y - quiet_zone * module_size, 
                  total_size, total_size, white, alpha_multiplier);
        
        // Draw black modules
        for (int row = 0; row < qr_size; row++) {
            for (int col = 0; col < qr_size; col++) {
                if (qr.getModule(col, row)) {  // True = black module
                    float px = x + col * module_size;
                    float py = y + row * module_size;
                    draw_quad(px, py, module_size, module_size, black, alpha_multiplier);
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to generate QR code: " << e.what() << std::endl;
    }
}

} // namespace ui

