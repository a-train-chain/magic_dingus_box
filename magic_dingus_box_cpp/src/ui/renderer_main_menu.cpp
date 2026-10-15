// ui::Renderer — The kiosk's main UI frame: render(state) and the main-menu pieces it
// draws (title, playlist list, footer, overlays, seek/volume bars,
// on-screen keyboard).
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

void Renderer::render(app::AppState& state) {
    // One batching scope for the whole UI pass (menu, settings, wizard,
    // pairing, keyboard); flushed on every return path.
    BatchScope batch_scope(*this);
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
        flush_ui_batch();
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
    }

    // Fullscreen-playback fast path: skip the entire UI render when nothing
    // would be visible anyway. The previously commented-out early-return at
    // ui_overlay_alpha == 0 was disabled because CRT effects need the
    // pipeline to run; we now correctly gate on whether ANY CRT effect is
    // active AND whether other always-visible UI elements (scrub bar,
    // settings menu) are showing. Saves ~3-5% CPU + reduces GPU contention
    // with v4l2h264dec on the Pi 4 during fullscreen 1080p playback.
    if (ui_overlay_alpha <= 0.0f) {
        const auto& s = state.display_settings;
        const bool any_crt_effect = (s.scanline_intensity   > 0.0f ||
                                     s.warmth_intensity     > 0.0f ||
                                     s.glow_intensity       > 0.0f ||
                                     s.rgb_mask_intensity   > 0.0f ||
                                     s.bloom_intensity      > 0.0f ||
                                     s.interlacing_intensity > 0.0f ||
                                     s.flicker_intensity    > 0.0f);
        const bool settings_open = state.settings_menu &&
            (state.settings_menu->is_active()  ||
             state.settings_menu->is_opening() ||
             state.settings_menu->is_closing());
        const bool seek_visible = state.show_seek_bar;
        if (!any_crt_effect && !settings_open && !seek_visible) {
            // Nothing would draw; skip the rest of the UI render entirely.
            return;
        }
    }
    // Otherwise (no video active), render UI normally (ui_overlay_alpha = 1.0f from initial value)
    
    // CRITICAL: Reset OpenGL state after mpv renders to ensure consistent text rendering
    // mpv may change blending, texture state, etc. that affects UI rendering
    flush_ui_batch();
    glEnable(GL_BLEND);
    flush_ui_batch();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    flush_ui_batch();
    glDisable(GL_DITHER);
    flush_ui_batch();
    glActiveTexture(GL_TEXTURE0);  // Ensure we're using texture unit 0
    
    flush_ui_batch();
    glUseProgram(shader_program_);
    if (shader_program_ == 0) {
        std::cerr << "ERROR: Shader program is 0!" << std::endl;
        return;
    }
    
    // Set screen size uniform
    GLint screenSizeLoc = u_screen_size_loc_;
    if (screenSizeLoc < 0) {
        std::cerr << "Warning: screenSize uniform not found" << std::endl;
    } else {
        static bool logged_screensize = false;
        if (!logged_screensize) {
            std::cout << "UI Renderer: render() screenSize=" << width_ << "x" << height_ << std::endl;
            logged_screensize = true;
        }
        flush_ui_batch();
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
        
        // Render UI components.
        render_title(text_alpha, state.video_active, state.ui_visible_when_playing);

        // Publish the actual on-screen row count to state so
        // main.cpp's input handler scrolls only when the selection
        // really goes off-screen for the current viewport.
        // Mirrors the math inside render_playlist_list — see the
        // detailed comment block there for the derivation.
        {
            const int title_baseline_offset =
                title_font_manager_->get_baseline_at_size(theme_->font_title_size);
            const float title_baseline = 8.0f + title_baseline_offset;
            const int title_line_height =
                static_cast<int>(theme_->font_title_size * 1.2f);
            const float header_baseline = title_baseline + title_line_height + 24.0f;
            const int header_line_height =
                static_cast<int>(theme_->font_heading_size * 1.2f);
            const float start_y =
                header_baseline + header_line_height + 4.0f + 40.0f;
            const float bottom_reserve = 25.0f;
            const float available_h =
                static_cast<float>(height_) - start_y - bottom_reserve;
            const int row_h = std::max(1, theme_->playlist_item_height);
            state.playlist_max_visible =
                std::max(8, static_cast<int>(available_h / row_h));
        }

        render_playlist_list(state.playlists, state.selected_index, state.playlist_scroll_offset, state.video_active, state.ui_visible_when_playing, state.current_playlist_index);
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

    // Render seek progress bar (only during active seeking via rotary encoder)
    render_seek_bar(state);

    // Render error overlay banner (if any error message is set)
    render_error_overlay(state);

    // Render settings menu if active (on top of everything, before scanlines)
    if (state.settings_menu && state.settings_menu->is_active()) {

        if (state.settings_menu->is_controller_wizard_active()) {
            // Controller Setup wizard replaces the regular settings panel,
            // same as the pairing screen below.
            ui::ControllerWizard* wiz = state.settings_menu->controller_wizard();
            if (wiz) render_controller_wizard(*wiz);
        } else if (state.settings_menu->is_pairing_screen_active()) {
            // Phone Remote pairing screen replaces the regular settings panel.
            // Use the shared mtime-based cache so this and main.cpp's input
            // handler always see the same device list (no divergence after
            // forget).
            const auto& cached_devices = ui::paired_devices_cached(
                config::get_data_path() + "/paired_remotes.json");
            ui::PairingScreen* ps = state.settings_menu->pairing_screen();
            if (ps) {
                render_pairing_screen(*ps, cached_devices,
                                      state.lan_ip, state.hostname);
            }
        } else {
        // The settings panel itself. (The Content Manager Info screen's
        // QR used to render here when the INFO submenu was open; that
        // screen was merged into the "Connect a Device" pairing screen,
        // which is the one connection surface now.)
        render_settings_menu(state.settings_menu, state.game_playlists, state.video_active, state.ui_visible_when_playing);
        } // end else (neither the wizard nor the pairing screen is active)
    }

    // Virtual Keyboard overlay
    if (state.keyboard && state.keyboard->is_active()) {
        render_virtual_keyboard(*state.keyboard);
    }
        
    // Apply CRT effects (scanlines, warmth, glow, etc.)
    // These are rendered as an overlay on top of everything in the
    // legacy path. In the enhanced path the call is a no-op
    // (scene_fbo_active_) and the effects are deferred to
    // end_scene_fbo_and_composite, which reads last_scanlines_enabled_
    // because it runs OUTSIDE this function and doesn't see
    // ui_overlay_alpha directly.
    last_scanlines_enabled_ = (ui_overlay_alpha > 0.0f);
    render_crt_effects(state, last_scanlines_enabled_);
    
    // Check for errors after rendering
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::cerr << "OpenGL error after render: " << err << std::endl;
    }
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
    
    // Title — baseline derived from the panel's top border plus the
    // shared minimum inset (padding audit: the old fixed start_y-50
    // baseline left the title's cap height ~8 px off the border line).
    const float title_baseline =
        (start_y - 80.0f) + ui::overlay::kMinTextInset
        + static_cast<float>(title_font_manager_->get_baseline_at_size(24));
    draw_text(keyboard.get_title(), start_x, title_baseline, 24, theme_->fg, true);

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
    // Centered on real metrics inside the input box (the old start_y-12
    // baseline put the glyph tops 8 px ABOVE the box's top edge and left
    // a 30 px void above the underline).
    const float input_baseline =
        (start_y - 20.0f) + (input_box_height - 20.0f) / 2.0f
        + static_cast<float>(body_font_manager_->get_baseline_at_size(20));
    draw_text(display_text, start_x + 10, input_baseline, 20, theme_->fg);
    
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
            
            // Proper font-based centering
            float text_width = static_cast<float>(body_font_manager_->get_text_width(label, font_size));
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
        
        flush_ui_batch();
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
        
        // Use white color to render texture as-is (multiplied by alpha)
        glUniform4f(u_color_loc_,
                    1.0f, 1.0f, 1.0f, ui_alpha_ * text_alpha);
        glUniform1i(u_use_texture_loc_, 1); // Enable texture
        
        glBindTexture(GL_TEXTURE_2D, logo_texture_id_);
        
        glBindVertexArray(vao_);
        UI_DRAW_ARRAYS(GL_TRIANGLE_STRIP, 0, 4);
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

void Renderer::render_playlist_list(const std::vector<app::Playlist>& playlists, int selected_index, int scroll_offset, bool video_active, bool ui_visible_when_playing, int current_playlist_index) {
    // Debug: Log playlist rendering
    static int playlist_render_count = 0;
    if (playlist_render_count < 2) {
        std::cout << "    Rendering playlist list: " << playlists.size() << " playlists, selected=" << selected_index << ", scroll=" << scroll_offset << std::endl;
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
    float start_y = header_baseline + header_line_height + 4.0f + 40.0f;
    
    // Compute how many rows fit between start_y and the bottom of
    // the screen.
    //
    // Was hardcoded `max_visible = 8`, sized for CRT native (640x480).
    // In Modern TV the UI viewport is 4:3 within 16:9 (~960x720), so
    // there's room for many more rows. With the old cap, the playlist
    // visibly stopped ~2/3 down the screen even with 20 items.
    //
    // The playlist column lives on the LEFT side of the screen; the
    // footer (status text, time, video info) is right-aligned and
    // does not overlap, so we don't need to clear it. The only thing
    // below the last visible row is the down-scroll arrow:
    //
    //   arrow_y      = last_row_bottom + arrow_margin/2  (≈ +6)
    //   arrow_height = arrow_size * 1.2                  (≈ 7)
    //   total tail   ≈ 13 px below last row
    //
    // Plus a small breathing-room margin so the arrow isn't flush
    // against the screen edge. bottom_reserve = 13 (arrow) + 12
    // (margin) = 25.
    //
    // Modern TV (720): (720 - 166 - 25) / 36 = 14 rows  → last bottom 670, 37px free.
    // CRT (480):       (480 - 166 - 25) / 36 = 8 rows   → last bottom 454, 13px free.
    //
    // Floor at 8 stays as a safety net (CRT is already 8 by formula
    // but a future tweak to start_y or font sizes could push it to 7
    // and silently regress the established CRT layout — the floor
    // protects against that).
    const float bottom_reserve = 25.0f;  // down-arrow tail + breathing room
    const float available_h = static_cast<float>(height_) - start_y - bottom_reserve;
    const int row_h = std::max(1, theme_->playlist_item_height);
    int max_visible = std::max(8, static_cast<int>(available_h / row_h));
    if (max_visible > static_cast<int>(playlists.size())) {
        max_visible = static_cast<int>(playlists.size());
    }
    
    // Determine alpha multipliers based on video state
    // When video is active and UI is visible: text fully opaque (1.0), backgrounds 50% transparent (0.5)
    // When video is not active: everything fully opaque (1.0)
    float text_alpha = (video_active && ui_visible_when_playing) ? 1.0f : 1.0f;
    // background_alpha is no longer used in this function (highlight bar uses fixed alpha)
    // Removed to eliminate unused variable warning
    
    // Blinking indicator (time-based, matching Python: 500ms). The phase
    // comes from main_menu_blink_phase() so the redraw gate sees the same
    // flips this draws.
    bool indicator_visible =
        main_menu_blink_phase(std::chrono::steady_clock::now()) % 2 == 0;
    
    // Calculate if we need to show scroll arrows
    int total_playlists = static_cast<int>(playlists.size());
    bool show_up_arrow = scroll_offset > 0;
    bool show_down_arrow = scroll_offset + max_visible < total_playlists;
    
    // Reserve space for arrows if needed
    float arrow_size = 6.0f;  // Smaller arrows for better spacing
    float arrow_margin = 12.0f;
    // Position arrows aligned with center of "Playlists" header text
    // Header starts at margin_x + 36.0f, "Playlists" is ~100px wide, so center is ~50px in
    float header_x = static_cast<float>(theme_->margin_x) + 36.0f;
    float header_text_width = static_cast<float>(title_font_manager_->get_text_width("Playlists", theme_->font_heading_size));
    float arrow_center_x = header_x + (header_text_width / 2.0f);  // Center of "Playlists" text
    float y = start_y;
    
    // If showing up arrow, draw it just above the first visible playlist item
    // Position it symmetrically with how the down arrow is positioned below the last item
    if (show_up_arrow) {
        float arrow_y = start_y - arrow_margin;  // Position above start of playlist area
        
        // Draw triangle pointing UP
        float triangle_vertices[] = {
            arrow_center_x - arrow_size, arrow_y + arrow_size * 1.2f, 0.0f, 0.0f,  // Bottom-left
            arrow_center_x + arrow_size, arrow_y + arrow_size * 1.2f, 1.0f, 0.0f,  // Bottom-right  
            arrow_center_x, arrow_y,                                  0.5f, 1.0f   // Top (pointing up)
        };
        
        flush_ui_batch();
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(triangle_vertices), triangle_vertices, GL_DYNAMIC_DRAW);
        
        GLint colorLoc = u_color_loc_;
        if (colorLoc >= 0) {
            glUniform4f(colorLoc, theme_->accent2.r / 255.0f, theme_->accent2.g / 255.0f, 
                       theme_->accent2.b / 255.0f, (theme_->accent2.a / 255.0f) * ui_alpha_ * text_alpha);
        }
        GLint useTextureLoc = u_use_texture_loc_;
        if (useTextureLoc >= 0) {
            glUniform1i(useTextureLoc, 0);
        }
        
        glBindVertexArray(vao_);
        UI_DRAW_ARRAYS(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
    }
    
    // Render visible playlists starting from scroll_offset
    for (int i = 0; i < max_visible && (scroll_offset + i) < total_playlists; i++) {
        int playlist_idx = scroll_offset + i;
        const auto& pl = playlists[playlist_idx];
        bool selected = (playlist_idx == selected_index);
        
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
        
        // Channel number - regular playlists start from 01 (Master Shuffle has no number)
        char channel_buf[16];
        snprintf(channel_buf, sizeof(channel_buf), "%02d.", playlist_idx);
        std::string channel_num = channel_buf;

        // Playlist title only (curator removed per user request)
        std::string text = pl.title;

        int font_size = selected ? theme_->font_large_size : theme_->font_medium_size;
        bool is_now_playing = (playlist_idx == current_playlist_index && current_playlist_index >= 0);
        ui::Color text_color = selected ? theme_->accent2 : (is_now_playing ? theme_->highlight1 : theme_->fg);
        ui::Color channel_color = is_now_playing ? theme_->highlight1 : theme_->dim;

        // Use a common baseline for all text on this line (based on the larger font)
        int item_baseline_offset = body_font_manager_->get_baseline_at_size(font_size);
        float item_baseline = y + item_baseline_offset;

        float text_x = static_cast<float>(theme_->margin_x) + 36.0f;

        if (playlist_idx == 0) {
            // Draw shuffle icon (crossed arrows) instead of channel number for Master Shuffle
            float line_height = static_cast<float>(font_size) * 1.2f;
            float icon_cy = y + line_height * 0.45f;
            float icon_x = static_cast<float>(theme_->margin_x) + 2.0f;
            float icon_w = 16.0f;
            float icon_h = 10.0f;
            float half_h = icon_h / 2.0f;
            float arrow_w = 4.0f;
            float line_body = icon_w - arrow_w;
            float t = 1.4f;  // line half-thickness

            // Perpendicular offset for diagonal lines
            float diag_len = sqrtf(line_body * line_body + half_h * half_h);
            float px = (half_h / diag_len) * t;
            float py = (line_body / diag_len) * t;

            // Line endpoints
            float x1 = icon_x,             y1 = icon_cy + half_h;   // bottom-left
            float x2 = icon_x + line_body, y2 = icon_cy - half_h;   // top-right
            float x3 = icon_x,             y3 = icon_cy - half_h;   // top-left
            float x4 = icon_x + line_body, y4 = icon_cy + half_h;   // bottom-right
            float ah = 2.8f;  // arrowhead half-height

            float verts[] = {
                // Line 1: bottom-left → top-right
                x1 - px, y1 - py, 0, 0,   x1 + px, y1 + py, 0, 0,   x2 + px, y2 + py, 0, 0,
                x1 - px, y1 - py, 0, 0,   x2 + px, y2 + py, 0, 0,   x2 - px, y2 - py, 0, 0,
                // Line 2: top-left → bottom-right
                x3 + px, y3 - py, 0, 0,   x3 - px, y3 + py, 0, 0,   x4 - px, y4 + py, 0, 0,
                x3 + px, y3 - py, 0, 0,   x4 - px, y4 + py, 0, 0,   x4 + px, y4 - py, 0, 0,
                // Arrowhead at top-right
                x2, y2 - ah, 0, 0,   x2, y2 + ah, 0, 0,   x2 + arrow_w, y2, 0, 0,
                // Arrowhead at bottom-right
                x4, y4 - ah, 0, 0,   x4, y4 + ah, 0, 0,   x4 + arrow_w, y4, 0, 0,
            };

            flush_ui_batch();
            glBindBuffer(GL_ARRAY_BUFFER, vbo_);
            glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
            GLint colorLoc = u_color_loc_;
            if (colorLoc >= 0) {
                glUniform4f(colorLoc, channel_color.r / 255.0f, channel_color.g / 255.0f,
                           channel_color.b / 255.0f, (channel_color.a / 255.0f) * ui_alpha_ * text_alpha);
            }
            GLint useTextureLoc = u_use_texture_loc_;
            if (useTextureLoc >= 0) glUniform1i(useTextureLoc, 0);
            glBindVertexArray(vao_);
            UI_DRAW_ARRAYS(GL_TRIANGLES, 0, 18);
            glBindVertexArray(0);
        } else {
            // Draw channel number for regular playlists
            draw_text(channel_num, static_cast<float>(theme_->margin_x), item_baseline, theme_->font_small_size, channel_color, false, text_alpha);
        }

        // Draw playlist text on the same baseline - use body font
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
            
            flush_ui_batch();
            glBindBuffer(GL_ARRAY_BUFFER, vbo_);
            glBufferData(GL_ARRAY_BUFFER, sizeof(triangle_vertices), triangle_vertices, GL_DYNAMIC_DRAW);
            
            GLint colorLoc = u_color_loc_;
            if (colorLoc >= 0) {
                glUniform4f(colorLoc, theme_->accent2.r / 255.0f, theme_->accent2.g / 255.0f, 
                           theme_->accent2.b / 255.0f, (theme_->accent2.a / 255.0f) * ui_alpha_ * text_alpha);
            }
            GLint useTextureLoc = u_use_texture_loc_;
            if (useTextureLoc >= 0) {
                glUniform1i(useTextureLoc, 0);  // No texture, solid color
            }
            
            glBindVertexArray(vao_);
            UI_DRAW_ARRAYS(GL_TRIANGLES, 0, 3);  // Draw as triangle
            glBindVertexArray(0);
        }
        
        y += static_cast<float>(theme_->playlist_item_height);
    }
    
    // If showing down arrow, draw it after the list items
    if (show_down_arrow) {
        float arrow_y = y + arrow_margin / 2.0f;
        
        // Draw triangle pointing DOWN (arrow_center_x already defined above)
        float triangle_vertices[] = {
            arrow_center_x - arrow_size, arrow_y,            0.0f, 0.0f,  // Top-left
            arrow_center_x + arrow_size, arrow_y,            1.0f, 0.0f,  // Top-right
            arrow_center_x, arrow_y + arrow_size * 1.2f,     0.5f, 1.0f   // Bottom (pointing down)
        };
        
        flush_ui_batch();
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(triangle_vertices), triangle_vertices, GL_DYNAMIC_DRAW);
        
        GLint colorLoc = u_color_loc_;
        if (colorLoc >= 0) {
            glUniform4f(colorLoc, theme_->accent2.r / 255.0f, theme_->accent2.g / 255.0f, 
                       theme_->accent2.b / 255.0f, (theme_->accent2.a / 255.0f) * ui_alpha_ * text_alpha);
        }
        GLint useTextureLoc = u_use_texture_loc_;
        if (useTextureLoc >= 0) {
            glUniform1i(useTextureLoc, 0);
        }
        
        glBindVertexArray(vao_);
        UI_DRAW_ARRAYS(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
    }
}

void Renderer::render_error_overlay(const app::AppState& state) {
    if (!state.has_error_message()) return;

    // Padding audit 2026-08-09: the banner was a fixed 60%-width box with
    // the text merely centered over it — a message wider than the box ran
    // past both borders. The banner now GROWS to the measured text plus
    // the shared banner insets (clamped to the canvas edge margin), and
    // anything still wider is ellipsis-truncated to the interior.
    namespace ov = ui::overlay;
    std::string msg = state.error_message;
    const int font_size = 18;

    const float max_banner_w =
        static_cast<float>(width_) - 2.0f * ov::kScreenEdgeMargin;
    float text_w = static_cast<float>(
        body_font_manager_->get_text_width(msg, font_size));
    float banner_w = std::max(static_cast<float>(width_) * 0.6f,
                              text_w + 2.0f * ov::kBannerPadX);
    banner_w = std::min(banner_w, max_banner_w);

    const float max_text_w = banner_w - 2.0f * ov::kBannerPadX;
    if (text_w > max_text_w) {
        while (!msg.empty() &&
               body_font_manager_->get_text_width(msg + "...", font_size) >
                   max_text_w) {
            ::ui::utf8_pop_back(msg);
        }
        if (!msg.empty()) msg += "...";
        text_w = static_cast<float>(
            body_font_manager_->get_text_width(msg, font_size));
    }

    const float banner_h = static_cast<float>(font_size) + 2.0f * ov::kBannerPadY;
    const float banner_y = static_cast<float>(height_) - banner_h - 20.0f;
    const float banner_x = (static_cast<float>(width_) - banner_w) / 2.0f;

    // Semi-transparent dark background
    ui::Color bg = {0, 0, 0, 180};
    draw_quad(banner_x, banner_y, banner_w, banner_h, bg);

    // Error text in red, centered on real font metrics (the old
    // font_size*0.3 baseline guess sat the line visibly high).
    const int baseline = body_font_manager_->get_baseline_at_size(font_size);
    float text_x = banner_x + (banner_w - text_w) / 2.0f;
    float text_y = banner_y
                 + (banner_h - static_cast<float>(font_size)) / 2.0f
                 + static_cast<float>(baseline);
    draw_text(msg, text_x, text_y, font_size, {255, 100, 100, 255});
}

void Renderer::render_loading_overlay(const app::AppState& state) {
    if (!state.is_loading_game) return;

    // Return-dissolve alpha. 1.0 for the whole launch side; ramped to 0 by
    // the game-exit path. The caller clears to black first, so alpha 0 is
    // pure black — skip the draws entirely.
    const float a = std::min(1.0f, std::max(0.0f, state.loading_alpha.load()));
    if (a <= 0.004f) return;

    // DESIGNED TO LOOK CORRECT WHEN IT STOPS UPDATING.
    //
    // The kiosk can only draw until it hands DRM master to RetroArch; the
    // frame presented at that moment stays on the panel for ~2.5s while
    // RetroArch initialises, and nothing can update it. Measured on hardware:
    // of a 4.3s launch only ~0.4s was animatable, 3.9s was one frozen frame.
    //
    // The previous design was a rotating square plus a sine-pulsed text alpha
    // — two purely time-driven cues, so the freeze left a spinner stopped at a
    // random angle and text stuck at a random brightness. A stopped spinner
    // reads as "hung"; that is the worst possible thing to leave on screen.
    //
    // So: no spinner, no pulse. A titled plate with a phase-stepped bar, which
    // the launch path drives to FULL immediately before handing over the
    // display. Every pixel is meaningful at rest, and the full bar is honest —
    // everything the kiosk controls really has finished by then.

    const float w = static_cast<float>(width_);
    const float h = static_cast<float>(height_);

    draw_quad(0.0f, 0.0f, w, h, ui::Color(0, 0, 0, 220), a);
    if (!body_font_manager_) return;

    // Panel proportions follow the 720p logical canvas the rest of the UI is
    // authored against, so this scales with the display mode like everything
    // else instead of being pinned to pixels.
    const float panel_w = w * 0.62f;
    const float panel_h = h * 0.34f;
    const float panel_x = (w - panel_w) / 2.0f;
    const float panel_y = (h - panel_h) / 2.0f;

    const ui::Color frame_color = theme_->accent;

    draw_quad(panel_x, panel_y, panel_w, panel_h, theme_->bg_lift, a);
    // Hairline frame, drawn as four thin quads (no line primitive here).
    const float edge = std::max(2.0f, h * 0.004f);
    draw_quad(panel_x, panel_y, panel_w, edge, frame_color, a);
    draw_quad(panel_x, panel_y + panel_h - edge, panel_w, edge, frame_color, a);
    draw_quad(panel_x, panel_y, edge, panel_h, frame_color, a);
    draw_quad(panel_x + panel_w - edge, panel_y, edge, panel_h, frame_color, a);

    const float inner_x = panel_x + panel_w * 0.06f;
    const float inner_w = panel_w * 0.88f;

    // --- Eyebrow ----------------------------------------------------------
    // The plate's wording and colour are fixed: it is drawn only for the
    // launch. On the way back the exit path dissolves this exact frame to
    // black (loading_alpha) rather than replacing it with a return variant,
    // so the frozen handover frame and the transition are one and the same.
    const int label_size = theme_->font_small_size;
    const std::string label = "NOW LOADING";
    draw_text(label, inner_x, panel_y + panel_h * 0.20f, label_size,
              theme_->dim, false, a);

    // --- Game title, truncated to the panel ------------------------------
    const int title_size = theme_->font_large_size;
    std::string title = state.loading_title.empty() ? "Loading" : state.loading_title;
    while (!title.empty() &&
           body_font_manager_->get_text_width(title + "...", title_size) > inner_w) {
        ::ui::utf8_pop_back(title);
    }
    if (title != state.loading_title && !title.empty()) title += "...";
    draw_text(title, inner_x, panel_y + panel_h * 0.46f, title_size,
              theme_->fg, false, a);

    // --- System name -----------------------------------------------------
    if (!state.loading_system.empty()) {
        draw_text(state.loading_system, inner_x, panel_y + panel_h * 0.62f,
                  theme_->font_small_size, theme_->dim, false, a);
    }

    // --- Chunky segmented progress bar -----------------------------------
    // Discrete blocks rather than a smooth fill: a segmented bar is the
    // cartridge-era idiom, and it also makes a stopped bar look like a
    // finished count rather than an interrupted animation.
    const float bar_h = std::max(10.0f, panel_h * 0.10f);
    // 0.72 (was 0.74): the padding audit found the phase line below the
    // bar rendering ~3 px off the plate's bottom border on both canvases
    // (0.74 + 0.10 bar + 0.13 gap puts the baseline at 0.97·panel_h).
    // Nudging the bar up plus clamping the phase baseline below restores
    // a real bottom margin without touching the rows above.
    const float bar_y = panel_y + panel_h * 0.72f;
    draw_quad(inner_x, bar_y, inner_w, bar_h, theme_->bg, a);

    const int kSegments = 20;
    const float gap = std::max(2.0f, inner_w * 0.004f);
    const float seg_w = (inner_w - gap * (kSegments - 1)) / kSegments;
    float progress = state.loading_progress.load();
    progress = std::min(1.0f, std::max(0.0f, progress));
    const int filled = static_cast<int>(progress * kSegments + 0.5f);
    const ui::Color bar_color = frame_color;
    for (int i = 0; i < filled; ++i) {
        draw_quad(inner_x + i * (seg_w + gap), bar_y, seg_w, bar_h, bar_color, a);
    }

    // --- Phase line ------------------------------------------------------
    if (!state.loading_phase.empty()) {
        // Clamp the baseline so the text (baseline + ~4 px descent) keeps
        // overlay::kMinTextInset from the plate's bottom border — the
        // proportional 0.13 gap alone parked the descenders ~3 px off the
        // border at every canvas height.
        const float phase_baseline = std::min(
            bar_y + bar_h + panel_h * 0.13f,
            panel_y + panel_h - (ui::overlay::kMinTextInset + 4.0f));
        draw_text(state.loading_phase, inner_x, phase_baseline,
                  theme_->font_small_size, theme_->dim, false, a);
    }
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
                const double footer_duration = state.get_duration();
                if (footer_duration > 0) {
                    const double footer_position = state.get_position();
                    std::string progress_text = format_time(footer_position) + " / " + format_time(footer_duration);
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
    const double time_position = state.get_position();
    const double time_duration = state.get_duration();
    if (time_position > 0 || time_duration > 0) {
        std::string time_text = format_time(time_position);
        if (time_duration > 0) {
            time_text += " / " + format_time(time_duration);
        }
        int time_width = body_font_manager_->get_text_width(time_text);
        float time_x = static_cast<float>(width_) - time_width - 80.0f;
        float time_baseline = footer_baseline + 18.0f;
        draw_text(time_text, time_x, time_baseline, theme_->font_small_size, theme_->fg, false, text_alpha);
    }
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
    
    // Label — top margin balanced against the bar's bottom margin
    // (padding audit: the old fixed baseline of y+25 left ~11 px above
    // the text vs 20 px under the bar). Content block = label + 12 px
    // gap + 10 px bar = 40 px, so 20 px insets top and bottom.
    std::string label = "MASTER VOLUME: " + std::to_string(state.master_volume) + "%";
    int font_size = theme_->font_medium_size;
    int label_width = body_font_manager_->get_text_width(label, font_size);
    float label_x = x + (overlay_width - label_width) / 2.0f;
    float label_y = y + 20.0f
                  + static_cast<float>(body_font_manager_->get_baseline_at_size(font_size));
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

void Renderer::render_seek_bar(const app::AppState& state) {
    // Gate on show_seek_bar only. video_active was previously checked here
    // as a defensive guard, but show_seek_bar is only flipped true by seek
    // input handlers that run during active playback in the first place,
    // so the extra gate was redundant — and it broke the Media Browser's
    // PlaybackScreen path where state.video_active can briefly drop during
    // a FLUSH seek transition while the user is actively scrubbing.
    if (!state.show_seek_bar) return;

    // Fade behavior: full opacity when timer > 0.5, linear fade 0.5 -> 0.0
    float alpha = 1.0f;
    if (state.seek_bar_timer <= 0.5) {
        alpha = static_cast<float>(state.seek_bar_timer / 0.5);
    }
    if (alpha <= 0.0f) return;

    double position = state.get_position();
    double duration = state.get_duration();
    if (duration <= 0.0) return;

    float progress = static_cast<float>(position / duration);
    progress = std::max(0.0f, std::min(1.0f, progress));

    // Layout: centered, 80% screen width.
    // Bar sits at ~110px from screen bottom so the chrome footer hints
    // (rendered ~40px above the bezel bottom = screen_h - 40 - 30 ≈ -70px
    // from bottom) have clear breathing room below the scrub bar.
    float bar_total_width = static_cast<float>(width_) * 0.8f;
    float bar_x = (static_cast<float>(width_) - bar_total_width) / 2.0f;
    float bar_y = static_cast<float>(height_) - 110.0f;
    float bar_height = 4.0f;

    // Track background (dim gray)
    ui::Color track_color = theme_->dim;
    track_color.a = static_cast<uint8_t>(track_color.a * alpha);
    draw_quad(bar_x, bar_y, bar_total_width, bar_height, track_color);

    // Progress fill (accent/gold)
    float fill_width = bar_total_width * progress;
    if (fill_width > 0.0f) {
        ui::Color fill_color = theme_->accent;
        fill_color.a = static_cast<uint8_t>(fill_color.a * alpha);
        draw_quad(bar_x, bar_y, fill_width, bar_height, fill_color);
    }

    // Playhead indicator (bright square at current position)
    float head_size = 10.0f;
    float head_x = bar_x + fill_width - head_size / 2.0f;
    float head_y = bar_y - (head_size - bar_height) / 2.0f;
    ui::Color head_color = theme_->fg;
    head_color.a = static_cast<uint8_t>(head_color.a * alpha);
    draw_quad(head_x, head_y, head_size, head_size, head_color);

    // Time labels above the bar
    std::string time_current = format_time(position);
    std::string time_total = format_time(duration);
    int font_size = theme_->font_small_size;
    float label_y = bar_y - 8.0f;  // Just above the bar

    // Current time (left-aligned)
    ui::Color text_color = theme_->fg;
    text_color.a = static_cast<uint8_t>(text_color.a * alpha);
    draw_text(time_current, bar_x, label_y, font_size, text_color, false, alpha);

    // Total duration (right-aligned)
    int total_width = body_font_manager_->get_text_width(time_total, font_size);
    float total_x = bar_x + bar_total_width - static_cast<float>(total_width);
    draw_text(time_total, total_x, label_y, font_size, text_color, false, alpha);
}

} // namespace ui
