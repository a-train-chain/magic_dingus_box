#include "settings_input.h"

#include "controller.h"
#include "game_handoff_kiosk.h"
#include "playback_reset.h"
#include "settings_persistence.h"
#include "../platform/drm_display.h"
#include "../platform/input_manager.h"
#include "../ui/controller_wizard.h"
#include "../ui/pairing_screen.h"
#include "../ui/pairing_screen_renderer.h"
#include "../ui/settings_menu.h"
#include "../ui/toast.h"
#include "../ui/virtual_keyboard.h"
#include "../utils/config.h"
#include "../utils/logger.h"
#ifdef MEDIA_BROWSER_ENABLED
#include "../media_browser/mb_entry_gate.h"
#endif

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace app {

using platform::InputAction;

bool handle_menu_button(SettingsInputContext& ctx, MenuButtonHold& menu_hold,
                        const platform::InputEvent& ev) {
    AppState& state = ctx.state;
    ui::SettingsMenuManager& settings_menu = ctx.settings_menu;
    Controller& controller = ctx.controller;

    // Handle Menu button hold logic
    if (ev.action == InputAction::SETTINGS_MENU) {
        if (ev.pressed) {
            menu_hold.press(std::chrono::steady_clock::now());
            state.show_volume_slider = false; // Don't show immediately
        } else {
            state.show_volume_slider = false; // Hide immediately

            // Only toggle menu if we didn't change volume AND it was a
            // short press (MenuButtonHold::release).
            switch (menu_hold.release(std::chrono::steady_clock::now())) {
                case MenuButtonHold::Release::ToggleSettings: {
                    // BTN4 toggle gating:
                    //  - Inside Media Browser: never toggle the kiosk
                    //    Settings overlay. MB owns the screen and has
                    //    its own internal dispatcher (back/exit modal).
                    //  - Outside MB (main kiosk playlist):
                    //    * always allow CLOSE (never trap user in an
                    //      open Settings overlay)
                    //    * allow OPEN when UI is available, i.e. no
                    //      playback active OR the user's
                    //      `ui_visible_when_playing` preference is on.
                    //      That preserves the original kiosk behavior:
                    //      operators who ticked "show UI during
                    //      playback" can still pop into Settings while
                    //      a playlist video is rolling.
                    bool in_mb = false;
#ifdef MEDIA_BROWSER_ENABLED
                    in_mb = (state.current_screen == app::AppScreen::MediaBrowser);
#endif
                    if (!in_mb) {
                        const bool already_open =
                            settings_menu.is_active() || settings_menu.is_opening();
                        const bool ui_available =
                            !state.video_active || state.ui_visible_when_playing;
                        if (already_open || ui_available) {
                            settings_menu.toggle();
                        }
                    }
                    break;
                }
                case MenuButtonHold::Release::SaveVolume:
                    // Volume was changed, save settings now
                    app::SettingsPersistence::save_settings(state);
                    break;
                case MenuButtonHold::Release::Nothing:
                    break;
            }
        }
        return true;
    }

    // If Menu button is held, hijack Rotate/Up/Down for volume
    if (menu_hold.button_held) {
        if (ev.action == InputAction::ROTATE ||
            ev.action == InputAction::ROTATE_VERTICAL) {
            // 5% increments, clamped; the vertical axis is inverted
            // (Up = -1 -> Volume Up). See volume_after_hold_step.
            state.master_volume = volume_after_hold_step(
                state.master_volume, ev.delta,
                ev.action == InputAction::ROTATE_VERTICAL);

            // Apply volume
            controller.set_system_volume(state.master_volume);

            // Show slider immediately on interaction and mark as changed
            state.show_volume_slider = true;
            menu_hold.volume_changed_while_held = true;
        }
        return true; // Consume event
    }
    return false;
}

bool dispatch_overlay_input(SettingsInputContext& ctx,
                            const platform::InputEvent& ev) {
    AppState& state = ctx.state;
    ui::SettingsMenuManager& settings_menu = ctx.settings_menu;
    ui::VirtualKeyboard& keyboard = ctx.keyboard;
    Controller& controller = ctx.controller;
    platform::InputManager& input = ctx.input;
    // THE game list (a reference to the AppState member — see main()).
    const std::vector<Playlist>& game_playlists = state.game_playlists;

    // Route input
    if (keyboard.is_active()) {
        // Handle navigation (axis/dpad) or button presses
        bool is_navigation = (ev.action == InputAction::ROTATE || ev.action == InputAction::ROTATE_VERTICAL);

        if (ev.pressed || is_navigation) {
            switch (ev.action) {
                case InputAction::ROTATE_VERTICAL:
                    if (ev.delta < 0) keyboard.navigate_up();
                    else if (ev.delta > 0) keyboard.navigate_down();
                    break;
                case InputAction::ROTATE:
                    if (ev.delta < 0) keyboard.navigate_left();
                    else if (ev.delta > 0) keyboard.navigate_right();
                    break;
                case InputAction::SELECT: keyboard.select(); break;
                case InputAction::PREV: // Backspace shortcut
                case InputAction::SEEK_LEFT:
                    keyboard.backspace();
                    break;
                case InputAction::NEXT: // Space shortcut
                case InputAction::SEEK_RIGHT:
                    keyboard.space();
                    break;
                case InputAction::QUIT: keyboard.close(); break;
                default: break;
            }
        }
        return true; // Consume event if keyboard is active
    } else if (settings_menu.is_active() || settings_menu.is_opening() || settings_menu.is_closing()) {
        // Settings Menu Input
        // Handle game browser navigation and selection
        if (settings_menu.is_game_browser_active()) {
            switch (ev.action) {
                case InputAction::ROTATE:
                case InputAction::ROTATE_VERTICAL: {
                    // Navigate game browser
                    int game_playlist_count = static_cast<int>(game_playlists.size());
                    int games_in_current_playlist = 0;
                    if (settings_menu.is_viewing_games_in_playlist()) {
                        int playlist_idx = settings_menu.get_current_game_playlist_index();
                        if (playlist_idx >= 0 && playlist_idx < game_playlist_count) {
                            games_in_current_playlist = static_cast<int>(game_playlists[playlist_idx].items.size());
                        }
                    }

                    settings_menu.navigate(ev.delta, game_playlist_count, games_in_current_playlist);

                    break;
                }

                case InputAction::SELECT: {
                    if (!ev.pressed) break; // Only trigger on press

                    std::cout << "SELECT pressed - checking menu state..." << std::endl;
                    std::cout << "  is_viewing_games_in_playlist: " << (settings_menu.is_viewing_games_in_playlist() ? "YES" : "NO") << std::endl;
                    std::cout << "  is_game_browser_active: " << (settings_menu.is_game_browser_active() ? "YES" : "NO") << std::endl;
                    std::cout << "  is_active: " << (settings_menu.is_active() ? "YES" : "NO") << std::endl;

                    if (settings_menu.is_viewing_games_in_playlist()) {
                        // Launch selected game or go back
                        int playlist_idx = settings_menu.get_current_game_playlist_index();
                        int game_idx = settings_menu.get_selected_game_in_playlist();

                        std::cout << "Game browser SELECT: playlist_idx=" << playlist_idx << ", game_idx=" << game_idx << std::endl;

                        if (playlist_idx >= 0 && playlist_idx < static_cast<int>(game_playlists.size())) {
                            const auto& playlist = game_playlists[playlist_idx];

                            // "Back" is the row after the last game
                            // (classify_game_list_select).
                            switch (classify_game_list_select(
                                        game_idx, static_cast<int>(playlist.items.size()))) {
                                case GameListSelect::Back:
                                    std::cout << "Back button selected - returning to playlist list" << std::endl;
                                    settings_menu.exit_game_list();
                                    break;
                                case GameListSelect::Launch:
                                    // Loading plate, launch, outcome, then
                                    // force-close Settings. Blocks for the
                                    // whole game session; the bracketing
                                    // runs in the controller's session
                                    // hooks. See app/game_handoff_kiosk.h.
                                    app::launch_game_from_browser(
                                        controller, state, settings_menu,
                                        ctx.gfx, ctx.mode, playlist,
                                        game_idx, ctx.playlist_directory);
                                    break;
                                case GameListSelect::Invalid:
                                    std::cout << "Invalid game index: " << game_idx << " (max: " << playlist.items.size() << ")" << std::endl;
                                    break;
                            }
                        } else {
                            std::cout << "Invalid playlist index: " << playlist_idx << " (max: " << game_playlists.size() << ")" << std::endl;
                        }
                    } else {
                        // Enter selected playlist or go back
                        int selected_playlist = settings_menu.get_game_browser_selected();

                        // "Back" is the row after the last playlist
                        // (classify_game_browser_select).
                        switch (classify_game_browser_select(
                                    selected_playlist,
                                    static_cast<int>(game_playlists.size()))) {
                            case GameBrowserSelect::ExitBrowser:
                                settings_menu.exit_game_browser();
                                break;
                            case GameBrowserSelect::EnterPlaylist:
                                settings_menu.enter_game_list(selected_playlist);
                                break;
                            case GameBrowserSelect::Nothing:
                                break;
                        }
                    }
                    break;
                }
                default:
                    break;
            }
            return true; // Skip normal menu handling when in game browser
        }

        // ── Controller Setup wizard: intercept everything ─────────────
        //
        // Every input surface EXCEPT the pad being configured lands
        // here (that pad is diverted to raw events by
        // set_raw_capture). on_action() owns cancel from every phase,
        // so the wizard is always escapable from the box buttons, the
        // rotary, the phone remote, or a keyboard.
        //
        // NOT from a gamepad — not even a working one. set_raw_capture
        // diverts EVERY real joystick (the phone-remote uinput device
        // is the sole exception), so no pad can produce an InputAction
        // while the wizard is up. See the header comment on
        // ui::ControllerWizard, which states the same guarantee
        // correctly.
        if (settings_menu.is_controller_wizard_active()) {
            auto* wiz = settings_menu.controller_wizard();
            if (wiz) {
                // Return value ignored on purpose: consumed or not,
                // nothing else may act on this event while the wizard
                // owns the screen.
                wiz->on_action(ev);
                // Close immediately rather than waiting for next
                // frame's pump, so the overlay can't paint one extra
                // frame after the user cancelled. The pump's
                // active→inactive edge still fires the overlay reload.
                if (!wiz->is_active()) settings_menu.close_controller_wizard();
            } else {
                settings_menu.close_controller_wizard();
            }
            return true;  // eat all other inputs while the wizard is up
        }
        // ─────────────────────────────────────────────────────────────

        // ── Phone Remote pairing screen: intercept navigation/forget ──
        if (settings_menu.is_pairing_screen_active()) {
            auto* ps = settings_menu.pairing_screen();
            if (ps) {
                // Shared mtime-based cache — same data the renderer
                // sees, so no post-forget divergence.
                const auto& ps_cached_devices = ui::paired_devices_cached(
                    config::get_data_path() + "/paired_remotes.json");
                if ((ev.action == InputAction::ROTATE || ev.action == InputAction::ROTATE_VERTICAL) && ev.delta != 0) {
                    if (ev.delta < 0)
                        ps->select_prev_device(static_cast<int>(ps_cached_devices.size()));
                    else
                        ps->select_next_device(static_cast<int>(ps_cached_devices.size()));
                    return true; // consumed
                } else if (ev.action == InputAction::PLAY_PAUSE && ev.pressed) {
                    std::vector<std::string> ids;
                    for (const auto& d : ps_cached_devices) ids.push_back(d.id);
                    ps->forget_selected_device(ids);
                    // No manual cache invalidation needed — Flask
                    // rewrites paired_remotes.json on its next
                    // broadcaster tick (~200 ms), and the cache
                    // mtime-checks every call.
                    return true; // consumed
                } else if (ev.action == InputAction::QUIT && ev.pressed) {
                    ps->close();
                    settings_menu.close_pairing_screen();
                    return true; // consumed
                } else if (ev.action == InputAction::SETTINGS_MENU && ev.pressed) {
                    // BTN4 (black) — close the pairing screen and return to settings.
                    ps->close();
                    settings_menu.close_pairing_screen();
                    return true; // consumed
                }
            }
            return true; // eat all other inputs while pairing screen is up
        }
        // ─────────────────────────────────────────────────────────────

        // Normal settings menu handling
        switch (ev.action) {
            case InputAction::ROTATE:
            case InputAction::ROTATE_VERTICAL:
                settings_menu.navigate(ev.delta);
                break;

            case InputAction::SELECT: {
                if (!ev.pressed) break; // Only trigger on press

                ui::MenuSection section = settings_menu.select_current();
                if (section == ui::MenuSection::VIDEO_GAMES) {
                    // Go directly to game browser (skip submenu)
                    settings_menu.enter_game_browser();
                } else if (section == ui::MenuSection::DISPLAY) {
                    settings_menu.enter_submenu(ui::MenuSection::DISPLAY);
                } else if (section == ui::MenuSection::AUDIO) {
                    settings_menu.enter_submenu(ui::MenuSection::AUDIO);
                } else if (section == ui::MenuSection::SYSTEM) {
                    settings_menu.enter_submenu(ui::MenuSection::SYSTEM);
                } else if (section == ui::MenuSection::WIFI) {
                    settings_menu.enter_submenu(ui::MenuSection::WIFI);
                } else if (section == ui::MenuSection::WIFI_NETWORKS) {
                    settings_menu.enter_submenu(ui::MenuSection::WIFI_NETWORKS);
                // MenuSection::INFO is deliberately NOT dispatched:
                // info-only rows ("Movies (configure VPN)" etc.) fall
                // through to the silent no-op below. The Content
                // Manager Info screen it used to open was merged into
                // the "Connect a Device" pairing screen.
                } else if (section == ui::MenuSection::PHONE_REMOTE) {
                    settings_menu.open_pairing_screen();
                } else if (section == ui::MenuSection::CONTROLLER_SETUP) {
                    settings_menu.open_controller_wizard(&input);
                } else if (section == ui::MenuSection::BROWSE_GAMES) {
                    // Enter game browser
                    settings_menu.enter_game_browser();
#ifdef MEDIA_BROWSER_ENABLED
                } else if (section == ui::MenuSection::MEDIA_BROWSER_NEEDS_DISPLAY) {
                    // The "Movies (needs Modern TV display)" row.
                    // Non-actionable by design, but unlike the INFO
                    // rows SELECT explains itself instead of
                    // silently doing nothing.
                    ui::Toast::show(media_browser::kMoviesNeedsModernTvToast);
                } else if (section == ui::MenuSection::MEDIA_BROWSER &&
                           !media_browser::display_supports_media_browser(
                               state.display_settings.mode ==
                               app::DisplayMode::CRT_NATIVE)) {
                    // Stale-row race, checked at the moment of entry:
                    // the actionable "Movies" row is built by open()
                    // when the mode was MB-capable, but the mode can
                    // change while the menu is still open — the
                    // Display submenu's mode toggle followed by
                    // exit_submenu() (which does NOT rebuild the
                    // top-level rows), or the web-admin settings
                    // restore poke. The logical canvas is already
                    // 640x480 by the time SELECT lands (the
                    // mode-change block applies it immediately), so
                    // entering would open the MB screens with their
                    // panels off the canvas. Same toast as the
                    // blocked row.
                    ui::Toast::show(media_browser::kMoviesNeedsModernTvToast);
                } else if (section == ui::MenuSection::MEDIA_BROWSER) {
                    // Close settings menu and transition to the
                    // Media Browser screen.
                    //
                    // Cleanly tear down whatever the main UI was
                    // playing first. Without this stop(), the
                    // playlist video keeps running, the GStreamer
                    // pipeline stays in PLAYING state, and frames
                    // continue to render underneath the Media
                    // Browser overlay (mb_fill_background isn't
                    // fully opaque). The user perceives this as
                    // the previous video "showing through" their
                    // movie browse session. Forcing video_active
                    // to false in the same frame avoids a one-
                    // frame race where the next render still
                    // thinks video is active before
                    // controller.update_state() catches up.
                    controller.stop();
                    // CRITICAL: also clear the playing-item indexes
                    // and any in-flight UI fade. The Renderer's
                    // is_transitioning logic (current_item_index >= 0
                    // && !video_active) skips the ENTIRE main-UI
                    // render, and a stale is_fading with a hidden
                    // target zeroes the UI alpha — either one leaves
                    // the main menu permanently BLANK after exiting
                    // the Media Browser. Not stop_to_menu(): the MB
                    // takes the screen, so the playlist UI is parked
                    // hidden — see reset_main_ui_for_media_browser.
                    app::reset_main_ui_for_media_browser(state);
                    // Clear the published now-playing/playlist info
                    // for the phone remote. Controller::update_state's
                    // stop-clear deliberately skips MB sessions (the
                    // MB PlaybackScreen owns these fields there), so
                    // without this the stopped playlist item's title
                    // would ride along in kiosk_status.json for the
                    // whole browse session.
                    app::clear_now_playing(state);
                    settings_menu.close();
                    // AppScreen -> MediaBrowser, entered on the
                    // Browse landing screen.
                    ctx.enter_media_browser();
                } else if (section == ui::MenuSection::HIDE_MEDIA_BROWSER) {
                    // Re-lock the Media Browser. Both the "Movies" and
                    // "Hide Movies feature" rows will disappear next
                    // time settings opens (open() rebuilds menu_items_
                    // based on media_browser_unlocked). User must
                    // re-enter the secret sequence to unlock again.
                    // This is an intermediate control — final home is
                    // the Movies Settings screen (Task 23).
                    state.media_browser_unlocked = false;
                    app::SettingsPersistence::save_settings(state);
                    settings_menu.close();
                    ui::Toast::show("Movie section hidden");
                    LOG_INFO("Media Browser: feature re-locked via settings menu");
#endif
                } else if (section == ui::MenuSection::BACK) {
                    if (settings_menu.get_current_submenu() != ui::MenuSection::BACK) {
                        settings_menu.exit_submenu();
                    } else {
                        settings_menu.close();
                    }
                }
                break;
            }

            case InputAction::QUIT:
                settings_menu.close();
                break;

            default:
                break;
        }
        return true;  // Skip normal input handling when menu is active
    }

    return false;
}

}  // namespace app
