#include "settings_menu.h"
#include <unistd.h>
#include "pairing_screen.h"
#include "toast.h"
#include "virtual_keyboard.h"
#include "../utils/config.h"
#include "../utils/wifi_manager.h"
#include "../platform/platform_profile.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>  // For std::max
#include <fstream>
#include <iostream>
#include <string_view>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/types.h>
#include <arpa/inet.h>

#include "../app/app_state.h"
#include "../app/settings_persistence.h"

namespace ui {

// Defined further down in this namespace; declared here because
// open_pairing_screen() appears earlier in the file and needs it to
// resolve the box's real address for the pairing QR. Must be inside
// namespace ui — a global-scope declaration binds to a different symbol
// and fails at link time.
static std::string get_interface_ipv4(const char* iface_name);

SettingsMenuManager::SettingsMenuManager(app::AppState* state)
    // Init order MUST match the declaration order in the header
    // (otherwise -Wreorder fires). The compiler initializes in
    // declaration order regardless, so a mismatched init list is
    // misleading at best and a use-before-init footgun at worst.
    : app_state_(state)
    , active_(false)
    , selected_index_(0)
    , animation_duration_(0.3f)
    , is_opening_(false)
    , is_closing_(false)
    , scroll_offset_(0)
    , was_scanning_(false)
    , was_connecting_(false)
    , current_submenu_(MenuSection::BACK)
    , game_browser_active_(false)
    , game_browser_selected_(0)
    , viewing_games_in_playlist_(false)
    , current_game_playlist_index_(0)
    , selected_game_in_playlist_(0)
{
    // Main menu items
    menu_items_ = {
        MenuItem("Video Games", MenuSection::VIDEO_GAMES, "Emulated games"),
        MenuItem("Display", MenuSection::DISPLAY, "Screen settings"),
        MenuItem("Audio", MenuSection::AUDIO, "Volume"),
        MenuItem("Wi-Fi", MenuSection::WIFI, "Network Setup"),
        MenuItem("System", MenuSection::SYSTEM, "Settings"),
        MenuItem("Content Manager", MenuSection::INFO, "Web UI"),
        MenuItem("Phone Remote", MenuSection::PHONE_REMOTE, "Pair phone"),
        MenuItem("Back", MenuSection::BACK)
    };
}

void SettingsMenuManager::update() {
    if (!active_ && !is_opening_ && !is_closing_) return;

    // Check Wi-Fi scanning state if we are in the networks submenu
    if (current_submenu_ == MenuSection::WIFI_NETWORKS) {
        bool is_scanning = utils::WifiManager::instance().is_scanning();

        // While a scan runs, refresh twice a second: the scan worker
        // publishes partial results incrementally, so the operator sees
        // the network list grow live (plus the animated "Scanning..."
        // header) instead of a frozen "Please wait".
        if (is_scanning) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_scan_refresh_ >= std::chrono::milliseconds(500)) {
                last_scan_refresh_ = now;
                rebuild_current_submenu();
            }
        }

        // If we were scanning and now we stopped, we need to refresh to show results
        if (was_scanning_ && !is_scanning) {
            std::cout << "SettingsMenuManager: Scan finished, rebuilding submenu..." << std::endl;
            rebuild_current_submenu();
        }

        was_scanning_ = is_scanning;
    }

    // Refresh the Content Manager (INFO) submenu when its connection state
    // could have changed. Without this, the QR + label freeze on whatever
    // they read at the moment the user navigated INTO the submenu — so if
    // a USB cable was plugged in then unplugged while the user was looking
    // at the screen, the QR would keep encoding the now-unreachable
    // 10.55.0.1 URL until the user re-enters the submenu.
    //
    // Refresh once per second (not per frame) is fine — build_info_submenu
    // is cheap (a couple of getifaddrs + nmcli queries) and the QR + URL
    // update is the only visible side effect.
    if (current_submenu_ == MenuSection::INFO) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_info_refresh_ >= std::chrono::seconds(1)) {
            last_info_refresh_ = now;
            // Just rebuild — preserves selected_index via the idempotent
            // enter_submenu() path (commit dc0459c).
            rebuild_current_submenu();
        }
    }

    // Track connection state edges (across all submenus, not just WIFI/WIFI_NETWORKS).
    // Fires Toasts on both START and END so the operator gets immediate
    // feedback regardless of which submenu they happen to be looking at
    // when the state changes (e.g. they navigated back to the parent
    // Wi-Fi submenu while a connection was still in progress).
    {
        auto& wifi = utils::WifiManager::instance();
        bool is_connecting = wifi.is_connecting();

        // Edge: connect just STARTED (was idle, now connecting).
        if (!was_connecting_ && is_connecting) {
            std::string ssid = wifi.get_connecting_ssid();
            ui::Toast::show("Connecting to " + (ssid.empty() ? std::string("Wi-Fi") : ssid) + "...");
        }

        // Edge: connect just FINISHED (was connecting, now idle).
        if (was_connecting_ && !is_connecting) {
            std::cout << "SettingsMenuManager: Connection finished, refreshing menu..." << std::endl;
            // Toast the actual outcome so the operator sees a clear ✓ or
            // ✗ even if they're not looking at the Wi-Fi submenu right
            // this frame. The result enum was set by the worker thread
            // before is_connecting_ flipped to false, so it's already
            // populated by the time we read it here.
            utils::ConnectionResult result = wifi.get_connection_result();
            if (result == utils::ConnectionResult::SUCCESS) {
                ui::Toast::show("Connected to " + wifi.get_current_ssid());
            } else {
                std::string err = wifi.get_connection_error();
                ui::Toast::show(err.empty() ? std::string("Connection failed")
                                            : ("Connection failed: " + err));
            }
            // Refresh whichever submenu we're in so the new state shows.
            if (current_submenu_ == MenuSection::WIFI ||
                current_submenu_ == MenuSection::WIFI_NETWORKS) {
                rebuild_current_submenu();
            }
        }

        // Also: if we ARE in the Wi-Fi parent submenu while a connect is
        // in progress, refresh once-per-second so the "Connecting to X..."
        // line stays accurate even if SSID changes between frames (rare but
        // possible if the operator queues a second attempt). This is also
        // why we no longer gate the rebuild on the WIFI_NETWORKS-only check
        // that was here before — both submenus need to reflect the state.
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
    
    // CRT effect settings (show for both modes, but mostly useful for CRT Native).
    // The "CRT Engine" toggle controls which shader implementation processes
    // these effects: the v1.4.3 procedural alpha-overlay (Classic) or the
    // physically-motivated FBO-based pipeline (Enhanced — Lottes mask,
    // Gaussian scanlines, gamma+temp warmth, convergence error, halation).
    // Both interpret the 7 intensity sliders below, so flipping the engine
    // is a live A/B comparison without touching slider values.
    items.insert(items.end(), {
        MenuItem(std::string("CRT Engine: ") +
                     (settings.enhanced_crt_enabled ? "Enhanced" : "Classic"),
                 MenuSection::TOGGLE_ENHANCED_CRT, "Classic / Enhanced",
                 [&]() {
                     settings.enhanced_crt_enabled = !settings.enhanced_crt_enabled;
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_);
                 }),

        MenuItem("Scanlines: " + intensity_to_label(settings.scanline_intensity),
                 MenuSection::CYCLE_SCANLINES, "Horizontal CRT lines",
                 [&]() {
                     settings.cycle_setting(settings.scanline_intensity);
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_);
                 }),

        MenuItem("Color Warmth: " + intensity_to_label(settings.warmth_intensity),
                 MenuSection::CYCLE_WARMTH, "Warm tone + gamma",
                 [&]() {
                     settings.cycle_setting(settings.warmth_intensity);
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_);
                 }),

        MenuItem("Phosphor Glow: " + intensity_to_label(settings.glow_intensity),
                 MenuSection::CYCLE_GLOW, "Vignette + RGB convergence",
                 [&]() {
                     settings.cycle_setting(settings.glow_intensity);
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_);
                 }),

        MenuItem("RGB Mask: " + intensity_to_label(settings.rgb_mask_intensity),
                 MenuSection::CYCLE_RGB_MASK, "Subpixel R/G/B stripes",
                 [&]() {
                     settings.cycle_setting(settings.rgb_mask_intensity);
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_);
                 }),

        MenuItem("Screen Bloom: " + intensity_to_label(settings.bloom_intensity),
                 MenuSection::CYCLE_BLOOM, "Phosphor halation",
                 [&]() {
                     settings.cycle_setting(settings.bloom_intensity);
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_);
                 }),

        MenuItem("Interlacing: " + intensity_to_label(settings.interlacing_intensity),
                 MenuSection::CYCLE_INTERLACING, "Alternate-line darken",
                 [&]() {
                     settings.cycle_setting(settings.interlacing_intensity);
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_);
                 }),

        MenuItem("Flicker: " + intensity_to_label(settings.flicker_intensity),
                 MenuSection::CYCLE_FLICKER, "Brightness wobble",
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
    // Menu closed (or being closed) → open it.
    if (!active_ && !is_opening_) {
        open();
        return;
    }

    // Menu is open or opening. Treat the MENU button as "back one level"
    // when the user has dived into sub-pages, and only as "close entirely"
    // when they're at the top-level. This gives the operator a natural
    // back-stack matching the nav depth they actually entered:
    //
    //   games inside a playlist   ← MENU →   playlist list
    //   playlist list (game browser) ← MENU →   Video Games submenu
    //   Wi-Fi networks list       ← MENU →   Wi-Fi submenu
    //   any first-level submenu   ← MENU →   top-level menu
    //   top-level menu            ← MENU →   closed
    //
    // The order of these checks matters — deeper states are tested first
    // because some shallow states are also true at deeper depths
    // (e.g. game_browser_active_ stays true while viewing_games_in_playlist_).

    // Depth 4 (deepest): looking at the games inside a specific playlist.
    if (game_browser_active_ && viewing_games_in_playlist_) {
        exit_game_list();
        return;
    }

    // Depth 3: in the game browser at the playlist-list level.
    if (game_browser_active_) {
        exit_game_browser();
        return;
    }

    // Depth 3: Wi-Fi networks list (one level under the Wi-Fi submenu).
    if (current_submenu_ == MenuSection::WIFI_NETWORKS) {
        enter_submenu(MenuSection::WIFI);
        return;
    }

    // Depth 2: any first-level submenu (Display, Audio, Wi-Fi, System, etc.).
    if (current_submenu_ != MenuSection::BACK) {
        exit_submenu();
        return;
    }

    // Depth 1 (top-level): close the menu.
    close();
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
        game_browser_active_ = false;
        viewing_games_in_playlist_ = false;
        current_game_playlist_index_ = 0;
        selected_game_in_playlist_ = 0;
        game_browser_selected_ = 0;

#ifdef MEDIA_BROWSER_ENABLED
        // Layer 2 may have changed since boot if the operator just
        // dropped a WireGuard config via Content Manager. Re-read it
        // on each open so the UI reflects current state without
        // requiring a kiosk restart.
        if (app_state_) {
            std::ifstream f("/opt/magic_dingus_box/services/.env");
            bool found = false;
            if (f) {
                std::string line;
                while (std::getline(f, line)) {
                    constexpr std::string_view kKey = "WIREGUARD_PRIVATE_KEY=";
                    if (line.rfind(kKey, 0) != 0) continue;
                    std::string val = line.substr(kKey.size());
                    while (!val.empty() && (val.back() == ' ' || val.back() == '\t' ||
                                            val.back() == '"' || val.back() == '\'')) val.pop_back();
                    while (!val.empty() && (val.front() == ' ' || val.front() == '\t' ||
                                            val.front() == '"' || val.front() == '\'')) val.erase(val.begin());
                    found = !val.empty();
                    break;
                }
            }
            app_state_->media_browser_vpn_configured = found;

            // Is the movie drive attached? The library lives on an
            // external drive mounted at STORAGE_ROOT (/mnt/ssd); without
            // it Radarr cannot start, so the Movies row would just vanish
            // with no explanation. Re-checked on each open, same as the
            // WireGuard probe above.
            app_state_->media_browser_storage_present =
                platform::is_storage_mounted();
        }

        // Rebuild the top-level menu on every open() so the "Movies" row
        // (conditional on media_browser_unlocked) is re-evaluated each time.
        // Placement: directly under "Video Games" — the feature entry point
        // deserves prominent top-level placement. The administrative
        // "Hide Movies feature" row lives inside the System submenu.
        menu_items_.clear();
        menu_items_.emplace_back("Video Games", MenuSection::VIDEO_GAMES, "Emulated games");
        if (app_state_ && app_state_->media_browser_unlocked) {
            if (!app_state_->media_browser_vpn_configured) {
                // Layer 1 pass, Layer 2 fail: show an info-only row pointing
                // at Content Manager. Uses MenuSection::INFO so the row is
                // non-actionable (matches "Content Manager" row pattern below).
                menu_items_.emplace_back("Movies (configure VPN)", MenuSection::INFO,
                                         "Drop WireGuard config in Content Manager");
            } else if (!app_state_->media_browser_storage_present) {
                // Movie drive unplugged. Checked BEFORE vpn_healthy: a
                // missing drive also takes Radarr down, so both flags trip
                // — but "connect the drive" is the specific, actionable
                // cause and the one the owner can actually fix.
                menu_items_.emplace_back("Movies (drive not connected)", MenuSection::INFO,
                                         "Connect the movie drive, then restart");
            } else if (!app_state_->media_browser_vpn_healthy) {
                // Layer 1+2 pass, Layer 3 fail: hide entirely. Toast (fired
                // in main.cpp) is the user-visible signal; no row rendered
                // here so operators can immediately see the feature has
                // degraded rather than a misleading "click here" entry.
            } else {
                // All three layers pass — render Media Browser entry.
                menu_items_.emplace_back("Movies", MenuSection::MEDIA_BROWSER, "Media Browser");
            }
        }
        menu_items_.emplace_back("Display", MenuSection::DISPLAY, "Screen settings");
        menu_items_.emplace_back("Audio", MenuSection::AUDIO, "Volume");
        menu_items_.emplace_back("Wi-Fi", MenuSection::WIFI, "Network Setup");
        menu_items_.emplace_back("System", MenuSection::SYSTEM, "Settings");
        menu_items_.emplace_back("Content Manager", MenuSection::INFO, "Web UI");
        menu_items_.emplace_back("Phone Remote", MenuSection::PHONE_REMOTE, "Pair phone");
        menu_items_.emplace_back("Back", MenuSection::BACK);
#endif
    }
}

void SettingsMenuManager::close() {
    if (active_ || is_opening_) {
        is_closing_ = true;
        is_opening_ = false;
        animation_start_ = std::chrono::steady_clock::now();
    }
    // Clean up pairing session file whenever the settings menu is dismissed.
    close_pairing_screen();
}

void SettingsMenuManager::force_close() {
    active_ = false;
    is_opening_ = false;
    is_closing_ = false;
    close_pairing_screen();
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
            active_ = false;
            is_closing_ = false;
            return 0.0f;
        }
        return progress;
    } else {
        float progress = elapsed / animation_duration_;
        if (progress >= 1.0f) {
            is_opening_ = false;
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
    
    // Scrolling logic — use the renderer-published row count so we
    // only scroll when the selection actually goes off-screen. With
    // the previous hardcoded 7, Modern TV (which can show 9+ rows)
    // would still scroll once selected_index reached 7, popping the
    // top item out of view even though it was visibly fitting fine.
    const int max_visible_items = max_visible_items_;
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
        // Save section BEFORE calling action — action may rebuild the submenu,
        // invalidating any references into the items vector
        MenuSection section = items[selected_index_].section;
        auto action = items[selected_index_].action;
        if (action) {
            action();
        }
        return section;
    }
    return MenuSection::BACK;
}

void SettingsMenuManager::enter_submenu(MenuSection section) {
    // IDEMPOTENT RE-ENTRY: when called with the same section we're
    // already in, preserve transient state (selected_index_,
    // scroll_offset_, wifi_disconnect_confirm_) instead of resetting
    // it to defaults. This matters because main.cpp calls
    //   enter_submenu(WIFI)
    // immediately after the user presses SELECT on a WIFI-section
    // action item (like Disconnect), as part of the standard
    //   `case WIFI: enter_submenu(WIFI)`
    // dispatch in the menu's SELECT handler. Without this guard, a
    // user pressing "Disconnect" would have the action lambda flip
    // wifi_disconnect_confirm_ to true → rebuild submenu (showing
    // "Confirm Disconnect?") → main.cpp re-runs enter_submenu(WIFI)
    // which silently resets the flag back to false and snaps the
    // selection to item 0. The Confirm prompt would render for one
    // frame and then revert; the user could never reach the actual
    // forget_network() call. Same trap applied to any future submenu
    // action that needs to manage transient confirm-style state.
    const bool reentering = (current_submenu_ == section);
    const int preserved_index  = reentering ? selected_index_  : 0;
    const int preserved_scroll = reentering ? scroll_offset_   : 0;

    current_submenu_ = section;
    if (!reentering) {
        // Fresh entry: reset all transient flags.
        wifi_disconnect_confirm_ = false;
    }

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

    // Restore index + scroll, clamped to the (possibly newly-rebuilt)
    // item count.
    const int max_idx = std::max(0, static_cast<int>(submenu_items_.size()) - 1);
    selected_index_ = std::min(preserved_index,  max_idx);
    scroll_offset_  = std::min(preserved_scroll, max_idx);
}

void SettingsMenuManager::exit_submenu() {
    current_submenu_ = MenuSection::BACK;
    selected_index_ = 0;
    scroll_offset_ = 0;
    wifi_disconnect_confirm_ = false;
    submenu_items_.clear();
}

void SettingsMenuManager::rebuild_current_submenu() {
    if (current_submenu_ == MenuSection::BACK) {
        return;
    }

    int old_index = selected_index_;
    int old_scroll = scroll_offset_;
    enter_submenu(current_submenu_);
    selected_index_ = std::min(old_index, static_cast<int>(submenu_items_.size() - 1));
    scroll_offset_ = std::min(old_scroll, std::max(0, static_cast<int>(submenu_items_.size()) - 1));
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

// ── Phone Remote / PairingScreen ─────────────────────────────────────────────

PairingScreen* SettingsMenuManager::pairing_screen() {
    if (!pairing_screen_) {
        pairing_screen_ = std::make_unique<PairingScreen>(
            config::get_data_path() + "/pairing_session.json");
    }
    return pairing_screen_.get();
}

void SettingsMenuManager::open_pairing_screen() {
    pairing_active_ = true;
    pairing_screen()->regenerate();

    // Refresh the address the QR encodes. This MUST be read live rather
    // than hardcoded: the QR used to point at a literal "magicpi.local",
    // which exists only on a box named exactly that — and first_boot.sh
    // names every cloned Pi "magicpi-XXXX", so the QR resolved to nothing
    // on every shipped unit and the phone failed silently.
    if (app_state_) {
        std::string ip = get_interface_ipv4("wlan0");
        if (ip.empty()) ip = get_interface_ipv4("eth0");
        if (ip.empty()) ip = get_interface_ipv4("usb0");
        app_state_->lan_ip = ip;

        char host[256] = {0};
        if (gethostname(host, sizeof(host) - 1) == 0) {
            app_state_->hostname = host;
        }
    }
}

void SettingsMenuManager::close_pairing_screen() {
    if (pairing_active_) {
        pairing_screen()->close();
        pairing_active_ = false;
    }
}

std::vector<MenuItem> SettingsMenuManager::build_games_submenu() {
    return {
        MenuItem("Browse Games", MenuSection::BROWSE_GAMES, "Game libraries"),
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

#ifdef MEDIA_BROWSER_ENABLED
    std::vector<MenuItem> items = {
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
    };

    // Inject "Hide Movies feature" row conditionally. This submenu is rebuilt
    // on every enter_submenu(SYSTEM) call, so the unlock state is re-evaluated
    // each time the user navigates in — picking up changes made elsewhere
    // (e.g. fresh unlock via secret seq).
    //
    // NOTE: The "Movies" entry point lives at the top level (see open()),
    // directly under "Video Games". Only the administrative re-lock row
    // belongs here alongside Playlist Loop / Shuffle.
    if (app_state_->media_browser_unlocked) {
        items.emplace_back("Hide Movies feature", MenuSection::HIDE_MEDIA_BROWSER, "Re-lock Media Browser");
    }

    items.emplace_back("Back", MenuSection::BACK);
    return items;
#else
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
#endif
}

// Helper to check whether an interface is actually carrying traffic.
//
// Why both IPv4-assigned AND IFF_RUNNING checks: the `usb-gadget-network.service`
// always assigns 10.55.0.1/24 to usb0 at boot, regardless of whether anything
// is plugged into the USB-C port. So the previous check ("does usb0 have an
// IPv4 address?") returned true unconditionally on every boot, making the
// kiosk's Content Manager UI display "USB Connection (Active)" + a USB QR code
// even with no cable connected — exactly the bug the operator hit.
//
// IFF_RUNNING is the right OS-level signal here: it tracks the carrier-detect
// state, becoming true only when there's an actual link partner. For USB
// gadget Ethernet that means a real USB cable terminating in a host that has
// brought up its end of the connection. For wired Ethernet, an unplugged
// cable similarly clears IFF_RUNNING.
static bool is_interface_active(const char* iface_name) {
    struct ifaddrs *ifap, *ifa;
    if (getifaddrs(&ifap) == -1) return false;

    bool active = false;
    for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) { // IPv4
            if (std::string(ifa->ifa_name) == iface_name) {
                // Must have IPv4 assigned AND IFF_RUNNING (carrier present).
                // Without IFF_RUNNING we'd false-positive on usb0's
                // statically-assigned 10.55.0.1 when no cable is plugged in.
                if ((ifa->ifa_flags & IFF_RUNNING) != 0) {
                    active = true;
                    break;
                }
            }
        }
    }
    freeifaddrs(ifap);
    return active;
}

// Read the IPv4 address assigned to the named interface, or empty string.
// Used so the QR code reflects whatever IP the gadget service actually
// assigned (10.55.0.1, 192.168.7.1, or anything else) without hardcoding.
static std::string get_interface_ipv4(const char* iface_name) {
    struct ifaddrs *ifap, *ifa;
    if (getifaddrs(&ifap) == -1) return "";

    std::string addr_str;
    for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (std::string(ifa->ifa_name) != iface_name) continue;

        char buf[INET_ADDRSTRLEN] = {0};
        auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
            addr_str = buf;
        }
        break;
    }
    freeifaddrs(ifap);
    return addr_str;
}

std::vector<MenuItem> SettingsMenuManager::build_info_submenu() {
    auto& wifi = utils::WifiManager::instance();

    bool usb_active = is_interface_active("usb0");
    bool wifi_active = wifi.is_connected();

    // Determine primary connection (USB priority)
    std::string primary_url = "";
    std::string connection_label = "Not Connected";
    std::string sub_label = "Connect via USB or Wi-Fi";

    // Read the actual IP off usb0 instead of hardcoding (the address
    // assigned by usb-gadget-network.service is 10.55.0.1, but older
    // setups used 192.168.7.1 — we don't care which, just what's live).
    std::string usb_ip = get_interface_ipv4("usb0");

    if (usb_active && !usb_ip.empty()) {
        primary_url = "http://" + usb_ip + ":5000";
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

    // Update app state.
    //
    // CRITICAL: gate usb_url on usb_active (carrier present), not just
    // on usb_ip being non-empty. usb-gadget-network.service assigns
    // 10.55.0.1/24 to usb0 unconditionally at boot, so usb_ip is
    // ALWAYS "10.55.0.1" on this hardware, even when no cable is
    // plugged in. Without this gate, the renderer's QR-label logic
    // (which compares the displayed URL against state.usb_url) gets
    // a perpetually-truthy state.usb_url and mis-labels the QR as
    // "USB Connection (Preferred)" even on a Wi-Fi-only screen.
    if (app_state_) {
        app_state_->usb_url = (usb_active && !usb_ip.empty())
            ? ("http://" + usb_ip + ":5000") : "";
        app_state_->wifi_url = wifi_active
            ? ("http://" + wifi.get_ip_address() + ":5000") : "";
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
    // Threshold placement: midpoints between the four cycle_setting()
    // values (0.0 / 0.25 / 0.50 / 0.75) — i.e. 0.125, 0.375, 0.625.
    // Any cycle output lands cleanly inside its tier, and JSON-deserialized
    // float fuzz (e.g. 0.30 returning as 0.30000001192092896) cannot push
    // it into the wrong bucket.
    //
    // We use a small floor of 0.05 for OFF rather than `<= 0.0` so an
    // operator who accidentally hand-edits a tiny non-zero value like
    // 0.01 still reads as OFF (which matches what they'd visually see —
    // sub-5% intensity is invisible across all 7 effects).
    if (intensity < 0.05f) {
        return "OFF";
    } else if (intensity < 0.375f) {
        return "Low (" + std::to_string(static_cast<int>(intensity * 100)) + "%)";
    } else if (intensity < 0.625f) {
        return "Medium (" + std::to_string(static_cast<int>(intensity * 100)) + "%)";
    } else {
        return "High (" + std::to_string(static_cast<int>(intensity * 100)) + "%)";
    }
}

bool SettingsMenuManager::is_game_browser_back_selected() const {
    if (!game_browser_active_) return false;
    return false;
}

std::string SettingsMenuManager::get_current_highlighted_label() const {
    // Top menu vs submenu. Game-browser levels are reported separately via
    // the game_browser cursor indices (the harness maps those to names via
    // AppState.game_playlists), so this covers the menu/submenu rows a
    // caller navigates to *reach* the game browser (e.g. "Browse Games").
    const std::vector<MenuItem>& items =
        (current_submenu_ != MenuSection::BACK) ? submenu_items_ : menu_items_;
    if (selected_index_ >= 0 && selected_index_ < static_cast<int>(items.size())) {
        return items[selected_index_].label;
    }
    return "";
}

std::vector<MenuItem> SettingsMenuManager::build_wifi_submenu() {
    auto& wifi = utils::WifiManager::instance();

    // Status line — three possible states:
    //   1. Connection in progress: "Connecting to <SSID>..." (overrides the
    //      previous status). Without this, the operator pressed Enter on
    //      the password prompt and saw nothing happen for ~5-10 sec until
    //      the line silently flipped to "Connected" — felt like the kiosk
    //      had hung. Now they get an immediate, clearly-named in-progress
    //      indicator the moment connect_async() fires.
    //   2. Connected: "Connected: <SSID>" + IP as the meta line.
    //   3. Not connected: "Not Connected" + empty meta.
    std::string status;
    std::string meta;
    if (wifi.is_connecting()) {
        std::string target = wifi.get_connecting_ssid();
        status = "Connecting to " + (target.empty() ? std::string("Wi-Fi") : target) + "...";
        meta = "Verifying credentials...";
    } else if (wifi.is_connected()) {
        status = "Connected: " + wifi.get_current_ssid();
        meta = wifi.get_ip_address();
    } else {
        status = "Not Connected";
    }

    std::vector<MenuItem> items;
    items.emplace_back("Status: " + status, MenuSection::BACK, meta);
    items.emplace_back("Scan Networks", MenuSection::WIFI_NETWORKS, "Find Wi-Fi",
                 [&]() {
                     utils::WifiManager::instance().scan_networks_async();
                 });

    // Disconnect only makes sense with an active connection — hide it
    // otherwise (it used to sit there permanently and silently no-op
    // when nothing was connected). Name the network in both the item
    // and the confirm step so the operator knows exactly what they're
    // forgetting, and toast the outcome.
    std::string current = wifi.is_connected() ? wifi.get_current_ssid() : "";
    if (!current.empty()) {
        items.emplace_back(
            wifi_disconnect_confirm_ ? "Confirm: forget " + current + "?"
                                     : "Disconnect from " + current,
            MenuSection::WIFI,
            wifi_disconnect_confirm_ ? "Press again to disconnect + forget"
                                     : "Drops Wi-Fi and deletes the saved password",
            [this, current]() {
                if (wifi_disconnect_confirm_) {
                    bool ok = utils::WifiManager::instance().forget_network(current);
                    ui::Toast::show(ok ? "Forgot network " + current
                                       : "Could not forget " + current);
                    wifi_disconnect_confirm_ = false;
                    rebuild_current_submenu();
                } else {
                    wifi_disconnect_confirm_ = true;
                    rebuild_current_submenu();
                }
            });
    }
    items.emplace_back("Back", MenuSection::BACK);
    return items;
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
    if (result == utils::ConnectionResult::FAILURE || result == utils::ConnectionResult::TIMEOUT) {
        std::string error = wifi.get_connection_error();
        // Recovery path: a failed saved-profile reconnect used to be a
        // dead end ("forget and reconnect" — with no way to do either
        // from this screen). Offer a password re-entry that goes down
        // the fresh-credentials path, which replaces the stale profile.
        std::string failed_ssid = wifi.get_last_failed_ssid();
        bool offer_reentry = !failed_ssid.empty();
        wifi.reset_connection_state();

        std::vector<MenuItem> items;
        items.emplace_back("Connection Failed", MenuSection::BACK,
                           error.empty() ? "Unknown error" : error);
        if (offer_reentry) {
            items.emplace_back("Enter New Password", MenuSection::WIFI,
                "Retry " + failed_ssid + " with fresh credentials",
                [this, failed_ssid]() {
                    if (app_state_ && app_state_->keyboard) {
                        app_state_->keyboard->open("", "Enter Password for " + failed_ssid,
                            [failed_ssid](const std::string& password) {
                                utils::WifiManager::instance().connect_async(
                                    failed_ssid, password, /*fresh_credentials=*/true);
                            },
                            [this]() { enter_submenu(MenuSection::WIFI); });
                    }
                });
        }
        items.emplace_back("Back", MenuSection::BACK, "Return to list",
            []() { utils::WifiManager::instance().scan_networks_async(); });
        return items;
    }

    // While scanning, show the partial results as they stream in (the
    // scan worker publishes incrementally) with a live header. update()
    // rebuilds this submenu twice a second while is_scanning() so the
    // list visibly grows instead of sitting on a static "Please wait".
    const bool scanning = wifi.is_scanning();
    auto results = wifi.get_scan_results();
    std::vector<MenuItem> items;

    if (scanning) {
        int dots = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count()
            / 500 % 4);
        items.emplace_back("Scanning" + std::string(dots, '.'),
                           MenuSection::BACK,
                           "Found " + std::to_string(results.size()) +
                           " so far");
    }

    for (const auto& net : results) {
        std::string label = net.ssid;
        if (net.in_use) label += " (Connected)";
        else if (net.saved) label += " (Saved)";

        const bool open_network = net.security.empty();
        std::string sub = "Signal: " + std::to_string(net.signal_strength) + "% " +
                          (open_network ? std::string("Open") : net.security);

        if (net.in_use) {
            // Already connected - no action needed
            items.emplace_back(label, MenuSection::BACK, sub);
        } else if (net.saved) {
            // Saved network - reconnect using the stored password
            items.emplace_back(label, MenuSection::WIFI, sub,
                [this, net]() {
                    utils::WifiManager::instance().connect_async(
                        net.ssid, "", /*fresh_credentials=*/false);
                    enter_submenu(MenuSection::WIFI);
                });
        } else if (open_network) {
            // Open network — no password to ask for. The old code sent
            // this through the password keyboard, and the resulting
            // empty-password connect took the saved-profile path and
            // failed with "unknown connection" every time.
            items.emplace_back(label, MenuSection::WIFI, sub,
                [this, net]() {
                    utils::WifiManager::instance().connect_async(
                        net.ssid, "", /*fresh_credentials=*/true);
                    enter_submenu(MenuSection::WIFI);
                });
        } else {
            items.emplace_back(label, MenuSection::WIFI, sub,
                [this, net]() {
                    if (app_state_ && app_state_->keyboard) {
                        app_state_->keyboard->open("", "Enter Password for " + net.ssid,
                            [net](const std::string& password) {
                                utils::WifiManager::instance().connect_async(
                                    net.ssid, password, /*fresh_credentials=*/true);
                            },
                            [this]() {
                                enter_submenu(MenuSection::WIFI);
                            }
                        );
                    }
                });
        }
    }

    if (items.empty()) {
        items.emplace_back("No networks found", MenuSection::BACK);
    }

    if (!scanning) {
        items.emplace_back("Rescan", MenuSection::WIFI, "", [&](){
             utils::WifiManager::instance().scan_networks_async();
             rebuild_current_submenu();
        });
    }

    items.emplace_back("Back", MenuSection::WIFI);
    return items;
}

} // namespace ui
