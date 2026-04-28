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

    // Current content viewport dimensions — the logical coordinate
    // space the projection matrix is set up for. Differs from the
    // HDMI mode size when set_content_viewport() has narrowed the
    // visible area (e.g., 960×720 inside 1280×720 for Modern TV's
    // 4:3 letterbox, or 640×480 for CRT_NATIVE). Overlay code
    // (Toast, error banner, anything centered or coordinate-aware)
    // should call these instead of using `mode.width/height`,
    // otherwise the overlay's logical coordinates project past the
    // visible area and the operator sees a partially-clipped panel
    // in the corner of the screen.
    int get_width() const { return static_cast<int>(width_); }
    int get_height() const { return static_cast<int>(height_); }
    
    // Render UI overlay.
    //
    // Non-const because the renderer publishes back computed layout
    // state (e.g., state.playlist_max_visible) so the input handler
    // in main.cpp can scroll only when the selection actually goes
    // off-screen for the current viewport size, instead of using a
    // hardcoded CRT-tuned row count.
    void render(app::AppState& state);
    
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
    // state.show_seek_bar / state.seek_bar_timer / state.get_position() /
    // state.get_duration(), all of which are already populated by the
    // controller and the screen's own input handler.
    void mb_render_seek_bar(const app::AppState& state);

    // Bind the UI shader program, enable alpha blending, and set the
    // screenSize uniform. Required before calling any of the mb_*
    // drawing helpers (mb_fill_rect, mb_draw_text, etc.) when the
    // caller cannot rely on render(state) having run first.
    //
    // Why this exists: render(state) does the same setup at the top of
    // its body (the "CRITICAL: Reset OpenGL state after mpv renders"
    // block), but the Media Browser dispatcher in main.cpp skips
    // render(state) entirely when current_screen == MediaBrowser. That
    // skip leaves whatever shader gst_renderer.render() most recently
    // bound (a YUV→RGB sampler) as the active program — so all
    // subsequent draw_quad / draw_text calls silently bind uniforms on
    // the wrong program and the geometry renders as garbage (or not at
    // all). PlaybackScreen, where gst_renderer runs every frame just
    // before the MB dispatch, was the worst-affected path: scrub
    // overlay draws were invisible and the Detail screen on exit
    // showed only fragments of the last video frame ("green rectangle
    // upper-right"). Calling this from the MB dispatch path guarantees
    // the UI shader is bound before any MB screen renders.
    void mb_begin_2d_state();
#endif
    
    // Render CRT effects (scanlines, warmth, glow, etc.)
    // scanlines_enabled: if true, scanlines are rendered (based on settings), otherwise forced off
    //
    // In the legacy path this draws a procedural alpha-blended overlay.
    // In the enhanced path (begin_scene_fbo returned true), this becomes a
    // no-op — the effects are deferred to end_scene_fbo_and_composite
    // where the shader has direct access to the scene texture.
    void render_crt_effects(const app::AppState& state, bool scanlines_enabled);

    // ----- Enhanced CRT pipeline (Phase 1: render-to-FBO + composite) -----
    //
    // The enhanced pipeline routes the kiosk's video+UI into an offscreen
    // RGBA8 texture (the "scene FBO"), then composites that texture back
    // to the default framebuffer through a shader that can sample the
    // scene pixels directly. This is the substrate for higher-fidelity
    // CRT effects (Lottes mask, brightness-modulated scanlines, color
    // matrix, halation) that are impossible to express as a procedural
    // alpha-blend overlay because they need to react to the underlying
    // pixel content.
    //
    // Lifecycle:
    //   - begin_scene_fbo() — call after clearing the default framebuffer
    //     and computing letterbox/viewport math, BEFORE video/UI render.
    //     Returns true if it actually bound the FBO; the caller MUST then
    //     call end_scene_fbo_and_composite() before the bezel/toast pass.
    //     Returns false in any of these cases (then caller behaves
    //     exactly like the legacy path):
    //       * enhanced_crt_enabled flag is off in settings
    //       * the Media Browser screen is active (its content is 16:9
    //         and the CRT effects are intentionally main-UI only)
    //       * all 7 effect intensities are 0 (no point paying FBO cost)
    //       * FBO creation failed (e.g. driver out of memory)
    //   - end_scene_fbo_and_composite() — call after ui_renderer.render(),
    //     before bezel/toast. Pairs with begin_scene_fbo. Safe no-op when
    //     begin_scene_fbo returned false.
    bool begin_scene_fbo(const app::AppState& state);
    void end_scene_fbo_and_composite(const app::AppState& state);

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
    uint32_t crt_shader_program_;            // Legacy: procedural alpha-blend overlay
    uint32_t crt_composite_shader_program_;  // Enhanced: samples scene FBO + applies effects
    uint32_t vao_;
    uint32_t vbo_;

    // Enhanced CRT pipeline — offscreen scene FBO state.
    //   scene_fbo_: GL framebuffer object name (0 when not yet created)
    //   scene_color_tex_: RGBA8 color attachment texture
    //   scene_fbo_width_/height_: cached creation size; if these don't
    //     match original_width_/height_ when begin_scene_fbo runs, the
    //     FBO is destroyed and recreated at the new size (handles
    //     resize_screen() after display reconnect)
    //   scene_fbo_active_: true between begin_scene_fbo and
    //     end_scene_fbo_and_composite. Read by render_crt_effects() to
    //     skip the legacy overlay pass while in enhanced mode.
    //   last_scanlines_enabled_: stashed by Renderer::render() so the
    //     deferred composite knows whether to draw scanlines (the
    //     "scanlines only when UI overlay is visible" rule).
    uint32_t scene_fbo_ = 0;
    uint32_t scene_color_tex_ = 0;
    uint32_t scene_fbo_width_ = 0;
    uint32_t scene_fbo_height_ = 0;
    bool scene_fbo_active_ = false;
    bool last_scanlines_enabled_ = true;

    // Phase 5 — halation/bloom downsample chain. Two intermediate
    // FBOs at 1/2 and 1/4 resolution feed a Kawase-style soft blur
    // that the main composite shader samples to add wide-radius
    // halation around bright pixels (modeling phosphor light bleed
    // on a real CRT).
    //
    // Lifecycle:
    //   - Lazily created on first use when bloomIntensity > 0.
    //   - Skipped entirely (no FBOs allocated, no passes run) when
    //     bloomIntensity == 0 — slider-OFF stays free.
    //   - Recreated on resize_screen() and reset_gl() through the
    //     same destroy/ensure pattern as the scene FBO.
    uint32_t bloom_a_fbo_ = 0;          // half-res FBO (1/2 in each axis)
    uint32_t bloom_a_tex_ = 0;
    uint32_t bloom_b_fbo_ = 0;          // quarter-res FBO (1/4 in each axis)
    uint32_t bloom_b_tex_ = 0;
    uint32_t bloom_a_width_ = 0;
    uint32_t bloom_a_height_ = 0;
    uint32_t bloom_b_width_ = 0;
    uint32_t bloom_b_height_ = 0;
    uint32_t bloom_downsample_shader_program_ = 0;
    
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
    bool compile_crt_shader();             // Legacy procedural-overlay shader
    bool compile_crt_composite_shader();   // Enhanced scene-sampling shader
    bool compile_bloom_downsample_shader();// Phase 5 Kawase-blur downsample
    uint32_t compile_shader(const std::string& source, uint32_t type);

    // Enhanced CRT pipeline helpers.
    //   ensure_scene_fbo: lazily create or recreate the scene FBO at
    //     the current screen size. Sets scene_fbo_=0 on failure.
    //   destroy_scene_fbo: free FBO + color texture. Safe to call on
    //     an already-empty FBO. Used by cleanup() and reset_gl().
    //   ensure_bloom_fbos / destroy_bloom_fbos: same pattern for the
    //     1/2-res and 1/4-res halation downsample chain (Phase 5).
    //     Only allocated when bloomIntensity > 0.
    void ensure_scene_fbo(uint32_t fb_width, uint32_t fb_height);
    void destroy_scene_fbo();
    void ensure_bloom_fbos(uint32_t base_w, uint32_t base_h);
    void destroy_bloom_fbos();
};

} // namespace ui

