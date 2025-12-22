#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "theme.h"  // For Color type

namespace ui {
class Theme;
class FontManager;
class SettingsMenuManager;
class VirtualKeyboard;
}

namespace app {
struct Playlist;
struct AppState;
}

namespace ui {

class Renderer {
public:
    Renderer(uint32_t width, uint32_t height);
    ~Renderer();

    // Initialize renderer (load fonts, set up GL state)
    // title_font_path: Zen Dots for title/heading
    // body_font_path: Mono font for body text
    bool initialize(const std::string& title_font_path, const std::string& body_font_path);
    
    // Set UI alpha (0.0 = fully transparent, 1.0 = fully opaque)
    void set_ui_alpha(float alpha) { ui_alpha_ = alpha; }
    
    // Render UI overlay
    void render(const app::AppState& state);
    
    // Render loading overlay
    void render_loading_overlay(const app::AppState& state);
    
    // Render CRT effects (scanlines, warmth, glow, etc.)
    // scanlines_enabled: if true, scanlines are rendered (based on settings), otherwise forced off
    void render_crt_effects(const app::AppState& state, bool scanlines_enabled);

    // Cleanup
    void cleanup();
    
    // Reset GL resources after external context takeover (e.g., RetroArch)
    // This invalidates current GL resources and triggers re-creation on next render
    void reset_gl();

private:
    uint32_t width_;
    uint32_t height_;
    float ui_alpha_;
    
    std::unique_ptr<Theme> theme_;
    std::unique_ptr<FontManager> title_font_manager_;  // Zen Dots for title/heading
    std::unique_ptr<FontManager> body_font_manager_;   // Mono for body text
    
    // GL state
    uint32_t shader_program_;
    uint32_t crt_shader_program_; // Shader for CRT effects
    uint32_t vao_;
    uint32_t vbo_;
    
    // Logo
    uint32_t logo_texture_id_;
    int logo_width_;
    int logo_height_;
    
    // Helper methods
    void draw_quad(float x, float y, float w, float h, const ui::Color& color, float alpha_multiplier = 1.0f);
    void draw_text(const std::string& text, float x, float y, int font_size, const ui::Color& color, bool use_title_font = false, float alpha_multiplier = 1.0f);
    void draw_line(float x1, float y1, float x2, float y2, float width, const ui::Color& color, float alpha_multiplier = 1.0f);
    
    // Component renderers
    void render_title(float text_alpha = 1.0f, bool video_active = false, bool ui_visible_when_playing = false);
    void render_playlist_list(const std::vector<app::Playlist>& playlists, int selected_index, bool video_active, bool ui_visible_when_playing);
    void render_footer(const app::AppState& state, float text_alpha = 1.0f, bool video_active = false, bool ui_visible_when_playing = false);
    void render_volume_overlay(const app::AppState& state);
    void render_scanlines(); // Deprecated, replaced by render_crt_effects
    void render_settings_menu(SettingsMenuManager* menu, const std::vector<app::Playlist>& game_playlists, bool video_active, bool ui_visible_when_playing);
    void render_game_browser(SettingsMenuManager* menu, const std::vector<app::Playlist>& game_playlists, float menu_x, uint32_t menu_width, const ui::Color& section_color, float text_alpha, float background_alpha);
    void render_virtual_keyboard(const VirtualKeyboard& keyboard);
    
    // Helper: format time as MM:SS
    std::string format_time(double seconds);
    
    // Shader compilation
    bool compile_shaders();
    bool compile_crt_shader();
    uint32_t compile_shader(const std::string& source, uint32_t type);
};

} // namespace ui

