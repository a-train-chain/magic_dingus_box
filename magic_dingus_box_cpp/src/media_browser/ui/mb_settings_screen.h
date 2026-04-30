#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "app/app_state.h"
#include "media_browser/radarr/radarr_types.h"
#include "media_browser/ui/mb_screen.h"

namespace media_browser { class RadarrClient; }

namespace media_browser::ui {

// Task 23: the Movies Settings screen — replaces the Task 17 stub.
//
// A scrollable vertical list of ~11 settings rows (spec §9.6). Each row
// renders according to its RowKind:
//   Service status    : three colored dots (Radarr / Prowlarr / qBittorrent)
//   Quality profile   : cycles through RadarrClient::get_quality_profiles()
//   Min seeders       : int slider (0..20)       [MVP: not persisted]
//   Storage path      : root folder + free-space readout
//   Low-space thresh  : int slider (10..200 GB)  [MVP: not persisted]
//   Max concurrent dl : int slider (1..5)        [MVP: not persisted]
//   Indexer toggles   : scrollable list of Prowlarr indexers, SELECT toggles
//   Retry all failed  : button (placeholder banner for MVP)
//   Pause all dl      : button (placeholder banner for MVP)
//   Resume all dl     : button (placeholder banner for MVP)
//   Hide Movies       : checkbox that triggers on_hide_feature_ and exits
//   Advanced URL hint : fine-print row at the bottom
//
// The Hide-Movies action is wired via a std::function callback passed from
// main.cpp that flips media_browser_unlocked=false, persists settings, and
// returns to the kiosk main menu (this is the spec-prescribed re-lock path
// per §7.4). When toggled ON, the screen calls the callback AND returns
// Screen::Exit so the dispatcher goes straight to MainMenu.
class MbSettingsScreen : public MbScreen {
public:
    MbSettingsScreen(RadarrClient& radarr,
                     ::app::AppState& state,
                     std::function<void()> on_hide_feature);

    void enter() override;
    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

private:
    enum class RowKind {
        SectionHeader,            // Gold ZenDots heading + steel-blue rule.
        ServiceStatus,
        QualityProfile,
        AutoGrabOnAdd,            // Toggle: add-to-library auto-fires Search.
        PlaybackShowFrame,        // Toggle: show wood-frame overlay during playback.
        MinSeeders,
        StoragePath,
        RefreshLibrary,           // Action: rescans Radarr's root folder.
        LowSpaceThresholdGb,
        MaxConcurrentDownloads,
        IndexerToggles,
        RetryAllFailed,
        PauseAllDownloads,
        ResumeAllDownloads,
        // CRT-overlay intensity rows (v1.6.x). Each cycles through
        // OFF / Low / Medium / High via DisplaySettings::cycle_setting,
        // independently from the kiosk's main-menu CRT settings.
        // Driven by state.display_settings.mb_<name>.
        CrtScanlines,
        CrtWarmth,
        CrtGlow,
        CrtRgbMask,
        CrtBloom,
        CrtInterlacing,
        CrtFlicker,
        HideMovies,
        AdvancedUrlHint,
    };

    struct Row {
        RowKind kind;
        std::string label;
        // How tall this row is in pixels. Most rows are a single line;
        // IndexerToggles expands to show its embedded list.
        float height;
    };

    // Fetched once on enter() (or when reachable state changes) so the
    // render pass doesn't have to block on HTTP every frame.
    struct ServiceHealth {
        bool radarr = false;
        bool prowlarr = false;
        bool qbittorrent = false;
        std::chrono::steady_clock::time_point fetched_at{};
    };

    // Local (non-persisted for MVP) kiosk-side tunables. See design doc §9.6.
    struct LocalPrefs {
        int min_seeders = 2;
        int low_space_threshold_gb = 30;
        int max_concurrent_downloads = 3;
        bool auto_grab_on_add = true;     // Add → auto-fires Radarr search.
    };

    // Prowlarr indexer, lightly parsed out of the `/api/v1/indexer` response.
    struct ProwlarrIndexer {
        int id = 0;
        std::string name;
        bool enabled = true;
    };

    void build_rows();
    void refresh_service_health();
    void refresh_quality_profiles();
    void refresh_root_folders();
    void refresh_indexers();
    void trigger_hide_feature();

    // Banner helpers — the screen renders a transient one-line toast when
    // the user hits a placeholder button. Not a real overlay system; just
    // a ~2 second label that fades when the clock runs out.
    void show_banner(const std::string& msg);

    // Static HTTP helpers used for the non-Radarr services (Prowlarr,
    // qBittorrent). Deliberately minimal — ping + best-effort GET.
    static bool ping_http(const std::string& url);
    static std::string http_get(const std::string& url,
                                const std::string& header = "");

    // Format bytes as a human-readable "12.3 GB free / 500 GB"-style
    // string for the Storage row.
    static std::string format_free_space(int64_t free_bytes,
                                         int64_t total_bytes);

    RadarrClient& radarr_;
    ::app::AppState& state_;     // For mb_playback_show_frame + persistence.
    std::function<void()> on_hide_feature_;

    std::vector<Row> rows_;
    int cursor_ = 0;          // Index into rows_ of the focused row.
    float scroll_y_ = 0.0f;   // Top of the visible list area (pixels).

    // Two-mode rotary navigation (v1.6.x):
    //   - editing_ = false: rotary CW/CCW walks rows vertically. SELECT
    //     enters editing on cycle/slider rows, executes immediately on
    //     action rows, toggles inline on boolean rows.
    //   - editing_ = true:  rotary CW/CCW adjusts the focused row's
    //     value. SELECT exits editing. Vertical D-pad movement also
    //     exits editing.
    // The focused row's visual treatment changes too: navigate-mode
    // shows a gold ◂ marker + gold label; editing-mode draws a 2 px
    // gold rectangle border around the whole row (matches the active-
    // tab vocabulary on the chrome strip and the Marquee design mockup).
    bool editing_ = false;

    ServiceHealth health_;
    LocalPrefs prefs_;

    // Radarr-side state used by multiple rows.
    std::vector<QualityProfile> quality_profiles_;
    int quality_profile_idx_ = 0;  // index into quality_profiles_

    std::vector<RootFolder> root_folders_;

    // Prowlarr indexer state.
    std::vector<ProwlarrIndexer> indexers_;
    int indexer_cursor_ = 0;    // focused indexer when IndexerToggles is focused
    bool prowlarr_api_key_available_ = false;

    // Placeholder banner ("Use qBittorrent web UI", etc.).
    std::string banner_text_;
    std::chrono::steady_clock::time_point banner_until_{};

    bool loaded_once_ = false;
};

}  // namespace media_browser::ui
