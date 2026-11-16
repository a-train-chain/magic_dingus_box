#include "settings_menu.h"
#include <unistd.h>
#include "controller_wizard.h"
#include "pairing_screen.h"
#include "toast.h"
#include "virtual_keyboard.h"
#include "../utils/config.h"
#include "../utils/services_env.h"
#include "../utils/wifi_manager.h"
#include "../platform/platform_profile.h"
#include "../retroarch/controller_profile.h"
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
#include "../app/redraw_gate.h"
#include "../app/settings_persistence.h"
#include "../app/tube_menu_logic.h"
#include "../tube/tube_groups.h"
#include "../tube/tube_setting.h"
#ifdef MEDIA_BROWSER_ENABLED
#include "../media_browser/mb_entry_gate.h"
#endif

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
        // ONE connection surface. This replaced the two-row split
        // ("Content Manager" Info screen + "Connect Phone / Computer"):
        // both screens' QRs already landed on the same /connect page, so
        // two rows were two names for one thing. The pairing screen is
        // now the single "Connect a Device" surface — QR, 6-digit code,
        // typed address, and the USB-C cable path.
        MenuItem("Connect a Device", MenuSection::PHONE_REMOTE, "Phone, computer, or USB cable"),
        MenuItem("Pair Wireless Controller", MenuSection::PAIR_CONTROLLER,
                 "PlayStation, Xbox, Switch, 8BitDo"),
        MenuItem("Controller Setup", MenuSection::CONTROLLER_SETUP, "Map any gamepad's buttons"),
        MenuItem("Back", MenuSection::BACK)
    };
}

// Out-of-line so the unique_ptr members can stay forward-declared in the
// header — see the note there.
SettingsMenuManager::~SettingsMenuManager() = default;

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

    // An open Tube group shows which effects the box is not drawing. A web edit already
    // rebuilds the page (main.cpp, page_shows_tube_state); the renderer's own changes (the
    // picture, drawing starting or stopping) are caught here: one allocation-free
    // plan_passes_and_drops() a frame, only while a group is open, and a rebuild only when
    // the answer differs from the one the rows were built with.
    if (current_submenu_ == MenuSection::TUBE_GROUP && app_state_ &&
        app::tube_not_drawn(*app_state_) != tube_page_not_drawn_) {
        rebuild_current_submenu();
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
                // The SSID the worker just connected to — NOT
                // get_current_ssid(): that runs nmcli synchronously (up
                // to two invocations, 15 s timeout each) and this is the
                // render thread, which pings the systemd watchdog.
                const std::string ssid = wifi.get_last_connected_ssid();
                ui::Toast::show("Connected to " +
                                (ssid.empty() ? std::string("Wi-Fi") : ssid));
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

        // Async forget's outcome. The confirm action armed was_forgetting_;
        // the worker stores its result before is_forgetting falls, so the
        // result is ready on this edge.
        if (was_forgetting_ && !wifi.is_forgetting()) {
            was_forgetting_ = false;
            ui::Toast::show(wifi.last_forget_succeeded()
                                ? "Forgot network " + forgetting_ssid_
                                : "Could not forget " + forgetting_ssid_);
            if (current_submenu_ == MenuSection::WIFI ||
                current_submenu_ == MenuSection::WIFI_NETWORKS) {
                rebuild_current_submenu();
            }
        }
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
        MenuItem(std::string("Menu Style: ") +
                     (settings.menu_style == app::MenuStyle::ChannelGuide ? "Channel Guide"
                                                                          : "List"),
                 MenuSection::TOGGLE_MENU_STYLE, "List / Channel guide",
                 [&]() {
                     settings.menu_style = (settings.menu_style == app::MenuStyle::List)
                                               ? app::MenuStyle::ChannelGuide
                                               : app::MenuStyle::List;
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_);
                 }),
    };
    
    if (settings.menu_style == app::MenuStyle::ChannelGuide) {
        items.emplace_back(std::string("Guide Mode: ") +
                               (settings.guide_mode == app::GuideMode::Live ? "Live" : "Pick Ahead"),
                           MenuSection::TOGGLE_GUIDE_MODE, "Live / Pick Ahead",
                           [&]() {
                               settings.guide_mode = settings.guide_mode == app::GuideMode::Live
                                   ? app::GuideMode::PickAhead : app::GuideMode::Live;
                               rebuild_current_submenu();
                               app::SettingsPersistence::save_settings(*app_state_);
                           });
    }

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
    // The "CRT Engine" row picks which shader implementation draws them: the
    // v1.4.3 procedural alpha-overlay (Classic), the physically-motivated
    // FBO-based pipeline (Enhanced — Lottes mask, Gaussian scanlines, gamma+temp
    // warmth, convergence error, halation), or — only where app::tube_offered —
    // the look-based Tube engine. Classic and Enhanced both interpret the 7
    // intensity sliders below, so flipping between them is a live A/B
    // comparison without touching slider values; Tube swaps those rows for its
    // own (look, reset, the ten groups, Degauss).
    const bool tube_offered = app::tube_offered(*app_state_);
    items.emplace_back(app::crt_engine_label(settings.crt_engine, tube_offered),
                       MenuSection::CYCLE_CRT_ENGINE,
                       std::string(app::crt_engine_sublabel(tube_offered)),
                       [this, &settings]() {
                           // Through the setter so crt_engine and the old flag
                           // stay in step. Stepped from the EFFECTIVE engine: a
                           // saved Tube where Tube is not offered reads
                           // "Enhanced" and steps to Classic.
                           settings.set_crt_engine(app::next_crt_engine(
                               settings.crt_engine, app::tube_offered(*app_state_)));
                           rebuild_current_submenu();
                           app::SettingsPersistence::save_settings(*app_state_);
                       });

    if (app::display_crt_rows(settings.crt_engine, tube_offered).tube) {
        auto& tube_setting = settings.tube;
        const bool crt_native = settings.mode == app::DisplayMode::CRT_NATIVE;
        items.emplace_back(app::tube_look_label(tube_setting, crt_native, app::tube_film_engine(*app_state_)),
                           MenuSection::CYCLE_TUBE_LOOK,
                           app::tube_look_sublabel(), [this, &tube_setting]() {
                               tube::select_look(tube_setting, app::next_look(tube_setting.look));
                               rebuild_current_submenu();
                               app::SettingsPersistence::save_settings(*app_state_);
                           });
        items.emplace_back("Reset to look", MenuSection::TUBE_RESET_LOOK,
                           app::tube_reset_sublabel(tube_setting), [this, &tube_setting]() {
                               if (tube::moved_keys(tube_setting).empty()) return;
                               tube::reset_to_look(tube_setting);
                               rebuild_current_submenu();
                               app::SettingsPersistence::save_settings(*app_state_);
                           });
        for (std::size_t i = 0; i < tube::kGroups.size(); ++i) {
            items.emplace_back(app::tube_group_label(tube::kGroups[i]), MenuSection::TUBE_GROUP,
                               app::tube_group_sublabel(tube::kGroups[i], tube_setting),
                               [this, i]() { enter_tube_group(i); });
        }
        // A live request, never saved and never toasted: the renderer (Plan 3)
        // consumes the increase; until then nothing is drawn.
        items.emplace_back("Degauss", MenuSection::TUBE_DEGAUSS, "demagnetize the tube",
                           [this]() { ++app_state_->tube_live.degauss_requests; });
        items.emplace_back("Back", MenuSection::BACK);
        return items;
    }

    items.insert(items.end(), {
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

    // Depth 4: the Controller Setup wizard owns the whole screen. MENU here
    // means "cancel the wizard", NOT "close Settings" — the operator came
    // from the Settings list and should land back on it. This is also the
    // cancel path that actually fires in practice: main.cpp handles
    // SETTINGS_MENU at the top of its dispatch loop (BTN4 hold/volume logic)
    // and routes the short press here, so the event never reaches the
    // wizard's own on_action().
    if (wizard_active_) {
        close_controller_wizard();
        return;
    }

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

    // Tube edit mode: MENU ends the edit (saving it), and only that.
    if (tube_edit_.active) {
        end_tube_edit();
        return;
    }

    // Depth 3: a Tube group (one level under Display).
    if (current_submenu_ == MenuSection::TUBE_GROUP) {
        exit_tube_group();
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
        settle_tube_edit();
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
            app_state_->media_browser_vpn_configured =
                !utils::read_env_value(utils::kServicesEnvPath,
                                       "WIREGUARD_PRIVATE_KEY").empty();

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
            // Precedence lives in movies_row_state() (pure logic,
            // unit-tested on the Mac) — this switch is only the mapping
            // from decision to row. Display mode is checked first: the
            // MB screens are authored for the 720p logical canvas and
            // CRT_NATIVE runs 640x480, so no amount of VPN/drive fixing
            // makes the feature usable there.
            const auto row = media_browser::movies_row_state(
                app_state_->display_settings.mode == app::DisplayMode::CRT_NATIVE,
                app_state_->media_browser_vpn_configured,
                app_state_->media_browser_storage_present,
                app_state_->media_browser_vpn_healthy);
            switch (row) {
            case media_browser::MoviesRowState::NeedsModernTv:
                // Non-actionable like the INFO rows below, but with its
                // own section so SELECT can toast the reason instead of
                // silently no-opping (dispatched in main.cpp).
                menu_items_.emplace_back(media_browser::kMoviesNeedsModernTvLabel,
                                         MenuSection::MEDIA_BROWSER_NEEDS_DISPLAY,
                                         media_browser::kMoviesNeedsModernTvSublabel);
                break;
            case media_browser::MoviesRowState::NeedsVpnConfig:
                // Layer 1 pass, Layer 2 fail: show an info-only row pointing
                // at the Content Manager (reached via "Connect a Device"
                // below). MenuSection::INFO rows are non-actionable — the
                // Info screen retired with the connect-flow merge, and
                // main.cpp no longer dispatches INFO, so SELECT is a no-op.
                menu_items_.emplace_back("Movies (configure VPN)", MenuSection::INFO,
                                         "Drop WireGuard config in Content Manager");
                break;
            case media_browser::MoviesRowState::NeedsDrive:
                // Movie drive unplugged. Checked BEFORE vpn_healthy: a
                // missing drive also takes Radarr down, so both flags trip
                // — but "connect the drive" is the specific, actionable
                // cause and the one the owner can actually fix.
                menu_items_.emplace_back("Movies (drive not connected)", MenuSection::INFO,
                                         "Connect the movie drive, then restart");
                break;
            case media_browser::MoviesRowState::HiddenTunnelDown:
                // Layer 1+2 pass, Layer 3 fail: hide entirely. Toast (fired
                // in main.cpp) is the user-visible signal; no row rendered
                // here so operators can immediately see the feature has
                // degraded rather than a misleading "click here" entry.
                break;
            case media_browser::MoviesRowState::Ready:
                // All layers pass — render Media Browser entry.
                menu_items_.emplace_back("Movies", MenuSection::MEDIA_BROWSER, "Media Browser");
                break;
            }
        }
        menu_items_.emplace_back("Display", MenuSection::DISPLAY, "Screen settings");
        menu_items_.emplace_back("Audio", MenuSection::AUDIO, "Volume");
        menu_items_.emplace_back("Wi-Fi", MenuSection::WIFI, "Network Setup");
        menu_items_.emplace_back("System", MenuSection::SYSTEM, "Settings");
        // Same row as the constructor's static list — keep the title in
        // sync. The single "Connect a Device" surface (pairing screen)
        // covers phone remote, Content Manager, and the USB-C cable path.
        menu_items_.emplace_back("Connect a Device", MenuSection::PHONE_REMOTE,
                                 "Phone, computer, or USB cable");
        menu_items_.emplace_back("Pair Wireless Controller", MenuSection::PAIR_CONTROLLER,
                                 "PlayStation, Xbox, Switch, 8BitDo");
        menu_items_.emplace_back("Controller Setup", MenuSection::CONTROLLER_SETUP,
                                 "Map any gamepad's buttons");
        menu_items_.emplace_back("Back", MenuSection::BACK);
#endif
    }
}

void SettingsMenuManager::close() {
    end_tube_edit();   // saves a pending edit; the sliding-out row drops its "< >"
    if (active_ || is_opening_) {
        is_closing_ = true;
        is_opening_ = false;
        animation_start_ = std::chrono::steady_clock::now();
    }
    // Clean up pairing session file whenever the settings menu is dismissed.
    close_pairing_screen();
    // ...and never leave the wizard holding InputManager in raw-capture mode.
    //
    // DEFENSE IN DEPTH, not a live bug fix. No caller can reach this while the
    // wizard is up today: main.cpp's wizard interception block ends in
    // `continue` before the SELECT dispatch that launches a game, and the
    // force_close() below it is gated on current_screen == MediaBrowser, which
    // that same `continue` makes unreachable. The point is that "Settings
    // dismissed ⇒ wizard retired" holds HERE, structurally, instead of resting
    // on main.cpp continuing to swallow every event — a future dispatch path
    // that reaches close() would otherwise strand InputManager in raw-capture
    // mode with nobody draining, i.e. every gamepad dead until restart.
    close_controller_wizard();
}

void SettingsMenuManager::force_close() {
    end_tube_edit();
    active_ = false;
    is_opening_ = false;
    is_closing_ = false;
    close_pairing_screen();
    close_controller_wizard();
}

bool SettingsMenuManager::is_static_for_redraw() const {
    if (!active_ || is_opening_ || is_closing_) return false;
    if (pairing_active_ || wizard_active_) return false;
    if (game_browser_active_) return false;
    const auto& wifi = utils::WifiManager::instance();
    if (wifi.is_scanning() || wifi.is_connecting() || wifi.is_forgetting()) {
        return false;
    }
    return true;
}

uint64_t SettingsMenuManager::redraw_signature() const {
    app::ContentSignature sig;
    sig.add(static_cast<uint64_t>(current_submenu_));
    sig.add(static_cast<uint64_t>(selected_index_));
    sig.add(static_cast<uint64_t>(scroll_offset_));
    sig.add(static_cast<uint64_t>(wifi_disconnect_confirm_));
    sig.add(static_cast<uint64_t>(controller_reset_confirm_));
    sig.add(static_cast<uint64_t>(tube_group_index_));
    sig.add(static_cast<uint64_t>(tube_edit_.active));
    sig.add(static_cast<uint64_t>(tube_edit_.mask));
    sig.add(static_cast<uint64_t>(tube::index_of(tube_edit_.effect)));
    for (const auto* items : {&menu_items_, &submenu_items_}) {
        sig.add(static_cast<uint64_t>(items->size()));
        for (const auto& item : *items) {
            sig.add(item.label);
            sig.add(item.sublabel);
        }
    }
    return sig.value();
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
    if (!reentering) settle_tube_edit();   // leaving a group mid-edit saves it
    const int preserved_index  = reentering ? selected_index_  : 0;
    const int preserved_scroll = reentering ? scroll_offset_   : 0;

    current_submenu_ = section;
    if (!reentering) {
        // Fresh entry: reset all transient flags.
        wifi_disconnect_confirm_ = false;
        controller_reset_confirm_ = false;
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
    } else if (section == MenuSection::TUBE_GROUP) {
        // The engine changed (or Tube stopped being offered) while the group
        // was open: Tube rows would be lying, so go back to Display instead.
        if (!app_state_ || !app::display_crt_rows(app_state_->display_settings.crt_engine,
                                                  app::tube_offered(*app_state_)).tube) {
            settle_tube_edit();
            exit_tube_group();
            return;
        }
        // The row being edited became n/a (the mode changed to CRT Native from
        // elsewhere): it no longer shows, so its edit must not go on unseen.
        if (tube_edit_.active &&
            app::tube_row_kind(tube_edit_.mask ? std::nullopt : std::optional<tube::Effect>(tube_edit_.effect),
                               app_state_->display_settings.mode == app::DisplayMode::CRT_NATIVE) ==
                app::TubeRowKind::NotApplicable) {
            settle_tube_edit();
        }
        submenu_items_ = build_tube_group_submenu();
    }

    // Restore index + scroll, clamped to the (possibly newly-rebuilt)
    // item count.
    const int max_idx = std::max(0, static_cast<int>(submenu_items_.size()) - 1);
    selected_index_ = std::min(preserved_index,  max_idx);
    scroll_offset_  = std::min(preserved_scroll, max_idx);
}

void SettingsMenuManager::enter_tube_group(std::size_t group) {
    tube_group_index_ = group < tube::kGroups.size() ? group : 0;
    tube_group_return_index_ = selected_index_;
    enter_submenu(MenuSection::TUBE_GROUP);
}

void SettingsMenuManager::exit_tube_group() {
    // Only from a group: elsewhere it would re-enter Display and move the cursor.
    if (current_submenu_ != MenuSection::TUBE_GROUP) return;
    settle_tube_edit();
    enter_submenu(MenuSection::DISPLAY);
    // Back on the group's own row (found by its label: the cursor that opened
    // it is only a fallback), scrolled into view.
    const int last = std::max(0, static_cast<int>(submenu_items_.size()) - 1);
    selected_index_ = std::clamp(tube_group_return_index_, 0, last);
    const std::string group_label = app::tube_group_label(tube::kGroups[tube_group_index_]);
    for (std::size_t i = 0; i < submenu_items_.size(); ++i) {
        if (submenu_items_[i].section == MenuSection::TUBE_GROUP &&
            submenu_items_[i].label == group_label) {
            selected_index_ = static_cast<int>(i);
            break;
        }
    }
    if (selected_index_ < scroll_offset_) {
        scroll_offset_ = selected_index_;
    } else if (selected_index_ >= scroll_offset_ + max_visible_items_) {
        scroll_offset_ = selected_index_ - max_visible_items_ + 1;
    }
}

void SettingsMenuManager::begin_tube_edit(std::optional<tube::Effect> effect) {
    if (!app_state_ || current_submenu_ != MenuSection::TUBE_GROUP) return;
    // An edit already open is saved first (tube::begin_edit would carry its
    // change over, but each row's edit saves on its own).
    settle_tube_edit();
    tube::begin_edit(tube_edit_, effect);
    rebuild_current_submenu();
}

void SettingsMenuManager::step_tube_edit(int direction) {
    if (!app_state_) return;
    if (tube::edit_turn(tube_edit_, app_state_->display_settings.tube, direction)) {
        rebuild_current_submenu();   // never saves: end_tube_edit saves once
    }
}

void SettingsMenuManager::end_tube_edit() {
    if (!tube_edit_.active) return;
    settle_tube_edit();
    rebuild_current_submenu();       // the row drops its "< >"
}

void SettingsMenuManager::settle_tube_edit() {
    if (!tube_edit_.active) return;
    if (tube::end_edit(tube_edit_) && app_state_) {
        app::SettingsPersistence::save_settings(*app_state_);
    }
}

std::vector<MenuItem> SettingsMenuManager::build_tube_group_submenu() {
    std::vector<MenuItem> items;
    if (app_state_) {
        const tube::Group& group = tube::kGroups[tube_group_index_];
        const tube::Resolved resolved = tube::resolve(app_state_->display_settings.tube);
        const bool crt_native = app_state_->display_settings.mode == app::DisplayMode::CRT_NATIVE;
        const tube::NotDrawn not_drawn = app::tube_not_drawn(*app_state_);
        tube_page_not_drawn_ = not_drawn;
        const auto not_applicable = [&items](std::string_view label) {
            items.emplace_back(app::tube_na_label(label), MenuSection::INFO,
                               std::string(app::kTubeNaSublabel));
        };
        if (group.has_mask_row) {
            if (app::tube_row_kind(std::nullopt, crt_native) == app::TubeRowKind::NotApplicable) {
                not_applicable(tube::kMaskRowInfo.label);
            } else {
                const bool editing = tube_edit_.active && tube_edit_.mask;
                items.emplace_back(app::tube_mask_label(resolved.mask, editing), MenuSection::TUBE_MASK,
                                   std::string(tube::mask_choice(resolved.mask).hint1),
                                   [this]() { begin_tube_edit(std::nullopt); });
            }
        }
        for (const tube::Effect effect : group.rows) {
            const tube::RowInfo& info = tube::row_info(effect);
            if (app::tube_row_kind(effect, crt_native) == app::TubeRowKind::NotApplicable) {
                not_applicable(info.label);
                continue;
            }
            const bool editing = tube_edit_.active && !tube_edit_.mask && tube_edit_.effect == effect;
            items.emplace_back(app::tube_value_label(effect, resolved.effects[tube::index_of(effect)], editing),
                               MenuSection::TUBE_VALUE, app::tube_value_sublabel(effect, not_drawn),
                               [this, effect]() { begin_tube_edit(effect); });
        }
    }
    items.emplace_back("Back", MenuSection::TUBE_GROUP_BACK, "", [this]() { exit_tube_group(); });
    return items;
}

std::string SettingsMenuManager::submenu_title() const {
    switch (current_submenu_) {
        case MenuSection::BACK:          return "Settings";
        case MenuSection::VIDEO_GAMES:   return "Video games";
        case MenuSection::DISPLAY:       return "Display";
        case MenuSection::AUDIO:         return "Audio";
        case MenuSection::SYSTEM:        return "System info";
        case MenuSection::WIFI:          return "Wi-Fi";
        case MenuSection::WIFI_NETWORKS: return "Wi-Fi Networks";
        case MenuSection::TUBE_GROUP:    return app::tube_group_label(tube::kGroups[tube_group_index_]);
        default:                         return "Settings";
    }
}

void SettingsMenuManager::exit_submenu() {
    settle_tube_edit();
    current_submenu_ = MenuSection::BACK;
    selected_index_ = 0;
    scroll_offset_ = 0;
    wifi_disconnect_confirm_ = false;
    controller_reset_confirm_ = false;
    submenu_items_.clear();
}

void SettingsMenuManager::rebuild_current_submenu() {
    if (current_submenu_ == MenuSection::BACK) {
        return;
    }

    const MenuSection section = current_submenu_;
    int old_index = selected_index_;
    int old_scroll = scroll_offset_;
    enter_submenu(section);
    // A group that redirected to Display (see enter_submenu) placed its own cursor.
    if (current_submenu_ != section) return;
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

// ── Controller Setup wizard ──────────────────────────────────────────────────

ControllerWizard* SettingsMenuManager::controller_wizard() {
    if (!controller_wizard_) {
        controller_wizard_ = std::make_unique<ControllerWizard>();
        controller_wizard_->set_bluetooth(bt_port_);
    }
    return controller_wizard_.get();
}

void SettingsMenuManager::open_controller_wizard(platform::InputManager* input) {
    wizard_active_ = true;
    controller_wizard()->open(input);
}

void SettingsMenuManager::open_controller_wizard_for_pairing(platform::InputManager* input) {
    wizard_active_ = true;
    controller_wizard()->open_for_pairing(input);
}

void SettingsMenuManager::set_bluetooth_port(platform::bt::BluetoothPort* port) {
    bt_port_ = port;
    if (controller_wizard_) controller_wizard_->set_bluetooth(port);
}

void SettingsMenuManager::close_controller_wizard() {
    // Unconditional close() on the wizard, not gated on wizard_active_: the
    // wizard can retire itself (cancel, idle timeout, pad unplugged) a frame
    // before this flag catches up, and ControllerWizard::close() is a no-op
    // once it has already run.
    if (controller_wizard_) controller_wizard_->close();
    wizard_active_ = false;
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

#ifdef MEDIA_BROWSER_ENABLED
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
#endif

    // ── Reset Controller Setup ──────────────────────────────────────────────
    //
    // THE UNDO FOR THE CONTROLLER SETUP WIZARD. A captured profile
    // unconditionally shadows the built-in one for its VID/PID
    // (resolve_mapping_for_pad) and the file is deliberately immune to OTA
    // updates, so before this row existed a bad capture could only be
    // corrected by completing all 24 wizard steps again — on the pad the bad
    // capture had just broken. There was no path back to the shipped mapping
    // from the kiosk at all.
    //
    // It clears EVERY captured profile rather than just the pad in front of
    // the operator. Targeting one pad means enumerating it first, and "the
    // reset did nothing because the box could not see your controller" is
    // exactly the dead end this row exists to remove. In practice the store
    // holds only pads that were deliberately captured (both fielded pads
    // share one VID/PID, i.e. one entry), and the row is hidden entirely when
    // there is nothing to erase — same idiom as the Wi-Fi "Disconnect" row,
    // which is hidden when nothing is connected.
    //
    // Two-press confirm, matching that same Wi-Fi row: destructive and
    // irreversible (the captured mapping is not backed up anywhere).
    const size_t captured_count = retroarch::load_profile_store().size();
    if (captured_count > 0) {
        items.emplace_back(
            controller_reset_confirm_ ? "Confirm: erase controller setup?"
                                      : "Reset Controller Setup",
            MenuSection::SYSTEM,
            controller_reset_confirm_
                ? "Press again to restore the built-in mapping"
                : (captured_count == 1
                       ? "Discard the saved mapping, use the built-in one"
                       : "Discard " + std::to_string(captured_count) +
                             " saved mappings, use the built-in ones"),
            [this]() {
                if (!controller_reset_confirm_) {
                    controller_reset_confirm_ = true;
                    rebuild_current_submenu();
                    return;
                }
                controller_reset_confirm_ = false;
                const bool ok = retroarch::save_profile_store({});
                // Report the truth: a read-only /opt or a full SD card fails
                // here exactly as it fails in the wizard's save.
                ui::Toast::show(ok ? "Controller setup reset — using built-in mapping"
                                   : "Could not reset controller setup");
                if (ok) controller_profiles_dirty_ = true;
                rebuild_current_submenu();
            });
    }

    items.emplace_back("Back", MenuSection::BACK);
    return items;
}

bool SettingsMenuManager::take_controller_profiles_dirty() {
    const bool dirty = controller_profiles_dirty_;
    controller_profiles_dirty_ = false;
    return dirty;
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
    } else if (wifi.is_forgetting()) {
        status = "Forgetting " + wifi.get_forgetting_ssid() + "...";
    } else if (auto ws = utils::WifiManager::instance().get_status_cached();
               ws.connected) {
        status = "Connected: " + ws.ssid;
        meta = ws.ip;
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
    const auto ws = wifi.get_status_cached();
    std::string current = ws.connected ? ws.ssid : "";
    // Hidden while a forget runs: the cached status still reads connected
    // until the worker invalidates it.
    if (!current.empty() && !wifi.is_forgetting()) {
        items.emplace_back(
            wifi_disconnect_confirm_ ? "Confirm: forget " + current + "?"
                                     : "Disconnect from " + current,
            MenuSection::WIFI,
            wifi_disconnect_confirm_ ? "Press again to disconnect + forget"
                                     : "Drops Wi-Fi and deletes the saved password",
            [this, current]() {
                if (wifi_disconnect_confirm_) {
                    // Async: forget_network() is two sudo nmcli calls (15 s
                    // timeout each) and this runs on the render thread,
                    // inside systemd's 10 s watchdog. update() toasts the
                    // outcome on the worker's falling edge.
                    if (utils::WifiManager::instance().forget_network_async(current)) {
                        // Arm the edge HERE, not in update(): a fast worker
                        // can finish before the next update() would see it
                        // running, and the outcome toast would be lost.
                        was_forgetting_ = true;
                        forgetting_ssid_ = current;
                        ui::Toast::show("Forgetting " + current + "...");
                    } else {
                        ui::Toast::show("Wi-Fi is busy — try again in a moment");
                    }
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
