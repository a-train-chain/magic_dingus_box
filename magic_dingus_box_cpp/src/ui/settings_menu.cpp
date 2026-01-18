#include "settings_menu.h"
#include "virtual_keyboard.h"
#include "../utils/wifi_manager.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>  // For std::max
#include <iostream>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/types.h>
#include <arpa/inet.h>

#include "../app/app_state.h"
#include "../app/settings_persistence.h"

namespace ui {

SettingsMenuManager::SettingsMenuManager(app::AppState* state)
    : app_state_(state)
    , active_(false)
    , selected_index_(0)
    , animation_duration_(0.3f)
    , is_opening_(false)
    , is_closing_(false)
    , scroll_offset_(0)
    , current_submenu_(MenuSection::BACK)
    , game_browser_active_(false)
    , game_browser_selected_(0)
    , viewing_games_in_playlist_(false)
    , current_game_playlist_index_(0)
    , selected_game_in_playlist_(0)
    , was_scanning_(false)
    , was_connecting_(false)
{
    // Main menu items
    menu_items_ = {
        MenuItem("Video Games", MenuSection::VIDEO_GAMES, "Emulated games"),
        MenuItem("Display", MenuSection::DISPLAY, "Screen settings"),
        MenuItem("Audio", MenuSection::AUDIO, "Volume"),
        MenuItem("Wi-Fi", MenuSection::WIFI, "Network Setup"),
        MenuItem("System", MenuSection::SYSTEM, "Settings"),
        MenuItem("Content Manager", MenuSection::INFO, "Web UI"),
        MenuItem("Back", MenuSection::BACK)
    };
}

void SettingsMenuManager::update() {
    if (!active_ && !is_opening_ && !is_closing_) return;

    // Check Wi-Fi scanning state if we are in the networks submenu
    if (current_submenu_ == MenuSection::WIFI_NETWORKS) {
        bool is_scanning = utils::WifiManager::instance().is_scanning();
        
        // If we were scanning and now we stopped, we need to refresh to show results
        if (was_scanning_ && !is_scanning) {
            std::cout << "SettingsMenuManager: Scan finished, rebuilding submenu..." << std::endl;
            rebuild_current_submenu();
        }
        
        was_scanning_ = is_scanning;
    }

    // Check for connection completion in both WIFI and WIFI_NETWORKS submenus
    if (current_submenu_ == MenuSection::WIFI || current_submenu_ == MenuSection::WIFI_NETWORKS) {
        bool is_connecting = utils::WifiManager::instance().is_connecting();
        if (was_connecting_ && !is_connecting) {
            std::cout << "SettingsMenuManager: Connection finished, refreshing menu..." << std::endl;
            // Connection finished, refresh to show updated status
            rebuild_current_submenu();
        }
        was_connecting_ = is_connecting;
    }
}

std::vector<MenuItem> SettingsMenuManager::build_display_submenu() {
    if (!app_state_) {
        return {
            MenuItem("Error: No AppState", MenuSection::BACK),
            MenuItem("Back", MenuSection::BACK)
        };
    }

    auto& settings = app_state_->display_settings;
    
    // Get current bezel name
    std::string bezel_name = "None";
    if (!app_state_->available_bezels.empty() && 
        settings.bezel_index >= 0 && 
        settings.bezel_index < static_cast<int>(app_state_->available_bezels.size())) {
        bezel_name = app_state_->available_bezels[settings.bezel_index].name;
    }

    std::vector<MenuItem> items = {
        MenuItem("Mode: " + settings.get_mode_name(), MenuSection::TOGGLE_DISPLAY_MODE, "Cycle modes",
                 [&]() { 
                     settings.cycle_mode(); 
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 }),
    };
    
    // Show bezel option only in Modern TV mode
    if (settings.mode == app::DisplayMode::MODERN_TV) {
        items.emplace_back("Bezel: " + bezel_name, MenuSection::CYCLE_BEZEL, "Overlay frame",
                 [&]() {
                     // Cycle to next bezel
                     if (!app_state_->available_bezels.empty()) {
                         settings.bezel_index = (settings.bezel_index + 1) % 
                                                static_cast<int>(app_state_->available_bezels.size());
                     }
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 });
    }
    
    // CRT effect settings (show for both modes, but mostly useful for CRT Native)
    items.insert(items.end(), {
        MenuItem("Scanlines: " + intensity_to_label(settings.scanline_intensity), 
                 MenuSection::CYCLE_SCANLINES, "CRT lines", 
                 [&]() { 
                     settings.cycle_setting(settings.scanline_intensity); 
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 }),
                 
        MenuItem("Color Warmth: " + intensity_to_label(settings.warmth_intensity), 
                 MenuSection::CYCLE_WARMTH, "Temperature",
                 [&]() { 
                     settings.cycle_setting(settings.warmth_intensity); 
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 }),
                 
        MenuItem("Phosphor Glow: " + intensity_to_label(settings.glow_intensity), 
                 MenuSection::CYCLE_GLOW, "Radial glow",
                 [&]() { 
                     settings.cycle_setting(settings.glow_intensity); 
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 }),
                 
        MenuItem("RGB Mask: " + intensity_to_label(settings.rgb_mask_intensity), 
                 MenuSection::CYCLE_PHOSPHOR_MASK, "RGB stripes",
                 [&]() { 
                     settings.cycle_setting(settings.rgb_mask_intensity); 
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 }),
                 
        MenuItem("Screen Bloom: " + intensity_to_label(settings.bloom_intensity), 
                 MenuSection::CYCLE_BLOOM, "Bright glow",
                 [&]() { 
                     settings.cycle_setting(settings.bloom_intensity); 
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 }),
                 
        MenuItem("Interlacing: " + intensity_to_label(settings.interlacing_intensity), 
                 MenuSection::CYCLE_INTERLACING, "Video lines",
                 [&]() { 
                     settings.cycle_setting(settings.interlacing_intensity); 
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 }),
                 
        MenuItem("Flicker: " + intensity_to_label(settings.flicker_intensity), 
                 MenuSection::CYCLE_FLICKER, "Subtle pulse",
                 [&]() { 
                     settings.cycle_setting(settings.flicker_intensity); 
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 }),
                 
        MenuItem("Back", MenuSection::BACK)
    });
    
    return items;
}

void SettingsMenuManager::toggle() {
    if (active_ || is_opening_) {
        close();
    } else {
        open();
    }
}

void SettingsMenuManager::open() {
    if (!active_ && !is_opening_) {
        active_ = true;
        is_opening_ = true;
        is_closing_ = false;
        animation_start_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(50);
        selected_index_ = 0;
        scroll_offset_ = 0;
        current_submenu_ = MenuSection::BACK;
    }
}

void SettingsMenuManager::close() {
    if (active_ || is_opening_) {
        is_closing_ = true;
        is_opening_ = false;
        animation_start_ = std::chrono::steady_clock::now();
    }
}

float SettingsMenuManager::get_animation_progress() const {
    if (!active_ && !is_closing_) {
        return 0.0f;
    }
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<float>(now - animation_start_).count();
    
    if (is_closing_) {
        float progress = 1.0f - (elapsed / animation_duration_);
        if (progress <= 0.0f) {
            const_cast<SettingsMenuManager*>(this)->active_ = false;
            const_cast<SettingsMenuManager*>(this)->is_closing_ = false;
            return 0.0f;
        }
        return progress;
    } else {
        float progress = elapsed / animation_duration_;
        if (progress >= 1.0f) {
            const_cast<SettingsMenuManager*>(this)->is_opening_ = false;
            return 1.0f;
        }
        return progress;
    }
}

void SettingsMenuManager::navigate(int delta, int game_playlists_count, int games_in_current_playlist) {
    if (!active_) return;
    
    // Handle game browser navigation
    if (game_browser_active_) {
        if (viewing_games_in_playlist_) {
            // Navigating games within a playlist
            selected_game_in_playlist_ += delta;
            if (selected_game_in_playlist_ < 0) {
                selected_game_in_playlist_ = 0;
            } else if (selected_game_in_playlist_ > games_in_current_playlist) {
                selected_game_in_playlist_ = games_in_current_playlist;
            }
        } else {
            // Navigating game playlists
            game_browser_selected_ += delta;
            if (game_browser_selected_ < 0) {
                game_browser_selected_ = 0;
            } else if (game_browser_selected_ > game_playlists_count) {
                game_browser_selected_ = game_playlists_count;
            }
        }
        return;
    }
    
    // Normal menu navigation
    const std::vector<MenuItem>& items = current_submenu_ == MenuSection::BACK ? menu_items_ : submenu_items_;
    int max_items = static_cast<int>(items.size());
    
    if (max_items == 0) {
        return;
    }
    
    selected_index_ += delta;
    if (selected_index_ < 0) {
        selected_index_ = 0;
    } else if (selected_index_ >= max_items) {
        selected_index_ = max_items - 1;
    }
    
    // Scrolling logic
    const int max_visible_items = 7;
    if (selected_index_ < scroll_offset_) {
        scroll_offset_ = selected_index_;
    } else if (selected_index_ >= scroll_offset_ + max_visible_items) {
        scroll_offset_ = selected_index_ - max_visible_items + 1;
    }
    
    // Ensure scroll_offset is valid
    if (scroll_offset_ < 0) {
        scroll_offset_ = 0;
    }
    if (scroll_offset_ >= max_items) {
        scroll_offset_ = std::max(0, max_items - 1);
    }
}

MenuSection SettingsMenuManager::select_current() {
    if (!active_) {
        return MenuSection::BACK;
    }
    
    const std::vector<MenuItem>& items = current_submenu_ == MenuSection::BACK ? menu_items_ : submenu_items_;
    if (selected_index_ >= 0 && selected_index_ < static_cast<int>(items.size())) {
        const MenuItem& item = items[selected_index_];
        if (item.action) {
            item.action();
        }
        return item.section;
    }
    return MenuSection::BACK;
}

void SettingsMenuManager::enter_submenu(MenuSection section) {
    current_submenu_ = section;
    selected_index_ = 0;
    scroll_offset_ = 0;
    
    if (section == MenuSection::VIDEO_GAMES) {
        submenu_items_ = build_games_submenu();
    } else if (section == MenuSection::DISPLAY) {
        submenu_items_ = build_display_submenu();
    } else if (section == MenuSection::AUDIO) {
        submenu_items_ = build_audio_submenu();
    } else if (section == MenuSection::SYSTEM) {
        submenu_items_ = build_system_submenu();
    } else if (section == MenuSection::WIFI) {
        submenu_items_ = build_wifi_submenu();
    } else if (section == MenuSection::WIFI_NETWORKS) {
        submenu_items_ = build_wifi_networks_submenu();
    } else if (section == MenuSection::INFO) {
        submenu_items_ = build_info_submenu();
    }
}

void SettingsMenuManager::exit_submenu() {
    current_submenu_ = MenuSection::BACK;
    selected_index_ = 0;
    scroll_offset_ = 0;
    submenu_items_.clear();
}

void SettingsMenuManager::rebuild_current_submenu() {
    if (current_submenu_ == MenuSection::BACK) {
        return;
    }
    
    int old_index = selected_index_;
    enter_submenu(current_submenu_);
    selected_index_ = std::min(old_index, static_cast<int>(submenu_items_.size() - 1));
}

void SettingsMenuManager::enter_game_browser() {
    game_browser_active_ = true;
    game_browser_selected_ = 0;
}

void SettingsMenuManager::exit_game_browser() {
    game_browser_active_ = false;
    game_browser_selected_ = 0;
    viewing_games_in_playlist_ = false;
    current_game_playlist_index_ = 0;
    selected_game_in_playlist_ = 0;
}

void SettingsMenuManager::enter_game_list(int playlist_index) {
    viewing_games_in_playlist_ = true;
    current_game_playlist_index_ = playlist_index;
    selected_game_in_playlist_ = 0;
}

void SettingsMenuManager::exit_game_list() {
    viewing_games_in_playlist_ = false;
    selected_game_in_playlist_ = 0;
}

std::vector<MenuItem> SettingsMenuManager::build_games_submenu() {
    return {
        MenuItem("Browse Games", MenuSection::BROWSE_GAMES, "Game libraries"),
        MenuItem("Controllers", MenuSection::BACK, "Button map"),
        MenuItem("Back", MenuSection::BACK)
    };
}

std::vector<MenuItem> SettingsMenuManager::build_audio_submenu() {
    if (!app_state_) {
        return {
            MenuItem("Error: No AppState", MenuSection::BACK),
            MenuItem("Back", MenuSection::BACK)
        };
    }
    
    std::string output_label = "Audio Output: " + app_state_->audio_settings.get_output_name();
    std::string volume_label = "Game Volume: " + app_state_->audio_settings.get_volume_offset_label();
    
    return {
        MenuItem(output_label, MenuSection::TOGGLE_PLAYLIST_LOOP, "HDMI / Headphone",
            [&]() {
                app_state_->audio_settings.cycle_output();
                rebuild_current_submenu();
                app::SettingsPersistence::save_settings(*app_state_);
            }),
        MenuItem(volume_label, MenuSection::TOGGLE_SHUFFLE, "RetroArch games",
            [&]() {
                app_state_->audio_settings.cycle_volume_offset();
                rebuild_current_submenu();
                app::SettingsPersistence::save_settings(*app_state_);
            }),
        MenuItem("Back", MenuSection::BACK)
    };
}

std::vector<MenuItem> SettingsMenuManager::build_system_submenu() {
    if (!app_state_) {
        return {
            MenuItem("Error: No AppState", MenuSection::BACK),
            MenuItem("Back", MenuSection::BACK)
        };
    }

    std::string loop_status = app_state_->playlist_loop ? "ON" : "OFF";
    std::string shuffle_status = app_state_->shuffle ? "ON" : "OFF";
    
    return {
        MenuItem("Playlist Loop: " + loop_status, 
                 MenuSection::TOGGLE_PLAYLIST_LOOP, "Auto-restart",
                 [&]() { 
                     app_state_->playlist_loop = !app_state_->playlist_loop; 
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 }),
        MenuItem("Shuffle: " + shuffle_status, 
                 MenuSection::TOGGLE_SHUFFLE, "Random order",
                 [&]() { 
                     app_state_->shuffle = !app_state_->shuffle; 
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 }),
        MenuItem("Back", MenuSection::BACK)
    };
}

// Helper to check interface status
static bool is_interface_active(const char* iface_name) {
    struct ifaddrs *ifap, *ifa;
    if (getifaddrs(&ifap) == -1) return false;

    bool active = false;
    for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) { // IPv4
            if (std::string(ifa->ifa_name) == iface_name) {
                active = true;
                break;
            }
        }
    }
    freeifaddrs(ifap);
    return active;
}

std::vector<MenuItem> SettingsMenuManager::build_info_submenu() {
    auto& wifi = utils::WifiManager::instance();
    
    bool usb_active = is_interface_active("usb0");
    bool wifi_active = wifi.is_connected();

    // Determine primary connection (USB priority)
    std::string primary_url = "";
    std::string connection_label = "Not Connected";
    std::string sub_label = "Connect via USB or Wi-Fi";
    
    if (usb_active) {
        primary_url = "http://192.168.7.1:5000";
        connection_label = "USB Connection (Active)";
        sub_label = "Fastest / Recommended";
    } else if (wifi_active) {
        std::string ip = wifi.get_ip_address();
        if (!ip.empty()) {
            primary_url = "http://" + ip + ":5000";
            connection_label = "Wi-Fi Connection";
            sub_label = wifi.get_current_ssid();
        }
    }
    
    // Update app state
    if (app_state_) {
        app_state_->usb_url = "http://192.168.7.1:5000"; // Always static
        app_state_->wifi_url = wifi_active ? ("http://" + wifi.get_ip_address() + ":5000") : "";
        app_state_->content_manager_url = primary_url;
    }
    
    // Simplified Menu - just show the connection status
    std::vector<MenuItem> items;
    
    if (!primary_url.empty()) {
        items.emplace_back(connection_label, MenuSection::INFO, sub_label, 
            [this, primary_url]() {
                if (app_state_) app_state_->content_manager_url = primary_url;
            });
    } else {
         items.emplace_back("No Connection", MenuSection::BACK, "Plug in USB or setup Wi-Fi");
    }

    items.emplace_back("Back", MenuSection::BACK);
    return items;
}

std::string SettingsMenuManager::intensity_to_label(float intensity) {
    if (intensity <= 0.0f) {
        return "OFF";
    } else if (intensity <= 0.35f) {
        return "Low (" + std::to_string(static_cast<int>(intensity * 100)) + "%)";
    } else if (intensity <= 0.6f) {
        return "Medium (" + std::to_string(static_cast<int>(intensity * 100)) + "%)";
    } else {
        return "High (" + std::to_string(static_cast<int>(intensity * 100)) + "%)";
    }
}

bool SettingsMenuManager::is_game_browser_back_selected() const {
    if (!game_browser_active_) return false;
    return false; 
}

std::vector<MenuItem> SettingsMenuManager::build_wifi_submenu() {
    auto& wifi = utils::WifiManager::instance();
    std::string status = wifi.is_connected() ? "Connected: " + wifi.get_current_ssid() : "Not Connected";
    std::string ip = wifi.is_connected() ? wifi.get_ip_address() : "";
    
    return {
        MenuItem("Status: " + status, MenuSection::BACK, ip),
        MenuItem("Scan Networks", MenuSection::WIFI_NETWORKS, "Find Wi-Fi",
                 [&]() {
                     utils::WifiManager::instance().scan_networks_async();
                 }),
        MenuItem("Disconnect", MenuSection::WIFI, "Forget current",
                 [this]() {
                     utils::WifiManager::instance().forget_network(utils::WifiManager::instance().get_current_ssid());
                     rebuild_current_submenu(); // Refresh to show updated status
                 }),
        MenuItem("Back", MenuSection::BACK)
    };
}

std::vector<MenuItem> SettingsMenuManager::build_wifi_networks_submenu() {
    auto& wifi = utils::WifiManager::instance();
    
    if (wifi.is_connecting()) {
         return {
            MenuItem("Connecting...", MenuSection::BACK, "Verifying credentials..."),
            MenuItem("Back", MenuSection::BACK) // Allow backing out (though cancellation isn't implemented)
        };
    }

    // Check previous result
    utils::ConnectionResult result = wifi.get_connection_result();
    if (result == utils::ConnectionResult::FAILURE) {
        wifi.reset_connection_state(); // Clear it so we don't show it forever
        return {
            MenuItem("Connection Failed", MenuSection::BACK, "Check password/signal"),
            MenuItem("Back", MenuSection::BACK, "Return to list", 
                []() { utils::WifiManager::instance().scan_networks_async(); }) 
        };
    }

    if (wifi.is_scanning()) {
        utils::WifiManager::instance().scan_networks_async(); // Ensure it's running
        return {
            MenuItem("Scanning...", MenuSection::BACK, "Please wait"),
            MenuItem("Back", MenuSection::BACK)
        };
    }
    
    auto results = wifi.get_scan_results();
    std::vector<MenuItem> items;
    
    for (const auto& net : results) {
        std::string label = net.ssid;
        if (net.in_use) label += " (Connected)";
        else if (net.saved) label += " (Saved)";
        
        std::string sub = "Signal: " + std::to_string(net.signal_strength) + "% " + net.security;
        
        // Use WIFI to return to Wi-Fi menu after connection attempt
        items.emplace_back(label, MenuSection::WIFI, sub, 
            [this, net]() {
                // Open Keyboard
                if (app_state_ && app_state_->keyboard) {
                    app_state_->keyboard->open("", "Enter Password for " + net.ssid, 
                        [this, net](const std::string& password) {
                            utils::WifiManager::instance().connect_async(net.ssid, password);
                            // Connection result will be shown via update() -> rebuild_current_submenu()
                        },
                        [this]() { 
                            // Cancel - stay in Wi-Fi menu
                            enter_submenu(MenuSection::WIFI);
                        }
                    );
                }
            });
    }
    
    if (items.empty()) {
        items.emplace_back("No networks found", MenuSection::BACK);
    }
    
    items.emplace_back("Rescan", MenuSection::WIFI, "", [&](){
         utils::WifiManager::instance().scan_networks_async();
         rebuild_current_submenu(); 
    });
    
    items.emplace_back("Back", MenuSection::WIFI);
    return items;
}

} // namespace ui
