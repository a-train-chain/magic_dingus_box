#pragma once

#include <string>
#include <sstream>
#include <vector>
#include <deque>
#include <cstdint>
#include <chrono>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

namespace app {

// Display mode enumeration
enum class DisplayMode {
    CRT_NATIVE,   // Fullscreen with CRT effects
    MODERN_TV     // 4:3 centered with bezel overlay
};

// Audio output enumeration (Pi 4B supports HDMI and 3.5mm headphone jack).
// Routing happens via PulseAudio, NOT the old ALSA `amixer numid=3` method —
// see apply_output() below. AUTO and HDMI both map to the HDMI sink today;
// HEADPHONE maps to the bcm2835 mailbox (3.5mm jack) sink.
enum class AudioOutput {
    AUTO,       // PulseAudio default sink + active stream move (defaults to HDMI)
    HDMI,       // alsa_output.platform-fef00700.hdmi.hdmi-stereo
    HEADPHONE   // alsa_output.platform-fe00b840.mailbox.stereo-fallback
};

// Phone remote screen mode — tracks current UI view for status sync.
// Updated by main loop, settings_menu, and controller.
enum class ScreenMode {
    Playlist,
    Playback,
    Settings,
    RetroArch,
    MediaBrowser
};

inline const char* screen_mode_to_string(ScreenMode m) {
    switch (m) {
        case ScreenMode::Playlist:     return "playlist";
        case ScreenMode::Playback:     return "playback";
        case ScreenMode::Settings:     return "settings";
        case ScreenMode::RetroArch:    return "retroarch";
        case ScreenMode::MediaBrowser: return "media_browser";
    }
    return "unknown";
}

#ifdef MEDIA_BROWSER_ENABLED
// Top-level screen/view the app is currently showing. Historically the
// kiosk has had a single main screen (playlist list + settings overlay);
// the Media Browser V2 feature introduces a separate full-screen view
// unlocked by a secret sequence. Task 17+ will add real screens (search,
// library, queue); for now we just need a way to route the main render
// dispatch between the default view and a placeholder.
enum class AppScreen {
    MainMenu,      // Default playlist + settings overlay view
    MediaBrowser   // V2 — unlocked via secret sequence (placeholder until Task 17)
};
#endif

// Bezel info structure
struct BezelInfo {
    std::string id;
    std::string name;
    std::string file;  // filename (empty for procedural)
    std::string description;
};

} // namespace app

// Forward declaration
namespace ui {
class SettingsMenuManager;
class VirtualKeyboard;
}

#include "../utils/wifi_manager.h"

namespace app {

struct PlaylistItem {
    std::string path;
    std::string source_type;  // "local", "youtube", "emulated_game"
    std::string title;  // Video/song title
    std::string artist;  // Artist name
    std::string emulator_core;  // RetroArch core name (for games)
    std::string emulator_system;  // System name (NES, SNES, etc.)
};

struct Playlist {
    std::string title;
    std::string curator;
    std::string path;
    std::vector<PlaylistItem> items;
    
    // Check if this playlist contains only games (no videos)
    bool is_game_playlist() const {
        if (items.empty()) return false;
        for (const auto& item : items) {
            if (item.source_type != "emulated_game") {
                return false;
            }
        }
        return true;
    }
    
    // Check if this playlist contains any video content
    bool is_video_playlist() const {
        if (items.empty()) return false;
        for (const auto& item : items) {
            if (item.source_type == "local" || item.source_type == "video" || item.source_type == "youtube") {
                return true;
            }
        }
        return false;
    }
};

struct AppState {
    std::vector<Playlist> playlists;  // Video playlists (shown on main screen)
    std::vector<Playlist> game_playlists;  // Game playlists (for menu)
    int selected_index;
    int playlist_scroll_offset = 0;  // Index of first visible playlist in scroll window

    // Display state
    bool ui_visible_when_playing = true; // Tracks whether UI should be visible when video is active

    // Thread-safe flags (accessed from GStreamer callbacks and main thread)
    std::atomic<bool> reset_display{false};       // Signal to reset display state (e.g. after VT switch)
    std::atomic<bool> video_active{false};        // True when video is actively playing
    std::atomic<bool> paused{false};              // True when playback is paused
    std::atomic<bool> is_switching_playlist{false}; // Flag to prevent overlapping playlist switches
    std::atomic<bool> playback_started_{false};   // True when current video has actually started playing

    // Phone remote status sync — derived from current UI mode.
    // Initialized to Playlist; updated by main loop / settings_menu / controller.
    std::atomic<ScreenMode> screen_mode{ScreenMode::Playlist};

    // Phone remote status sync — populated by the main loop / controller.
    // current_playlist_name: display title of the active playlist.
    // current_item_count:    total items in the active playlist.
    // now_playing_title:     human-readable title of the active item.
    // now_playing_subtitle:  secondary label (artist, system name, etc.).
    // now_playing_kind:      "video" | "game" | "media_browser_movie" | "".
    // retroarch_rom_name:    ROM filename (populated when ScreenMode == RetroArch).
    // retroarch_core:        libretro core name (populated when ScreenMode == RetroArch).
    // NOTE: is_paused is not added here — use the existing `paused` atomic<bool>
    // field (same semantic, different name from the plan's draft).
    std::string current_playlist_name;
    int current_item_count = 0;
    std::string now_playing_title;
    std::string now_playing_subtitle;
    std::string now_playing_kind;
    std::string retroarch_rom_name;
    std::string retroarch_core;

    double original_volume;  // Store original volume when video starts (for dimming when UI is visible)
    std::string current_file;
    int current_playlist_index;  // Index of playlist currently playing (-1 if none)
    int current_item_index;  // Index of item currently playing (-1 if none)
    int last_advanced_item_index;  // Last item index we advanced from (to prevent multiple advances)
    double last_advanced_duration;  // Duration of the video we last advanced from (to detect when new video loads)

    // Thread-safe position/duration (written by GStreamer, read by UI)
private:
    mutable std::shared_mutex playback_mutex_;
    double position_ = 0.0;
    double duration_ = 0.0;

public:
    // Thread-safe accessors for position/duration
    void set_position(double pos) {
        std::unique_lock lock(playback_mutex_);
        position_ = pos;
    }

    double get_position() const {
        std::shared_lock lock(playback_mutex_);
        return position_;
    }

    void set_duration(double dur) {
        std::unique_lock lock(playback_mutex_);
        duration_ = dur;
    }

    double get_duration() const {
        std::shared_lock lock(playback_mutex_);
        return duration_;
    }

    // Bulk update for efficiency (single lock)
    void update_playback_state(double pos, double dur) {
        std::unique_lock lock(playback_mutex_);
        position_ = pos;
        duration_ = dur;
    }

    bool loop;
    bool playlist_loop;  // Whether to loop back to start of playlist when finished
    bool shuffle;        // Whether to play videos in random order
    bool master_shuffle_active; // True if Master Shuffle is playing (picks random video from ANY playlist)
    
    // Shuffle queue - ensures all videos play once before reshuffling
    // For regular shuffle: indices within current playlist
    std::vector<int> shuffle_queue;
    int shuffle_queue_position = 0;
    int shuffle_queue_playlist_id = -1;  // Track which playlist the queue is for
    
    // Master shuffle queue - pairs of (playlist_index, item_index) for global shuffle
    std::vector<std::pair<int, int>> master_shuffle_queue;
    int master_shuffle_queue_position = 0;

    // Shuffle history for "Previous" in master shuffle (max 10)
    std::deque<std::pair<int, int>> shuffle_history;

    void push_shuffle_history(int playlist_idx, int item_idx) {
        shuffle_history.push_back({playlist_idx, item_idx});
        if (shuffle_history.size() > 10) shuffle_history.pop_front();
    }

    bool pop_shuffle_history(int& playlist_idx, int& item_idx) {
        if (shuffle_history.empty()) return false;
        auto [p, i] = shuffle_history.back();
        shuffle_history.pop_back();
        playlist_idx = p;
        item_idx = i;
        return true;
    }

    // Master Volume Control
    int master_volume = 100; // 0-100%
    bool show_volume_slider = false;

#ifdef MEDIA_BROWSER_ENABLED
    // Media Browser feature (unlocked via secret sequence)
    // Layer 1: operator-side unlock via secret sequence (persisted to settings.json).
    bool media_browser_unlocked = false;

    // Layer 2: WIREGUARD_PRIVATE_KEY non-empty in services/.env at boot.
    // Re-read on each Settings menu open so an operator who configures
    // VPN via Content Manager doesn't need to restart the kiosk.
    bool media_browser_vpn_configured = false;

    // Layer 3: Radarr /ping reachable on localhost:7878. Owned by
    // VpnHealthMonitor (background thread). Three consecutive failed
    // polls (~30s at 10s interval) flips this true->false; recovery
    // is instant on first successful poll. std::atomic because writes
    // happen on the monitor's worker thread while reads happen on the
    // main thread (toast detection) and settings_menu thread (Movies
    // entry visibility) — matches the convention used by other
    // cross-thread flags in this struct.
    std::atomic<bool> media_browser_vpn_healthy{false};

    // Active top-level screen. Only meaningful when media_browser_unlocked
    // is true; otherwise always MainMenu.
    AppScreen current_screen = AppScreen::MainMenu;
#endif

    // Seek bar overlay state (only visible while actively seeking via rotary encoder)
    bool show_seek_bar = false;
    double seek_bar_timer = 0.0;  // Countdown timer - bar hides when this reaches 0
    
    // UI Visibility Timer
    double ui_visibility_timer = 0.0; // Seconds to keep UI visible

    std::string status_text;

    // Error message overlay (displayed as a banner at bottom of screen)
    std::string error_message;
    std::chrono::steady_clock::time_point error_message_time;

    void set_error(const std::string& msg) {
        error_message = msg;
        error_message_time = std::chrono::steady_clock::now();
    }

    bool has_error_message() const {
        if (error_message.empty()) return false;
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - error_message_time);
        return elapsed.count() < 4;
    }
    
    // Sample mode
    bool sample_mode_active;
    std::vector<double> markers;
    
    // Settings menu (pointer - managed externally)
    ui::SettingsMenuManager* settings_menu;
    
    // Fade animation state (synchronized UI and audio fade)
    std::chrono::steady_clock::time_point fade_start_time;
    std::chrono::milliseconds fade_duration;  // Duration of fade in/out
    bool fade_target_ui_visible;  // Target UI visibility state during fade
    bool is_fading;  // Whether a fade is currently in progress
    
    // Playlist switching state
    std::chrono::steady_clock::time_point playlist_switch_start_time;  // When playlist switch started
    
    // Intro video state
    bool showing_intro_video;  // True when intro video is loaded/playing
    bool intro_ready;  // True when intro video is actually playing (first frame rendered)
    bool intro_complete;  // True after intro has finished (prevents replaying)
    bool intro_fading_out;  // True when intro video is fading out
    std::chrono::steady_clock::time_point intro_fade_out_start_time;  // When intro fade-out started
    
    // Loading state
    std::atomic<bool> is_loading_game{false}; // True when a game is being launched
    // Note: playback_started is now playback_started_ (atomic) - use playback_started_.load()/store()
    
    // Display settings for CRT effects and display mode
    struct DisplaySettings {
        DisplayMode mode = DisplayMode::CRT_NATIVE;
        int bezel_index = 0;  // Index into bezels array (0 = procedural/none)
        
        float scanline_intensity = 0.0f;      // 0.0 - 1.0
        float warmth_intensity = 0.0f;        // 0.0 - 1.0
        float glow_intensity = 0.0f;          // 0.0 - 1.0
        float rgb_mask_intensity = 0.0f;      // 0.0 - 1.0
        float bloom_intensity = 0.0f;         // 0.0 - 1.0
        float interlacing_intensity = 0.0f;   // 0.0 - 1.0
        float flicker_intensity = 0.0f;       // 0.0 - 1.0

        // Enhanced CRT pipeline opt-in flag.
        //
        // When true, the renderer redirects kiosk-side video+UI into an
        // offscreen scene FBO and composites it back to the default
        // framebuffer through a CRT shader that has direct read access
        // to the scene pixels. This is the substrate for higher-fidelity
        // effects (Lottes-style subpixel mask, brightness-modulated
        // Gaussian scanlines, color-matrix warmth, convergence error,
        // halation/bloom) that cannot be implemented as a procedural
        // alpha-blended overlay.
        //
        // Default false: the legacy procedural-overlay path is used,
        // which is bit-for-bit identical to the v1.4.3 behavior. This
        // makes Phase 1 a zero-visible-change refactor — operators only
        // see different visuals after they explicitly opt in. They can
        // also flip back to the classic path live if the enhanced look
        // doesn't suit their CRT.
        //
        // Persisted in config/settings.json as display.enhanced_crt_enabled.
        bool enhanced_crt_enabled = false;

        // Media Browser playback wood-frame toggle (v1.6.x).
        //   true  — wood-frame overlay stays visible during movie playback,
        //           and the video is inset 40 px on every side to fit
        //           cleanly INSIDE the frame's interior.
        //   false — wood-frame is hidden during playback and the movie
        //           fills the whole 1280×720 framebuffer (the same look
        //           operators saw before the Marquee redesign added the
        //           wood-frame asset).
        // Default true matches the visual intent of the Marquee design.
        // The toggle row lives in MovieSettings → Library, alongside
        // Quality preference / Auto-grab on add. Persisted in
        // config/settings.json as display.mb_playback_show_frame.
        bool mb_playback_show_frame = true;

        // Media Browser CRT-overlay intensities — independent from the
        // kiosk's main-menu values above. Each one mirrors the
        // corresponding scanline/warmth/etc. field semantically and is
        // written by the MB Settings → "CRT overlay" rows. Defaults are
        // 0 so a brand-new Pi has zero overlay on the Marquee menus
        // until the operator dials them in. On upgrade, settings_persistence
        // load falls back to inheriting the kiosk values when the
        // mb_* keys aren't present in settings.json yet — that way an
        // upgrading operator doesn't see a sudden visual regression
        // on their MB menus the first frame after install.
        //
        // CRT effects on MB menus go through the legacy procedural
        // overlay path (render_crt_effects), called from main.cpp's
        // MB render block AFTER the per-frame value-swap that
        // temporarily aliases mb_* into the kiosk fields the
        // renderer reads. The swap is per-frame, scoped, and
        // restores the kiosk values right after render — so the
        // home-menu UI never sees the MB values.
        //
        // Persisted in config/settings.json as display.mb_<name>.
        float mb_scanline_intensity   = 0.0f;
        float mb_warmth_intensity     = 0.0f;
        float mb_glow_intensity       = 0.0f;
        float mb_rgb_mask_intensity   = 0.0f;
        float mb_bloom_intensity      = 0.0f;
        float mb_interlacing_intensity = 0.0f;
        float mb_flicker_intensity    = 0.0f;

        // Media Browser Library overlay (v1.6.x). Two enums + two
        // fields capture the operator's chosen sort + filter for the
        // Library grid. The slide-in overlay (BTN4 on LibraryScreen)
        // writes these; LibraryScreen::rebuild_view() reads them on
        // every entry to apply the chosen ordering / subset.
        // Persisted to config/settings.json as
        //   display.mb_library_sort   (string: "recent"/"title"/"year"/"size")
        //   display.mb_library_filter (string: "all"/"unwatched"/"missing_files"/"recently_added")
        // Defaults match what an operator sees on a fresh kiosk.
        enum class MbLibrarySort {
            Recent = 0,
            Title  = 1,
            Year   = 2,
            Size   = 3,
        };
        enum class MbLibraryFilter {
            All            = 0,
            Unwatched      = 1,  // Placeholder until watched-history lands.
            MissingFiles   = 2,
            RecentlyAdded  = 3,
        };
        MbLibrarySort   mb_library_sort   = MbLibrarySort::Recent;
        MbLibraryFilter mb_library_filter = MbLibraryFilter::All;

        // Media Browser Discover tab filters (Popular + TopRated).
        // Persisted to config/settings.json under the display.mb_popular_*
        // and display.mb_toprated_* keys. Defaults represent "no filter
        // applied" — a fresh kiosk shows unfiltered Popular/TopRated results
        // exactly as TMDB returns them. The filter overlay (BrowseScreen)
        // writes these fields; BrowseScreen::fetch_page() reads them to
        // build the TMDB query parameters on every page load.
        enum class MbDecade : uint8_t {
            Any     = 0,
            D2020s  = 1,
            D2010s  = 2,
            D2000s  = 3,
            D1990s  = 4,
            D1980s  = 5,
            D1970s  = 6,
            Classic = 7,  // pre-1970
        };

        enum class MbMinRating : uint8_t {
            Any   = 0,
            Six   = 1,
            Seven = 2,
            Eight = 3,
        };

        enum class MbRuntime : uint8_t {
            Any          = 0,
            Under90      = 1,
            Range90To120 = 2,
            Range2To3Hr  = 3,
            Over3Hr      = 4,
        };

        enum class MbLanguage : uint8_t {
            Any      = 0,
            English  = 1,
            Japanese = 2,
            French   = 3,
            Spanish  = 4,
            Korean   = 5,
            Italian  = 6,
            German   = 7,
            Hindi    = 8,
            Mandarin = 9,
        };

        enum class MbDiscoverSort : uint8_t {
            Popularity     = 0,
            TopRated       = 1,
            MostVoted      = 2,
            RecentRelease  = 3,
        };

        // Popular tab filters
        uint32_t       mb_popular_genre_mask  = 0;
        MbDecade       mb_popular_decade      = MbDecade::Any;
        MbMinRating    mb_popular_min_rating  = MbMinRating::Any;
        MbRuntime      mb_popular_runtime     = MbRuntime::Any;
        MbLanguage     mb_popular_language    = MbLanguage::Any;
        MbDiscoverSort mb_popular_sort        = MbDiscoverSort::Popularity;

        // TopRated tab filters
        uint32_t       mb_toprated_genre_mask  = 0;
        MbDecade       mb_toprated_decade      = MbDecade::Any;
        MbMinRating    mb_toprated_min_rating  = MbMinRating::Any;
        MbRuntime      mb_toprated_runtime     = MbRuntime::Any;
        MbLanguage     mb_toprated_language    = MbLanguage::Any;
        MbDiscoverSort mb_toprated_sort        = MbDiscoverSort::TopRated;

        // Helper to cycle intensity: OFF -> Low (0.25) -> Medium (0.50) -> High (0.75) -> OFF.
        //
        // The four steps are clean fractions (0, 1/4, 1/2, 3/4) so the
        // mental model is "off / quarter / half / three-quarters". The
        // shader's effects are designed to be subtle at 0.25, comfortable
        // at 0.50, and clearly CRT-like at 0.75.
        //
        // Float-fuzz tolerances on each comparison absorb the noise that
        // JSON deserialization introduces (e.g. 0.30 round-trips back as
        // 0.30000001192092896) and also catch operators upgrading from
        // older settings.json files with the previous 0.15/0.30/0.50
        // cycle values — a few clicks rolls them onto the new clean
        // values without leaving them stranded mid-tier.
        //
        // Pair this with intensity_to_label() in settings_menu.cpp; the
        // label thresholds are placed at the midpoints between adjacent
        // cycle values (0.05, 0.375, 0.625) so cycle output always lands
        // in its correct bucket.
        void cycle_setting(float& setting) {
            if (setting <= 0.05f) setting = 0.25f;        // OFF (or near-zero)  → Low
            else if (setting <= 0.32f) setting = 0.50f;  // ≤Low-ish (incl legacy 0.15/0.30) → Medium
            else if (setting <= 0.55f) setting = 0.75f;  // ≤Medium-ish (incl legacy 0.50)   → High
            else setting = 0.0f;                          // High (or above) → OFF
        }
        
        // Cycle display mode: CRT_NATIVE <-> MODERN_TV
        void cycle_mode() {
            mode = (mode == DisplayMode::CRT_NATIVE) 
                   ? DisplayMode::MODERN_TV 
                   : DisplayMode::CRT_NATIVE;
        }
        
        // Get mode name for display
        std::string get_mode_name() const {
            return (mode == DisplayMode::CRT_NATIVE) ? "CRT Native" : "Modern TV";
        }
    } display_settings;
    
    // Audio settings for output selection and game volume
    struct AudioSettings {
        AudioOutput output = AudioOutput::AUTO;  // Default to auto
        float retroarch_volume_offset_db = 0.0f; // -12 to 0, applied to game volume
        
        // Get output name for display
        std::string get_output_name() const {
            switch (output) {
                case AudioOutput::AUTO: return "Auto";
                case AudioOutput::HDMI: return "HDMI";
                case AudioOutput::HEADPHONE: return "Headphone";
                default: return "Auto";
            }
        }
        
        // Apply the audio output setting via PulseAudio
        // Pi 4B uses PulseAudio, not the old ALSA numid=3 method
        // Sink 0 = Headphone (platform-fe00b840.mailbox)
        // Sink 1 = HDMI (platform-fef00700.hdmi)
        void apply_output() const {
            std::string sink_name;

            if (output == AudioOutput::HEADPHONE) {
                sink_name = "alsa_output.platform-fe00b840.mailbox.stereo-fallback";
            } else {
                // HDMI for both AUTO and HDMI modes
                sink_name = "alsa_output.platform-fef00700.hdmi.hdmi-stereo";
            }

            // Set default sink via fork/execlp.
            pid_t pid = fork();
            if (pid == 0) {
                int devnull = open("/dev/null", O_WRONLY);
                if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
                execlp("pactl", "pactl", "set-default-sink", sink_name.c_str(), nullptr);
                _exit(127);
            }
            if (pid > 0) { int s; waitpid(pid, &s, 0); }

            // Move active streams to the new sink. Previously this used
            // /bin/sh -c with sink_name interpolated into a pipeline — safe
            // today (sink_name is a hardcoded constant) but a shell-injection
            // landmine if sink_name ever becomes user-derived. Now: enumerate
            // sink-input IDs via pactl with a pipe(2)-captured stdout, then
            // execlp pactl move-sink-input directly per ID. No shell anywhere.
            int pipefd[2];
            if (pipe(pipefd) != 0) return;
            pid_t list_pid = fork();
            if (list_pid == 0) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                int devnull = open("/dev/null", O_WRONLY);
                if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
                close(pipefd[1]);
                execlp("pactl", "pactl", "list", "short", "sink-inputs", nullptr);
                _exit(127);
            }
            close(pipefd[1]);
            std::string list_out;
            if (list_pid > 0) {
                char buf[256];
                ssize_t n;
                while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
                    list_out.append(buf, static_cast<size_t>(n));
                }
                int s;
                waitpid(list_pid, &s, 0);
            }
            close(pipefd[0]);

            // Each line is "<id>\t<sink>\t..." — extract the leading numeric ID.
            std::istringstream iss(list_out);
            std::string line;
            while (std::getline(iss, line)) {
                std::string id;
                for (char c : line) {
                    if (c >= '0' && c <= '9') id += c;
                    else break;
                }
                if (id.empty()) continue;
                pid_t mv_pid = fork();
                if (mv_pid == 0) {
                    int devnull = open("/dev/null", O_WRONLY);
                    if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
                    execlp("pactl", "pactl", "move-sink-input", id.c_str(), sink_name.c_str(), nullptr);
                    _exit(127);
                }
                if (mv_pid > 0) { int s; waitpid(mv_pid, &s, 0); }
            }
        }
        
        // Get volume offset label for display
        std::string get_volume_offset_label() const {
            if (retroarch_volume_offset_db >= 0.0f) return "Normal";
            else if (retroarch_volume_offset_db >= -4.0f) return "Quiet (-3dB)";
            else if (retroarch_volume_offset_db >= -7.0f) return "Quieter (-6dB)";
            else return "Much Quieter (-12dB)";
        }
        
        // Cycle through volume offset options
        void cycle_volume_offset() {
            if (retroarch_volume_offset_db >= 0.0f) retroarch_volume_offset_db = -3.0f;
            else if (retroarch_volume_offset_db >= -4.0f) retroarch_volume_offset_db = -6.0f;
            else if (retroarch_volume_offset_db >= -7.0f) retroarch_volume_offset_db = -12.0f;
            else retroarch_volume_offset_db = 0.0f;
        }
        
        // Cycle through audio output options
        void cycle_output() {
            switch (output) {
                case AudioOutput::AUTO: output = AudioOutput::HDMI; break;
                case AudioOutput::HDMI: output = AudioOutput::HEADPHONE; break;
                case AudioOutput::HEADPHONE: output = AudioOutput::AUTO; break;
            }
            apply_output();
        }
    } audio_settings;
    
    // Available bezels (loaded from bezels.json)
    std::vector<BezelInfo> available_bezels;

    AppState()
        : selected_index(0),
          original_volume(100.0),
          current_playlist_index(-1),
          current_item_index(-1),
          last_advanced_item_index(-1),
          last_advanced_duration(0.0),
          loop(false),
          playlist_loop(true),
          shuffle(false),
          master_shuffle_active(false),
          sample_mode_active(false),
          settings_menu(nullptr),
          fade_duration(300),
          fade_target_ui_visible(false),
          is_fading(false),
          showing_intro_video(false),
          intro_ready(false),
          intro_complete(false),
          intro_fading_out(false)
    {
        // Atomic members are already initialized inline
        // Initialize default display settings
        display_settings.scanline_intensity = 0.1f;
    }
    
    // Wi-Fi State
    struct WifiState {
        bool enabled = true;
        bool scanning = false;
        std::vector<utils::WifiNetwork> scan_results;
        std::string status_message;
    } wifi_state;
    
    // Content Manager URLs for QR code display
    std::string wifi_url;           // WiFi URL (e.g., http://10.0.0.151:5000)
    std::string usb_url;            // USB URL (always http://192.168.7.1:5000)
    std::string content_manager_url; // Currently displayed URL (changes based on menu selection)

    // How many playlist rows the renderer is currently showing on screen.
    // Computed per-frame in render_playlist_list from height_ + item_height
    // and written here so main.cpp's input handler scrolls only when the
    // selection actually goes off-screen. Default 8 matches the established
    // CRT layout — the renderer overwrites this on every frame anyway.
    int playlist_max_visible = 8;
    
    // Virtual Keyboard
    ui::VirtualKeyboard* keyboard = nullptr; // pointer to keyboard instance (owned by main/renderer or here?) 
    // Better to let AppState own it or main. Let's make it a pointer managed by main, 
    // or include the header and make it a member. Header inclusion is cleaner for usage.
    // For now, pointer to avoid header dependency hell in this header 
    // (forward declared above).
};

} // namespace app
