#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
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

#ifdef MEDIA_BROWSER_ENABLED
namespace media_browser {
class ArtworkCache;
}
#endif

namespace ui {

class Renderer {
#ifdef MEDIA_BROWSER_ENABLED
    // Toast overlay needs access to private drawing helpers (draw_quad,
    // draw_text, draw_line) and to theme_/body_font_manager_ for measuring
    // and styling its centered panel. Kept as a friend to avoid widening
    // the public Renderer API for a single overlay primitive.
    friend class Toast;
#endif

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

#ifdef MEDIA_BROWSER_ENABLED
    // Placeholder full-screen overlay for the Media Browser screen. Replaced
    // by real screens (search / library / queue) in Task 17+. Draws a dark
    // background with centered "Movies — Media Browser" text + a hint to
    // press the Menu button to return to the main UI.
    void render_media_browser_placeholder();

    // Task 17 stub helper: draws a dark full-screen overlay with a centered
    // screen label (e.g. "Browse", "Search") and a "[Menu to return]" hint.
    // Used by the stub MbScreen implementations until Tasks 18-23 replace
    // each screen's render() with real UI.
    void render_media_browser_screen_stub(const std::string& label);

    // Task 18: minimal public drawing primitives that Media Browser screens
    // use to compose their own layouts (poster grids, category strips,
    // outlines, etc.). These are thin wrappers over the private draw_*
    // helpers, intentionally kept scoped to MEDIA_BROWSER_ENABLED so the
    // rest of the renderer API stays unchanged.
    void mb_fill_background();                           // Full-screen dark overlay
    void mb_fill_rect(float x, float y, float w, float h,
                      const ui::Color& color, float alpha_multiplier = 1.0f);
    void mb_stroke_rect(float x, float y, float w, float h, float thickness,
                        const ui::Color& color, float alpha_multiplier = 1.0f);
    // Draw a filled 5-point star centered at (cx, cy) with `outer_r` as the
    // bounding-circle radius. Inner radius is auto-derived to give classic
    // star proportions. Used by DetailScreen for the rating indicator so we
    // don't depend on the font shipping the U+2605 codepoint.
    void mb_fill_star(float cx, float cy, float outer_r,
                      const ui::Color& color, float alpha_multiplier = 1.0f);
    void mb_draw_text(const std::string& text, float x, float baseline_y,
                      int font_size, const ui::Color& color,
                      float alpha_multiplier = 1.0f);
    int  mb_text_width(const std::string& text, int font_size);
    int  mb_text_baseline(int font_size);
    // Retro-styled section headers and titles use the Zen Dots title font,
    // which is the same family the home-menu/playlist UI draws "Playlists"
    // and the product wordmark in. Body text stays on the body font.
    void mb_draw_title_text(const std::string& text, float x, float baseline_y,
                            int font_size, const ui::Color& color,
                            float alpha_multiplier = 1.0f);
    int  mb_title_text_width(const std::string& text, int font_size);
    int  mb_title_text_baseline(int font_size);
    // Stroke a single-pixel-thick line for underlines / section dividers —
    // the home menu uses these for the title and "Playlists" header.
    void mb_draw_line(float x1, float y1, float x2, float y2,
                      float thickness, const ui::Color& color,
                      float alpha_multiplier = 1.0f);
    // Filled triangle (3 vertices). Used for the blinking ◂ selection
    // marker next to focused buttons, matching the playlist-cursor idiom.
    void mb_fill_triangle(float x1, float y1, float x2, float y2,
                          float x3, float y3, const ui::Color& color,
                          float alpha_multiplier = 1.0f);
    const ui::Theme& mb_theme() const { return *theme_; }

    // Draw a poster at the given rect. If the ArtworkCache has a texture
    // for `url`, draws a textured quad; otherwise draws `fallback_tint`
    // as a filled rect AND enqueues the URL for background fetching.
    // Idempotent — safe to call every frame for the same URL.
    void mb_draw_poster_or_tint(const std::string& url,
                                float x, float y, float w, float h,
                                const ui::Color& fallback_tint,
                                float alpha_multiplier = 1.0f);

    // Like mb_draw_poster_or_tint but PRESERVES the image's native aspect
    // ratio. Letterboxes (top/bottom bars) or pillarboxes (left/right bars)
    // the texture inside (x, y, w, h), filling the empty space with a dim
    // copy of fallback_tint so the slot is still visually anchored. If the
    // poster is not yet loaded, fills the entire slot with fallback_tint.
    void mb_draw_poster_fit(const std::string& url,
                            float x, float y, float w, float h,
                            const ui::Color& fallback_tint,
                            float alpha_multiplier = 1.0f);

    // Called once per frame from the main loop. Drains any completed
    // background fetches and performs the GL uploads. Must run on the
    // GL-owning thread.
    void pump_artwork();

    // Renders the same seek bar overlay the main UI draws during scrubs.
    // The Media Browser's PlaybackScreen calls this so its scrub feedback
    // is visually identical to the kiosk's playlist scrubbing — same
    // colors, same position, same fade behavior. Reads
    // state.show_seek_bar / state.seek_bar_timer / state.position /
    // state.duration, all of which are already populated by the
    // controller and the screen's own input handler.
    void mb_render_seek_bar(const app::AppState& state);
#endif
    
    // Render CRT effects (scanlines, warmth, glow, etc.)
    // scanlines_enabled: if true, scanlines are rendered (based on settings), otherwise forced off
    void render_crt_effects(const app::AppState& state, bool scanlines_enabled);

    // Render error overlay banner
    void render_error_overlay(const app::AppState& state);

    // Cleanup
    void cleanup();
    
    // Reset GL resources after external context takeover (e.g., RetroArch)
    // This invalidates current GL resources and triggers re-creation on next render
    void reset_gl();
    
    // Bezel management for Modern TV mode
    bool load_bezel(const std::string& path);  // Load bezel texture from file
    void render_bezel();  // Render bezel overlay (fullscreen)
    
    // Set content viewport dimensions for Modern TV mode (4:3 pillarboxing)
    // This affects the projection matrix used for rendering
    void set_content_viewport(int width, int height);
    void reset_content_viewport();  // Reset to full screen dimensions
    void resize_screen(uint32_t width, uint32_t height);

private:
    uint32_t width_;
    uint32_t height_;
    uint32_t original_width_;   // Store original screen dimensions for reset
    uint32_t original_height_;
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
    
    // Bezel overlay
    uint32_t bezel_texture_id_ = 0;
    int bezel_width_ = 0;
    int bezel_height_ = 0;
    std::string current_bezel_path_;

    // Game thumbnail
    uint32_t thumbnail_texture_id_ = 0;
    int thumbnail_width_ = 0;
    int thumbnail_height_ = 0;
    std::string current_thumbnail_path_;
    bool load_thumbnail(const std::string& rom_path);

#ifdef MEDIA_BROWSER_ENABLED
    // Async poster/artwork cache (main thread API, background-thread
    // fetcher). Owned by the Renderer so its lifetime tracks the GL
    // context lifetime. Pointer keeps the artwork_cache.h include out
    // of this header.
    std::unique_ptr<media_browser::ArtworkCache> artwork_cache_;
    // Lazy first-use init so the background thread doesn't start
    // unless the Media Browser is actually used.
    media_browser::ArtworkCache& artwork_cache();
    // Textured-quad drawing helper (used by mb_draw_poster_or_tint).
    void draw_textured_quad(uint32_t tex_id, float x, float y, float w, float h,
                            float alpha_multiplier);
#endif

    // System logo cache (white-on-transparent logos for each console)
    struct CachedLogo {
        uint32_t texture_id = 0;
        int width = 0;
        int height = 0;
    };
    std::unordered_map<std::string, CachedLogo> system_logo_cache_;
    const CachedLogo* get_system_logo(const std::string& system_key);
    std::string get_system_key(const app::Playlist& playlist) const;
    
    // Helper methods
    void draw_quad(float x, float y, float w, float h, const ui::Color& color, float alpha_multiplier = 1.0f);
    void draw_text(const std::string& text, float x, float y, int font_size, const ui::Color& color, bool use_title_font = false, float alpha_multiplier = 1.0f);
    void draw_glyph(char32_t codepoint, float x, float baseline_y, int font_size, const ui::Color& color, float alpha_multiplier = 1.0f);
    void draw_line(float x1, float y1, float x2, float y2, float width, const ui::Color& color, float alpha_multiplier = 1.0f);
    
    // Component renderers
    void render_title(float text_alpha = 1.0f, bool video_active = false, bool ui_visible_when_playing = false);
    void render_playlist_list(const std::vector<app::Playlist>& playlists, int selected_index, int scroll_offset, bool video_active, bool ui_visible_when_playing, int current_playlist_index = -1);
    void render_footer(const app::AppState& state, float text_alpha = 1.0f, bool video_active = false, bool ui_visible_when_playing = false);
    void render_volume_overlay(const app::AppState& state);
    void render_seek_bar(const app::AppState& state);
    void render_scanlines(); // Deprecated, replaced by render_crt_effects
    void render_settings_menu(SettingsMenuManager* menu, const std::vector<app::Playlist>& game_playlists, bool video_active, bool ui_visible_when_playing);
    void render_game_browser(SettingsMenuManager* menu, const std::vector<app::Playlist>& game_playlists, float menu_x, uint32_t menu_width, const ui::Color& section_color, float text_alpha, float background_alpha);
    void render_virtual_keyboard(const VirtualKeyboard& keyboard);
    void render_qr_code(const std::string& url, float x, float y, float size, float alpha_multiplier = 1.0f);
    
    // Helper: format time as MM:SS
    std::string format_time(double seconds);
    
    // Shader compilation
    bool compile_shaders();
    bool compile_crt_shader();
    uint32_t compile_shader(const std::string& source, uint32_t type);
};

} // namespace ui

