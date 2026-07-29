#pragma once

#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <memory>

namespace app {
    struct AppState;
}

namespace ui {
    class PairingScreen;      // forward declaration — avoid circular include
    class ControllerWizard;   // ditto (controller_wizard.h pulls in platform/)
}

namespace platform {
    class InputManager;
}

namespace ui {

enum class MenuSection {
    VIDEO_GAMES,
    DISPLAY,
    AUDIO,
    SYSTEM,
    WIFI,
    WIFI_NETWORKS,
    INFO,
    PHONE_REMOTE,      // NEW: drills into the pairing-screen view, not a submenu
    CONTROLLER_SETUP,  // Drills into the Controller Setup wizard, not a submenu
    BACK,
    BROWSE_GAMES,
    TOGGLE_PLAYLIST_LOOP,
    TOGGLE_SHUFFLE,
    TOGGLE_DISPLAY_MODE,
    TOGGLE_BEZEL,
    CHANGE_RESOLUTION,
    CYCLE_BEZEL_STYLE,
    CYCLE_BEZEL,
    CYCLE_SCANLINES,
    CYCLE_WARMTH,
    CYCLE_BLOOM,
    CYCLE_GLOW,
    CYCLE_RGB_MASK,    // Renamed from CYCLE_PHOSPHOR_MASK to match the UI label "RGB Mask"
    CYCLE_INTERLACING,
    CYCLE_FLICKER,
    TOGGLE_ENHANCED_CRT,   // Classic ↔ Enhanced CRT pipeline (Phase 1+ shader rework)
    DOWNLOAD_CORES
#ifdef MEDIA_BROWSER_ENABLED
    ,
    MEDIA_BROWSER,   // Opens Media Browser screen (only shown when unlocked via secret sequence)
    HIDE_MEDIA_BROWSER   // Re-locks the Media Browser (only shown when unlocked)
#endif
};

struct MenuItem {
    std::string label;
    MenuSection section;
    std::string sublabel;
    std::function<void()> action;
    
    MenuItem(const std::string& l, MenuSection s = MenuSection::BACK, 
             const std::string& sub = "", std::function<void()> a = nullptr)
        : label(l), section(s), sublabel(sub), action(a) {}
};

class SettingsMenuManager {
public:
    SettingsMenuManager(app::AppState* state = nullptr);
    // Declared (not defaulted here) and defined in the .cpp: this class owns
    // unique_ptrs to forward-declared types (PairingScreen, ControllerWizard),
    // and an implicit destructor would have to be instantiated in every TU
    // that destroys a manager — each of which would then need the full
    // definition of both. Out-of-lining it keeps the forward declarations
    // honest.
    ~SettingsMenuManager();

    void set_app_state(app::AppState* state) { app_state_ = state; }
    
    void update();
    void toggle();
    void open();
    void close();
    // Teleport to fully-closed state without playing the close animation.
    // Required for paths where the renderer that would normally advance
    // the close animation isn't running (e.g. while the Media Browser
    // owns the screen — main.cpp skips ui_renderer.render(state) there,
    // so close()'s animation timer never advances and is_closing_ stays
    // stuck true forever).
    void force_close();
    
    bool is_active() const { return active_; }
    bool is_opening() const { return is_opening_; }
    bool is_closing() const { return is_closing_; }
    
    float get_animation_progress() const;
    
    void navigate(int delta, int game_playlists_count = 0, int games_in_current_playlist = 0);
    MenuSection select_current();
    
    void enter_submenu(MenuSection section);
    void exit_submenu();
    void rebuild_current_submenu();
    
    void enter_game_browser();
    void exit_game_browser();
    void enter_game_list(int playlist_index);
    void exit_game_list();
    
    // State accessors
    int get_selected_index() const { return selected_index_; }
    int get_scroll_offset() const { return scroll_offset_; }

    // Renderer publishes the actual max-visible row count it's using
    // (computed per-frame from height_ + item_height). Without this,
    // move_selection() used a hardcoded 7 — sized for CRT — which
    // forced scroll_offset to 1 the moment the user navigated to row
    // index 7+ in Modern TV mode, even when the renderer could
    // comfortably show all rows on screen. The result was visible as
    // "select Back, top item disappears."
    void set_max_visible_items(int n) { max_visible_items_ = std::max(1, n); }
    int get_max_visible_items() const { return max_visible_items_; }
    MenuSection get_current_submenu() const { return current_submenu_; }
    bool is_game_browser_active() const { return game_browser_active_; }
    bool is_viewing_games_in_playlist() const { return viewing_games_in_playlist_; }
    int get_game_browser_selected() const { return game_browser_selected_; }
    int get_current_game_playlist_index() const { return current_game_playlist_index_; }
    int get_selected_game_in_playlist() const { return selected_game_in_playlist_; }
    
    // Helper to check if "Back" is selected in game browser
    bool is_game_browser_back_selected() const;

    // Label of whatever row is currently highlighted, across menu levels
    // (top menu, submenu). Used to mirror cursor state into AppState /
    // kiosk_status.json for closed-loop test automation. Returns "" when
    // nothing sensible is highlighted.
    std::string get_current_highlighted_label() const;

    const std::vector<MenuItem>& get_menu_items() const { return menu_items_; }
    const std::vector<MenuItem>& get_submenu_items() const { return submenu_items_; }

    // Phone Remote / PairingScreen
    PairingScreen* pairing_screen();
    void open_pairing_screen();
    void close_pairing_screen();
    bool is_pairing_screen_active() const { return pairing_active_; }

    // Controller Setup wizard. Same shape as the pairing screen above: a
    // full-screen view that replaces the settings panel while it is up.
    // close_controller_wizard() is idempotent and is also called from
    // close()/force_close(), so no path can dismiss the settings menu while
    // leaving InputManager stuck in raw-capture mode (which would leave every
    // gamepad dead until a kiosk restart).
    ControllerWizard* controller_wizard();
    void open_controller_wizard(platform::InputManager* input);
    void close_controller_wizard();
    bool is_controller_wizard_active() const { return wizard_active_; }

private:
    app::AppState* app_state_;
    mutable bool active_;
    int selected_index_;
    std::chrono::steady_clock::time_point animation_start_;
    float animation_duration_;
    mutable bool is_opening_;
    mutable bool is_closing_;
    int scroll_offset_;
    // State for async operations
    bool was_scanning_;
    bool was_connecting_;
    bool wifi_disconnect_confirm_ = false;
    // Default to 7 (CRT layout); renderer overwrites this each frame
    // once it knows the actual viewport height.
    int max_visible_items_ = 7;
    // INFO submenu auto-refresh — the Content Manager page shows USB vs.
    // Wi-Fi URL based on which interface has carrier. We rebuild it
    // periodically while it's open so the QR code reflects state changes
    // (e.g., user unplugs the USB-C cable while sitting on this screen).
    std::chrono::steady_clock::time_point last_info_refresh_{};
    // Wi-Fi networks submenu live refresh while a scan streams in
    // partial results (2 Hz — see update()).
    std::chrono::steady_clock::time_point last_scan_refresh_{};
    
    MenuSection current_submenu_;
    std::vector<MenuItem> menu_items_;
    std::vector<MenuItem> submenu_items_;
    
    // Game browser state
    bool game_browser_active_;
    int game_browser_selected_;
    bool viewing_games_in_playlist_;
    int current_game_playlist_index_;
    int selected_game_in_playlist_;
    
    std::vector<MenuItem> build_games_submenu();
    std::vector<MenuItem> build_display_submenu();
    std::vector<MenuItem> build_audio_submenu();
    std::vector<MenuItem> build_system_submenu();
    std::vector<MenuItem> build_wifi_submenu();
    std::vector<MenuItem> build_wifi_networks_submenu();
    std::vector<MenuItem> build_info_submenu();
    std::string intensity_to_label(float intensity);

    std::unique_ptr<PairingScreen> pairing_screen_;
    bool pairing_active_ = false;

    std::unique_ptr<ControllerWizard> controller_wizard_;
    bool wizard_active_ = false;
};

} // namespace ui

