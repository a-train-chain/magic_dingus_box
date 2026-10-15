#pragma once

// MediaBrowserHost — the kiosk side of the Media Browser.
//
// Owns the MB screens, their modals and the dispatcher state (which screen
// is active), and runs every per-frame MB step main.cpp's loop used to
// inline: the unlock sequence, input dispatch + screen hand-offs, the
// render block, the watch-state checkpoints, and THE exit back to the
// kiosk MainMenu. main() still owns every long-lived service object
// (Radarr/Sonarr/TMDB/Prowlarr/qBit clients, WatchStore, the quiet-mode
// executors, Controller, AppState, Renderer, EGL) and passes references
// in; the host never outlives them because main declares it after them.
//
// main.cpp calls into the host at the same points in the frame where the
// inline code used to run, so the per-frame order is unchanged:
//   input   feed_unlock_sequence() -> handle_input()
//   gate    current_screen()/pump_artwork()/wants_static_frame()/
//           add_redraw_signature()
//   render  render()
//   after   tick_watch_state() -> on_external_seek()
// and flush_before_shutdown() on the way out.
//
// Kiosk-only (GL, EGL, Renderer): compiled via KIOSK_MEDIA_BROWSER_SOURCES
// and only under MEDIA_BROWSER_ENABLED. The pure hand-off decisions live
// in media_browser/ui/mb_transition.h, which IS unit-tested on the Mac.

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "platform/input_manager.h"
#include "platform/sequence_detector.h"
#include "media_browser/ui/mb_screen.h"
#include "media_browser/ui/browse_screen.h"
#include "media_browser/ui/search_screen.h"
#include "media_browser/ui/detail_screen.h"
#include "media_browser/ui/series_detail_screen.h"
#include "media_browser/ui/queue_screen.h"
#include "media_browser/ui/library_screen.h"
#include "media_browser/ui/mb_settings_screen.h"
#include "media_browser/ui/playback_screen.h"
#include "media_browser/ui/release_picker_screen.h"
#include "media_browser/ui/mb_exit_modal.h"
#include "media_browser/ui/stall_prompt_modal.h"
#include "media_browser/qbittorrent/download_watchdog.h"

namespace ui { class Renderer; class VirtualKeyboard; }
namespace platform { class EglContext; class GpioManager; }
namespace app { struct AppState; class Controller; class MovieQuietMode;
                class ContentSignature; }

namespace media_browser {

class RadarrClient;
class SonarrClient;
class TmdbClient;
class ProwlarrClient;
class QbittorrentClient;
namespace library { class WatchStore; }

class MediaBrowserHost {
public:
    // Everything the MB screens and dispatcher borrow from main(). All
    // references must outlive the host.
    struct Deps {
        RadarrClient& radarr;
        SonarrClient& sonarr;
        // False when `sonarr` is the SonarrMockClient fallback (no API
        // key): Browse/SeriesDetail/Queue/Library must not present the
        // mock's fixtures as real TV.
        bool sonarr_configured;
        TmdbClient& tmdb;
        ProwlarrClient* prowlarr;  // nullable: AVAILABILITY readout off
        QbittorrentClient& qbit;
        library::WatchStore& watch_store;
        app::MovieQuietMode& movie_quiet_mode;
        app::Controller& controller;
        app::AppState& state;
        // Pipeline-error probe for PlaybackScreen (errors end MB playback
        // with a toast, never as natural EOS).
        std::function<bool()> player_error_probe;
        ::ui::Renderer& renderer;
        platform::EglContext& egl;
    };

    explicit MediaBrowserHost(Deps deps);
    MediaBrowserHost(const MediaBrowserHost&) = delete;
    MediaBrowserHost& operator=(const MediaBrowserHost&) = delete;

    // Settings-menu "Movies" SELECT: flip AppScreen to MediaBrowser and
    // enter on the Browse landing screen. The caller has already stopped
    // the main UI's playback and closed the settings menu.
    void enter_from_settings();

    // Secret unlock sequence (BTN1+BTN3 chord -> BTN2 x3 -> rotary click).
    // Runs every frame, MB or not; matched events are consumed. The caller
    // skips it while the Controller Setup wizard is up.
    void feed_unlock_sequence(std::vector<platform::InputEvent>& input_events,
                              platform::GpioManager& gpio);

    // The MB Search screen's on-screen keyboard while it is the phone
    // remote's text target, else nullptr.
    ::ui::VirtualKeyboard* search_text_keyboard();

    // Display-mode eviction + the MB dispatcher. While the MB owns the
    // screen this consumes ALL of input_events (the main UI's loop then
    // sees an empty queue). fb_w/fb_h: physical framebuffer, for the
    // black clear between screens.
    void handle_input(std::vector<platform::InputEvent>& input_events,
                      int fb_w, int fb_h);

    // ── Redraw gate inputs (app/redraw_gate.h) ──────────────────────────
    media_browser::ui::Screen current_screen() const { return current_mb_screen_; }
    // Poster uploads; must run on skipped frames too (counted into the
    // content signature so an upload draws the frame that shows it).
    void pump_artwork();
    // The active screen opted out of continuous drawing and no dispatcher
    // modal is shown.
    bool wants_static_frame() const;
    void add_redraw_signature(app::ContentSignature& sig, bool screen_static) const;

    // AppScreen is MediaBrowser AND the Playback screen is active.
    bool in_playback() const;

    // Draw the active screen, modals, Marquee CRT look and wood frame.
    // Only call while AppScreen is MediaBrowser, on drawn frames.
    void render(int fb_w, int fb_h);

    // 30 s resume checkpoints + EOS watched marking. Every frame.
    void tick_watch_state();

    // A phone-remote tap-to-seek landed on the shared pipeline.
    void on_external_seek();

    // SIGTERM / systemctl stop while a movie plays: persist the position
    // and run Playback's leave() so the contention guard resumes.
    void flush_before_shutdown();

private:
    void exit_media_browser(std::vector<platform::InputEvent>& input_events);
    void transition_to(media_browser::ui::Screen next, int fb_w, int fb_h);

    app::AppState& state_;
    library::WatchStore& watch_store_;
    ::ui::Renderer& ui_renderer_;
    platform::EglContext& egl_;

    // Media Browser secret-unlock sequence detector. Fed each frame from
    // GPIO events; fires Toast + sets state.media_browser_unlocked when
    // the full chord → BTN2 × 3 → rotary-click sequence is entered.
    platform::SequenceDetector seq_detector_;
    // Edge detector for the level-triggered BTN1+BTN3 chord.
    bool chord_was_active_ = false;

    // Declaration order IS construction order — it matches the order
    // main.cpp built these in before the host existed.
    media_browser::ui::BrowseScreen        mb_browse_;
    media_browser::ui::SearchScreen        mb_search_;
    media_browser::ui::DetailScreen        mb_detail_;
    media_browser::ui::SeriesDetailScreen  mb_series_detail_;
    media_browser::ui::QueueScreen         mb_queue_;
    media_browser::ui::LibraryScreen       mb_library_;
    media_browser::ui::PlaybackScreen      mb_playback_;
    media_browser::ui::ReleasePickerScreen mb_release_picker_;
    media_browser::ui::MbSettingsScreen    mb_mb_settings_;

    media_browser::ui::Screen   current_mb_screen_ = media_browser::ui::Screen::Browse;
    media_browser::ui::MbScreen* active_mb_screen_ = nullptr;

    // Global "Exit Marquee?" confirm modal; intercepts BTN2 (PLAY_PAUSE)
    // before any MB screen sees it. Task 7.
    media_browser::ui::ExitModal mb_exit_modal_;

    // Background watchdog for stalled downloads + the modal that surfaces
    // its events (Task 16; the per-frame tick is disabled — see
    // handle_input).
    media_browser::DownloadWatchdog     mb_watchdog_;
    media_browser::ui::StallPromptModal mb_stall_modal_;

    // BTN4 long-press detection inside the MB.
    std::optional<std::chrono::steady_clock::time_point> mb_btn4_pressed_at_;

    // Poster textures uploaded so far (redraw gate: an upload draws).
    uint64_t mb_artwork_uploads_ = 0;

    // Last 30 s watch-position checkpoint; re-armed every frame outside
    // Playback, so a session always waits a full interval before its
    // first write.
    std::chrono::steady_clock::time_point last_checkpoint_;
};

}  // namespace media_browser
