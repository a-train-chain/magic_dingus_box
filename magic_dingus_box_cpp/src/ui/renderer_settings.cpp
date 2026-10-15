// ui::Renderer — The Settings menu, the game browser (thumbnails, system logos) and
// QR codes.
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
    // Compute visible row count from the actual viewport height.
    // Was hardcoded to 7 (sized for CRT 640x480). In Modern TV mode
    // the menu's 4:3 viewport is taller (720), so more items fit.
    // Floor at 7 to preserve CRT's established layout.
    //
    // bottom_reserve = 30: just a small breathing-room margin from the
    // bottom of the screen. We don't need to clear the controls-banner
    // footer because the settings menu draws its own opaque bg quad
    // over the right half of the screen, masking the banner there.
    // With this value, Modern TV (720) fits 10 rows — enough to show
    // every item in the Display submenu (Mode + Bezel + 7 CRT effects
    // + Back) without scrolling. CRT (480) stays floored at 7.
    const float settings_bottom_reserve = 30.0f;
    const float settings_available_h =
        static_cast<float>(height_) - start_y - settings_bottom_reserve;
    int max_visible = std::max(
        7, static_cast<int>(settings_available_h / static_cast<float>(item_height)));
    // Publish back to the menu so move_selection() knows when it
    // actually needs to scroll. Without this, the input handler used
    // a hardcoded 7 and forced scroll_offset to 1 on row index 7+,
    // chopping the top item off-screen even when all items fit.
    menu->set_max_visible_items(max_visible);
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
            
            flush_ui_batch();
            glBindBuffer(GL_ARRAY_BUFFER, vbo_);
            glBufferData(GL_ARRAY_BUFFER, sizeof(triangle_vertices), triangle_vertices, GL_DYNAMIC_DRAW);
            
            GLint colorLoc = u_color_loc_;
            if (colorLoc >= 0) {
                glUniform4f(colorLoc, section_color.r / 255.0f, section_color.g / 255.0f,
                           section_color.b / 255.0f, (section_color.a / 255.0f) * ui_alpha_ * text_alpha);
            }
            GLint useTextureLoc = u_use_texture_loc_;
            if (useTextureLoc >= 0) {
                glUniform1i(useTextureLoc, 0);
            }
            
            glBindVertexArray(vao_);
            UI_DRAW_ARRAYS(GL_TRIANGLES, 0, 3);
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
    // Footer hint removed - interface is self-explanatory
}

bool Renderer::load_thumbnail(const std::string& rom_path) {
    // Derive thumbnail path from ROM path: data/roms/ps1/Game.chd -> data/thumbnails/ps1/Game.png
    // Find the system directory component
    std::string thumb_path;
    size_t roms_pos = rom_path.find("data/roms/");
    if (roms_pos != std::string::npos) {
        thumb_path = rom_path.substr(0, roms_pos) + "data/thumbnails/" + rom_path.substr(roms_pos + 10); // skip "data/roms/"
    } else {
        // Fallback: just try replacing extension
        thumb_path = rom_path;
    }

    // Replace ROM extension (.chd, .m3u, .z64, .nes, etc.) with .png
    size_t dot_pos = thumb_path.rfind('.');
    if (dot_pos != std::string::npos) {
        thumb_path = thumb_path.substr(0, dot_pos) + ".png";
    }

    // Pump: upload a completed background decode (this function is
    // called every frame by the game-browser draw site, so this is the
    // natural render-thread drain point).
    if (thumb_done_.exchange(false, std::memory_order_acq_rel)) {
        const bool still_wanted = (thumb_result_path_ == current_thumbnail_path_);
        if (still_wanted && thumb_result_pixels_ != nullptr) {
            thumbnail_width_ = thumb_result_w_;
            thumbnail_height_ = thumb_result_h_;
            glGenTextures(1, &thumbnail_texture_id_);
            glBindTexture(GL_TEXTURE_2D, thumbnail_texture_id_);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            flush_ui_batch();
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, thumbnail_width_,
                         thumbnail_height_, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         thumb_result_pixels_);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        if (thumb_result_pixels_ != nullptr) {
            stbi_image_free(thumb_result_pixels_);
            thumb_result_pixels_ = nullptr;
        }
        // Selection moved on while we decoded — chase it.
        if (!still_wanted && !current_thumbnail_path_.empty()) {
            spawn_thumbnail_decode(current_thumbnail_path_);
        }
    }

    if (thumb_path == current_thumbnail_path_) {
        return thumbnail_texture_id_ != 0; // Loaded (or decode in flight / miss)
    }

    // Selection changed: free the old texture immediately (same visual
    // behavior as the synchronous path — the outgoing thumbnail
    // disappears at once) and hand the disk read + PNG decode to the
    // worker. The decode used to run right here on the render thread —
    // tens of ms per selection change on a Pi 4, a visible hitch while
    // scrolling the game list. The thumbnail now appears a frame or two
    // later instead of stalling the frame.
    if (thumbnail_texture_id_ != 0) {
        flush_ui_batch();
        glDeleteTextures(1, &thumbnail_texture_id_);
        thumbnail_texture_id_ = 0;
    }
    current_thumbnail_path_ = thumb_path;

    if (!thumb_in_flight_.load(std::memory_order_acquire)) {
        spawn_thumbnail_decode(thumb_path);
    }
    // else: worker busy with a stale path — the pump above respawns for
    // the current path when it finishes.

    return false;
}

void Renderer::spawn_thumbnail_decode(const std::string& thumb_path) {
    if (thumb_worker_.joinable()) thumb_worker_.join();  // finished run
    thumb_in_flight_.store(true, std::memory_order_release);
    try {
        thumb_worker_ = std::thread([this, thumb_path]() {
            // Candidate list — identical to the old synchronous search:
            // exact path, ../-relative, then progressively simpler
            // filename variants (disc/version/region suffix stripping).
            std::vector<std::string> search_paths = {
                thumb_path,
                "../" + thumb_path,
            };
            for (const auto& sp :
                 std::vector<std::string>{thumb_path, "../" + thumb_path}) {
                const std::string& base = sp;

                size_t disc_pos = base.find(" (Disc ");
                if (disc_pos != std::string::npos) {
                    search_paths.push_back(base.substr(0, disc_pos) + ".png");
                }

                size_t ver_pos = base.find(" (v");
                if (ver_pos != std::string::npos) {
                    search_paths.push_back(base.substr(0, ver_pos) + ".png");
                }

                for (const char* region :
                     {"(USA)", "(Europe)", "(Japan)", "(World)"}) {
                    size_t region_pos = base.find(region);
                    if (region_pos != std::string::npos) {
                        search_paths.push_back(
                            base.substr(0, region_pos + strlen(region)) + ".png");
                        break;
                    }
                }
            }

            int w = 0, h = 0, channels = 0;
            unsigned char* data = nullptr;
            for (const auto& path : search_paths) {
                data = stbi_load(path.c_str(), &w, &h, &channels, 4);
                if (data) break;
            }

            thumb_result_pixels_ = data;  // null = not found (texture stays 0)
            thumb_result_w_ = w;
            thumb_result_h_ = h;
            thumb_result_path_ = thumb_path;
            thumb_done_.store(true, std::memory_order_release);
            thumb_in_flight_.store(false, std::memory_order_release);
        });
    } catch (const std::system_error&) {
        thumb_in_flight_.store(false, std::memory_order_release);
    }
}

std::string Renderer::get_system_key(const app::Playlist& playlist) const {
    // Try emulator_system from the first item
    for (const auto& item : playlist.items) {
        if (!item.emulator_system.empty()) {
            std::string sys = item.emulator_system;
            // Lowercase
            for (auto& c : sys) c = tolower(c);

            if (sys == "ps1" || sys == "playstation") return "ps1";
            if (sys == "nes") return "nes";
            if (sys == "snes" || sys == "super nintendo") return "snes";
            if (sys == "genesis" || sys == "md" || sys == "mega drive") return "genesis";
            if (sys == "atari7800" || sys == "7800" || sys == "atari 7800") return "atari7800";
            if (sys == "pcengine" || sys == "tg16" || sys == "turbografx") return "pcengine";
            if (sys == "arcade" || sys == "mame" || sys == "fba") return "arcade";
            return sys; // Use as-is if no mapping found
        }
    }

    // Try emulator_core from the first item
    for (const auto& item : playlist.items) {
        if (!item.emulator_core.empty()) {
            const std::string& core = item.emulator_core;
            if (core.find("nestopia") != std::string::npos || core.find("fceumm") != std::string::npos) return "nes";
            if (core.find("snes9x") != std::string::npos || core.find("bsnes") != std::string::npos) return "snes";
            if (core.find("pcsx") != std::string::npos) return "ps1";
            if (core.find("genesis_plus") != std::string::npos || core.find("picodrive") != std::string::npos) return "genesis";
            if (core.find("prosystem") != std::string::npos) return "atari7800";
            if (core.find("mednafen_pce") != std::string::npos || core.find("supergrafx") != std::string::npos) return "pcengine";
            if (core.find("mame") != std::string::npos || core.find("fbneo") != std::string::npos || core.find("fbalpha") != std::string::npos) return "arcade";
            if (core.find("mupen64") != std::string::npos) return "n64";
        }
    }

    // Fallback: derive from playlist title
    std::string title = playlist.title;
    for (auto& c : title) c = tolower(c);

    if (title.find("playstation") != std::string::npos || title.find("ps1") != std::string::npos) return "ps1";
    if (title.find("super nintendo") != std::string::npos || title.find("snes") != std::string::npos) return "snes";
    if (title.find("nes") != std::string::npos) return "nes"; // After SNES check
    if (title.find("genesis") != std::string::npos || title.find("sega") != std::string::npos || title.find("mega drive") != std::string::npos) return "genesis";
    if (title.find("atari") != std::string::npos) return "atari7800";
    if (title.find("pc engine") != std::string::npos || title.find("turbografx") != std::string::npos) return "pcengine";
    if (title.find("arcade") != std::string::npos) return "arcade";

    return "";
}

const Renderer::CachedLogo* Renderer::get_system_logo(const std::string& system_key) {
    if (system_key.empty()) return nullptr;

    // Check cache first
    auto it = system_logo_cache_.find(system_key);
    if (it != system_logo_cache_.end()) {
        return it->second.texture_id != 0 ? &it->second : nullptr;
    }

    // Load from disk
    std::vector<std::string> search_paths = {
        "data/thumbnails/systems/" + system_key + ".png",
        "../data/thumbnails/systems/" + system_key + ".png",
    };

    CachedLogo logo;
    int channels;
    unsigned char* data = nullptr;
    for (const auto& path : search_paths) {
        data = stbi_load(path.c_str(), &logo.width, &logo.height, &channels, 4);
        if (data) break;
    }

    if (!data) {
        // Cache the miss so we don't retry every frame
        system_logo_cache_[system_key] = logo;
        return nullptr;
    }

    glGenTextures(1, &logo.texture_id);
    glBindTexture(GL_TEXTURE_2D, logo.texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    flush_ui_batch();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, logo.width, logo.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    // No glGenerateMipmap: min filter is GL_LINEAR, so the mip chain
    // could never be sampled — generating it only cost upload time.
    stbi_image_free(data);

    system_logo_cache_[system_key] = logo;
    return &system_logo_cache_[system_key];
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

    float content_start_y = underline_y + 15.0f;

    if (menu->is_viewing_games_in_playlist()) {
        // Show games in current playlist with thumbnail above the list
        int playlist_idx = menu->get_current_game_playlist_index();
        if (playlist_idx >= 0 && playlist_idx < static_cast<int>(game_playlists.size())) {
            const auto& playlist = game_playlists[playlist_idx];
            int selected_game = menu->get_selected_game_in_playlist();

            // --- Thumbnail or placeholder rendering ---
            float thumb_display_h = 160.0f; // Fixed height for thumbnail area
            float thumbnail_area_height = thumb_display_h + 10.0f; // Always reserve space

            if (selected_game >= 0 && selected_game < static_cast<int>(playlist.items.size())) {
                const auto& selected_item = playlist.items[selected_game];
                bool has_thumbnail = load_thumbnail(selected_item.path);

                if (has_thumbnail && thumbnail_texture_id_ != 0) {
                    // Scale thumbnail to fit, preserving aspect ratio
                    float max_thumb_w = static_cast<float>(menu_width) - 40.0f;
                    float scale = std::min(max_thumb_w / thumbnail_width_, thumb_display_h / thumbnail_height_);
                    float draw_w = thumbnail_width_ * scale;
                    float draw_h = thumbnail_height_ * scale;

                    // Center in the reserved area
                    float thumb_x = menu_x + (static_cast<float>(menu_width) - draw_w) / 2.0f;
                    float thumb_y = content_start_y + (thumb_display_h - draw_h) / 2.0f;

                    float vertices[] = {
                        thumb_x, thumb_y,                 0.0f, 0.0f,
                        thumb_x + draw_w, thumb_y,        1.0f, 0.0f,
                        thumb_x, thumb_y + draw_h,        0.0f, 1.0f,
                        thumb_x + draw_w, thumb_y + draw_h, 1.0f, 1.0f
                    };

                    flush_ui_batch();
                    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
                    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

                    glUniform4f(u_color_loc_,
                                1.0f, 1.0f, 1.0f, ui_alpha_ * text_alpha);
                    glUniform1i(u_use_texture_loc_, 1);

                    glBindTexture(GL_TEXTURE_2D, thumbnail_texture_id_);
                    glBindVertexArray(vao_);
                    UI_DRAW_ARRAYS(GL_TRIANGLE_STRIP, 0, 4);
                    glBindVertexArray(0);
                    glBindTexture(GL_TEXTURE_2D, 0);
                } else {
                    // Placeholder: dark box with system console logo
                    float placeholder_size = thumb_display_h * 0.8f;
                    float ph_x = menu_x + (static_cast<float>(menu_width) - placeholder_size) / 2.0f;
                    float ph_y = content_start_y + (thumb_display_h - placeholder_size) / 2.0f;

                    // Dark background
                    ui::Color ph_bg = {40, 35, 40, 180};
                    draw_quad(ph_x, ph_y, placeholder_size, placeholder_size, ph_bg, text_alpha);

                    // Border
                    ui::Color border_color = section_color;
                    border_color.a = 80;
                    draw_line(ph_x, ph_y, ph_x + placeholder_size, ph_y, 1.0f, border_color, text_alpha);
                    draw_line(ph_x, ph_y + placeholder_size, ph_x + placeholder_size, ph_y + placeholder_size, 1.0f, border_color, text_alpha);
                    draw_line(ph_x, ph_y, ph_x, ph_y + placeholder_size, 1.0f, border_color, text_alpha);
                    draw_line(ph_x + placeholder_size, ph_y, ph_x + placeholder_size, ph_y + placeholder_size, 1.0f, border_color, text_alpha);

                    // System logo centered in placeholder
                    std::string sys_key = get_system_key(playlist);
                    const CachedLogo* logo = get_system_logo(sys_key);
                    if (logo) {
                        // Scale logo to fit inside placeholder with padding
                        float logo_max_w = placeholder_size * 0.7f;
                        float logo_max_h = placeholder_size * 0.7f;
                        float logo_scale = std::min(logo_max_w / logo->width, logo_max_h / logo->height);
                        float logo_w = logo->width * logo_scale;
                        float logo_h = logo->height * logo_scale;

                        float logo_x = ph_x + (placeholder_size - logo_w) / 2.0f;
                        float logo_y = ph_y + (placeholder_size - logo_h) / 2.0f;

                        float vertices[] = {
                            logo_x, logo_y,                 0.0f, 0.0f,
                            logo_x + logo_w, logo_y,        1.0f, 0.0f,
                            logo_x, logo_y + logo_h,        0.0f, 1.0f,
                            logo_x + logo_w, logo_y + logo_h, 1.0f, 1.0f
                        };

                        flush_ui_batch();
                        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
                        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

                        // Render logo in white to match text color
                        glUniform4f(u_color_loc_,
                                    1.0f, 1.0f, 1.0f, ui_alpha_ * text_alpha * 0.5f);
                        glUniform1i(u_use_texture_loc_, 1);

                        glBindTexture(GL_TEXTURE_2D, logo->texture_id);
                        glBindVertexArray(vao_);
                        UI_DRAW_ARRAYS(GL_TRIANGLE_STRIP, 0, 4);
                        glBindVertexArray(0);
                        glBindTexture(GL_TEXTURE_2D, 0);
                    }
                }
            } else {
                // "Back" selected — show dimmed system logo placeholder
                float placeholder_size = thumb_display_h * 0.8f;
                float ph_x = menu_x + (static_cast<float>(menu_width) - placeholder_size) / 2.0f;
                float ph_y = content_start_y + (thumb_display_h - placeholder_size) / 2.0f;
                ui::Color ph_bg = {40, 35, 40, 100};
                draw_quad(ph_x, ph_y, placeholder_size, placeholder_size, ph_bg, text_alpha);

                // Show system logo dimmed
                std::string sys_key = get_system_key(playlist);
                const CachedLogo* logo = get_system_logo(sys_key);
                if (logo) {
                    float logo_max_w = placeholder_size * 0.7f;
                    float logo_max_h = placeholder_size * 0.7f;
                    float logo_scale = std::min(logo_max_w / logo->width, logo_max_h / logo->height);
                    float logo_w = logo->width * logo_scale;
                    float logo_h = logo->height * logo_scale;

                    float logo_x = ph_x + (placeholder_size - logo_w) / 2.0f;
                    float logo_y = ph_y + (placeholder_size - logo_h) / 2.0f;

                    float vertices[] = {
                        logo_x, logo_y,                 0.0f, 0.0f,
                        logo_x + logo_w, logo_y,        1.0f, 0.0f,
                        logo_x, logo_y + logo_h,        0.0f, 1.0f,
                        logo_x + logo_w, logo_y + logo_h, 1.0f, 1.0f
                    };

                    flush_ui_batch();
                    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
                    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

                    glUniform4f(u_color_loc_,
                                1.0f, 1.0f, 1.0f, ui_alpha_ * text_alpha * 0.3f);
                    glUniform1i(u_use_texture_loc_, 1);

                    glBindTexture(GL_TEXTURE_2D, logo->texture_id);
                    glBindVertexArray(vao_);
                    UI_DRAW_ARRAYS(GL_TRIANGLE_STRIP, 0, 4);
                    glBindVertexArray(0);
                    glBindTexture(GL_TEXTURE_2D, 0);
                }

                // Clear cached thumbnail
                if (thumbnail_texture_id_ != 0) {
                    flush_ui_batch();
                    glDeleteTextures(1, &thumbnail_texture_id_);
                    thumbnail_texture_id_ = 0;
                    current_thumbnail_path_.clear();
                }
            }

            // --- Game list (tighter spacing, no subtitle) ---
            int item_height = 30;
            float start_y = content_start_y + thumbnail_area_height;
            float available_height = static_cast<float>(height_) - start_y - 10.0f;
            int max_visible = std::max(3, static_cast<int>(available_height / item_height));

            int total_items = static_cast<int>(playlist.items.size()) + 1; // +1 for Back

            // Compute scroll offset from selection (resets across playlists since not static)
            int game_scroll_offset = 0;
            if (selected_game >= max_visible) {
                game_scroll_offset = selected_game - max_visible + 1;
            }
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
                    draw_quad(menu_x + 10.0f, y - 2.0f, static_cast<float>(menu_width) - 20.0f,
                             static_cast<float>(item_height), highlight, background_alpha);

                    // Selection indicator triangle
                    float indicator_x = menu_x + 15.0f;
                    float indicator_y = y + item_height / 2.0f;
                    float size = 6.0f;
                    float right_x = indicator_x + size * 1.2f;
                    float triangle_vertices[] = {
                        indicator_x, indicator_y - size,   0.0f, 0.0f,
                        indicator_x, indicator_y + size,   1.0f, 0.0f,
                        right_x, indicator_y,               1.0f, 1.0f
                    };

                    flush_ui_batch();
                    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
                    glBufferData(GL_ARRAY_BUFFER, sizeof(triangle_vertices), triangle_vertices, GL_DYNAMIC_DRAW);

                    GLint colorLoc = u_color_loc_;
                    if (colorLoc >= 0) {
                        glUniform4f(colorLoc, section_color.r / 255.0f, section_color.g / 255.0f,
                                   section_color.b / 255.0f, (section_color.a / 255.0f) * ui_alpha_ * text_alpha);
                    }
                    GLint useTextureLoc = u_use_texture_loc_;
                    if (useTextureLoc >= 0) {
                        glUniform1i(useTextureLoc, 0);
                    }

                    glBindVertexArray(vao_);
                    UI_DRAW_ARRAYS(GL_TRIANGLES, 0, 3);
                    glBindVertexArray(0);
                }

                // Game title only (no subtitle)
                int font_size = is_selected ? theme_->font_medium_size : theme_->font_small_size;
                ui::Color text_color = is_selected ? section_color : theme_->fg;
                float text_x = menu_x + 35.0f;
                int item_baseline_offset = body_font_manager_->get_baseline_at_size(font_size);
                float item_baseline = y + (item_height - font_size) / 2.0f + item_baseline_offset;

                if (idx < static_cast<int>(playlist.items.size())) {
                    draw_text(playlist.items[idx].title, text_x, item_baseline, font_size, text_color, false, text_alpha);
                } else {
                    draw_text("Back", text_x, item_baseline, font_size, text_color, false, text_alpha);
                }
            }
        }
    } else {
        // Show game playlists (keep subtitle with game count)
        int item_height = 50;
        float start_y = content_start_y + 15.0f;
        // Compute visible row count from the actual viewport height.
        // Was hardcoded to 8 (CRT 640x480). Modern TV's UI viewport is
        // ~960x720 — room for ~11 rows. Floor at 8 preserves CRT.
        const float gp_bottom_reserve = 80.0f;
        const float gp_available_h =
            static_cast<float>(height_) - start_y - gp_bottom_reserve;
        int max_visible = std::max(
            8, static_cast<int>(gp_available_h / static_cast<float>(item_height)));
        int selected_playlist = menu->get_game_browser_selected();

        int total_items = static_cast<int>(game_playlists.size()) + 1; // +1 for Back

        static int playlist_scroll_offset = 0;
        if (selected_playlist < playlist_scroll_offset) {
            playlist_scroll_offset = selected_playlist;
        } else if (selected_playlist >= playlist_scroll_offset + max_visible) {
            playlist_scroll_offset = selected_playlist - max_visible + 1;
        }
        if (playlist_scroll_offset < 0) playlist_scroll_offset = 0;
        if (playlist_scroll_offset > total_items - max_visible) playlist_scroll_offset = std::max(0, total_items - max_visible);

        // Clear thumbnail when not viewing games
        if (thumbnail_texture_id_ != 0) {
            flush_ui_batch();
            glDeleteTextures(1, &thumbnail_texture_id_);
            thumbnail_texture_id_ = 0;
            current_thumbnail_path_.clear();
        }

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

                flush_ui_batch();
                glBindBuffer(GL_ARRAY_BUFFER, vbo_);
                glBufferData(GL_ARRAY_BUFFER, sizeof(triangle_vertices), triangle_vertices, GL_DYNAMIC_DRAW);

                GLint colorLoc = u_color_loc_;
                if (colorLoc >= 0) {
                    glUniform4f(colorLoc, section_color.r / 255.0f, section_color.g / 255.0f,
                               section_color.b / 255.0f, (section_color.a / 255.0f) * ui_alpha_ * text_alpha);
                }
                GLint useTextureLoc = u_use_texture_loc_;
                if (useTextureLoc >= 0) {
                    glUniform1i(useTextureLoc, 0);
                }

                glBindVertexArray(vao_);
                UI_DRAW_ARRAYS(GL_TRIANGLES, 0, 3);
                glBindVertexArray(0);
            }

            // Render item text
            int font_size = is_selected ? theme_->font_medium_size : theme_->font_small_size;
            ui::Color text_color = is_selected ? section_color : theme_->fg;
            float text_x = menu_x + 35.0f;
            int item_baseline_offset = body_font_manager_->get_baseline_at_size(font_size);
            float item_baseline = y + item_baseline_offset;

            if (idx < static_cast<int>(game_playlists.size())) {
                draw_text(game_playlists[idx].title, text_x, item_baseline, font_size, text_color, false, text_alpha);

                // Game count as sublabel
                std::string count_text = std::to_string(game_playlists[idx].items.size()) + " games";
                float sublabel_baseline = item_baseline + font_size + 4.0f;
                draw_text(count_text, text_x, sublabel_baseline, theme_->font_small_size, theme_->dim, false, text_alpha);
            } else {
                draw_text("Back", text_x, item_baseline, font_size, text_color, false, text_alpha);
            }
        }
    }
}

void Renderer::render_qr_code(const std::string& url, float x, float y, float size, float alpha_multiplier) {
    if (url.empty()) return;

    // Rebuild the cached texture only when the payload changes (pairing
    // codes rotate every ~2 minutes; the screens calling this render at
    // 60fps). The old per-frame path re-ran the QR encoder AND issued
    // one draw_quad per black module — ~500 buffer uploads + draw calls
    // per frame for the whole time the "Connect a Device" screen was
    // open.
    if (qr_cache_tex_ == 0 || url != qr_cache_url_) {
        try {
            qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(
                url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
            int qr_size = qr.getSize();
            if (qr_size <= 0) return;

            // One RGBA pixel per module; GL_NEAREST scaling reproduces
            // the crisp square modules the per-quad path drew.
            std::vector<unsigned char> pixels(
                static_cast<size_t>(qr_size) * qr_size * 4);
            for (int row = 0; row < qr_size; row++) {
                for (int col = 0; col < qr_size; col++) {
                    const bool black = qr.getModule(col, row);
                    const unsigned char v = black ? 0 : 255;
                    unsigned char* px =
                        &pixels[(static_cast<size_t>(row) * qr_size + col) * 4];
                    px[0] = v; px[1] = v; px[2] = v; px[3] = 255;
                }
            }

            if (qr_cache_tex_ == 0) {
                glGenTextures(1, &qr_cache_tex_);
            }
            glBindTexture(GL_TEXTURE_2D, qr_cache_tex_);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            flush_ui_batch();
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, qr_size, qr_size, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);

            qr_cache_url_ = url;
            qr_cache_modules_ = qr_size;
        } catch (const std::exception& e) {
            std::cerr << "Failed to generate QR code: " << e.what() << std::endl;
            return;
        }
    }
    if (qr_cache_tex_ == 0 || qr_cache_modules_ <= 0) return;

    // Same geometry as the per-quad path: white background including the
    // 2-module quiet zone, QR modules over the inner [x, x+size] square.
    const float module_size = size / static_cast<float>(qr_cache_modules_);
    const int quiet_zone = 2;
    const float total_size = size + (quiet_zone * 2 * module_size);

    draw_quad(x - quiet_zone * module_size, y - quiet_zone * module_size,
              total_size, total_size,
              ui::Color(255, 255, 255, 255), alpha_multiplier);
    draw_textured_quad(qr_cache_tex_, x, y, size, size, alpha_multiplier);
}

} // namespace ui
