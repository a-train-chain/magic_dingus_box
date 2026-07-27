#include "platform/drm_display.h"
#include "platform/gbm_context.h"
#include "platform/egl_context.h"
#include "platform/input_manager.h"
#include "platform/gpio_manager.h"
#include "platform/platform_profile.h"
#include "video/gst_player.h"
#include "video/gst_renderer.h"
#include "ui/renderer.h"
#include "ui/settings_menu.h"
#include "ui/pairing_screen.h"
#include "ui/pairing_screen_renderer.h"
#ifdef MEDIA_BROWSER_ENABLED
#include "ui/toast.h"
#include "platform/sequence_detector.h"
#include "media_browser/prowlarr/prowlarr_client.h"
#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/radarr/radarr_client.h"
#include "media_browser/radarr/radarr_mock.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/ui/browse_screen.h"
#include "media_browser/ui/search_screen.h"
#include "media_browser/ui/detail_screen.h"
#include "media_browser/ui/queue_screen.h"
#include "media_browser/ui/library_screen.h"
#include "media_browser/ui/mb_settings_screen.h"
#include "media_browser/ui/playback_screen.h"
#include "media_browser/ui/release_picker_screen.h"
#include "media_browser/ui/mb_exit_modal.h"
#include "media_browser/ui/stall_prompt_modal.h"
#include "media_browser/qbittorrent/download_watchdog.h"
#include "media_browser/health/vpn_health_monitor.h"
#include "media_browser/artwork/artwork_cache.h"
#endif
#include "app/app_state.h"
#include "app/game_quiet_mode.h"
#include "app/playlist_loader.h"
#include "app/controller.h"
#include "app/sample_mode.h"
#include "app/settings_persistence.h"
#include "app/status_writer.h"
#include "utils/config.h"
#include "utils/path_resolver.h"
#include "utils/wifi_manager.h"
#include "utils/logger.h"
#include "ui/virtual_keyboard.h"

#include <json/json.h>
#include <iostream>
#include <memory>
#include <chrono>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <optional>
#ifdef MEDIA_BROWSER_ENABLED
#include <algorithm>
#endif
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <GLES3/gl3.h>
// GStreamer is now the only video backend - MPV headers removed
#include <cstring>
#include <cerrno>
#include <sys/select.h>
#include <unistd.h>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

using namespace platform;
using namespace video;
using namespace ui;
using namespace app;

namespace fs = std::filesystem;

struct PageFlipContext {
    bool waiting_for_flip;
};

static void page_flip_handler(int /*fd*/, unsigned int /*frame*/,
                              unsigned int /*sec*/, unsigned int /*usec*/,
                              void *data) {
    PageFlipContext *ctx = (PageFlipContext*)data;
    if (ctx) {
        ctx->waiting_for_flip = false;
    }
}

int main(int /* argc */, char* /* argv */[]) {
    // Initialize logging system
    // Log to file in config directory if available, otherwise console only
    std::string log_path = config::get_log_file();
    logging::init(log_path);

    LOG_INFO("Magic Dingus Box C++ Kiosk Engine (Async PageFlip)");
    LOG_DEBUG("Log file: {}", log_path.empty() ? "console only" : log_path);

    // Initialize random number generator for Master Shuffle
    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    // Check if X11 is running (it will block DRM access)
    if (getenv("DISPLAY") != nullptr) {
        LOG_WARN("DISPLAY environment variable is set. X11 may be using the display.");
        LOG_WARN("For DRM/KMS mode, stop X11 first: sudo systemctl stop lightdm.service");
    }

    // Initialize DRM/KMS display (use "auto" to find the right device)
    DrmDisplay display;
    if (!display.initialize("auto")) {
        LOG_ERROR("Failed to initialize DRM display");
        LOG_ERROR("Common causes:");
        LOG_ERROR("  1. X11/lightdm is running (stop with: sudo systemctl stop lightdm)");
        LOG_ERROR("  2. Another process is using the display");
        LOG_ERROR("  3. No display connected");
        logging::shutdown();
        return 1;
    }
    
    // Set display mode. The target depends on the persisted display
    // mode, so peek at just that one setting — the full load_settings()
    // runs much later and needs AppState, which does not exist yet.
    //
    //   MODERN_TV  -> 1920x1080. The UI still draws into a 1280x720
    //                 LOGICAL canvas (see config::display::logical_canvas
    //                 and the resize_screen calls below), so the menus
    //                 keep pixel-identical proportions; the extra
    //                 resolution goes to video, which is where the Media
    //                 Browser's true-1080p movies were being downscaled.
    //   CRT_NATIVE -> 1280x720, unchanged. Pi 5 has no composite output,
    //                 so CRT rigs run through an HDMI->composite
    //                 converter; CRT_MAX_HEIGHT keeps it on a mode that
    //                 converter accepts. 1080p is deliberately NOT in
    //                 this branch's fallback chain.
    const bool boot_crt_native = app::SettingsPersistence::peek_is_crt_native();
    const config::display::Size boot_mode =
        config::display::target_drm_mode(boot_crt_native);
    LOG_INFO("Display mode setting: {} -> requesting {}x{}",
             boot_crt_native ? "CRT_NATIVE" : "MODERN_TV",
             boot_mode.w, boot_mode.h);

    bool mode_ok = display.set_mode(boot_mode.w, boot_mode.h);
    if (!mode_ok && !boot_crt_native) {
        // Modern TV only: fall back to 720p before the CRT-safe sizes.
        LOG_INFO("{}x{} not available, trying {}x{}...", boot_mode.w, boot_mode.h,
                 config::display::PREFERRED_WIDTH, config::display::PREFERRED_HEIGHT);
        mode_ok = display.set_mode(config::display::PREFERRED_WIDTH,
                                   config::display::PREFERRED_HEIGHT);
    }
    if (!mode_ok) {
        LOG_INFO("Trying {}x{} (CRT native)...",
                 config::display::FALLBACK_WIDTH_2, config::display::FALLBACK_HEIGHT_2);
        mode_ok = display.set_mode(config::display::FALLBACK_WIDTH_2,
                                   config::display::FALLBACK_HEIGHT_2);
    }
    if (!mode_ok) {
        LOG_INFO("Using auto-detect...");
        if (!display.set_mode(0, 0)) {
            LOG_ERROR("Failed to set display mode");
            logging::shutdown();
            return 1;
        }
    }

    auto mode = display.get_current_mode();
    LOG_INFO("Display mode: {}x{}@{}Hz", mode.width, mode.height, mode.refresh);
    
    // Get mode info for page flipping
    drmModeConnector* conn = drmModeGetConnector(display.get_fd(), display.get_connector_id());
    drmModeModeInfo mode_info = {}; // Value initialization
    if (conn && conn->count_modes > 0) {
        // Find the mode matching our current resolution
        for (int i = 0; i < conn->count_modes; i++) {
            if (conn->modes[i].hdisplay == mode.width && 
                conn->modes[i].vdisplay == mode.height) {
                mode_info = conn->modes[i];
                break;
            }
        }
        if (mode_info.hdisplay == 0) {
            mode_info = conn->modes[0];  // Fallback to first mode
        }
    }
    if (conn) drmModeFreeConnector(conn);
    
    // Initialize GBM
    GbmContext gbm;
    if (!gbm.initialize(display.get_fd(), mode.width, mode.height)) {
        LOG_ERROR("Failed to initialize GBM");
        logging::shutdown();
        return 1;
    }

    // Initialize EGL
    EglContext egl;
    if (!egl.initialize(gbm.get_device(), gbm.get_surface())) {
        LOG_ERROR("Failed to initialize EGL");
        logging::shutdown();
        return 1;
    }

    LOG_INFO("EGL initialized: OpenGL ES {}.{}", egl.get_major_version(), egl.get_minor_version());

    // Make EGL context current
    if (!egl.make_current()) {
        LOG_ERROR("Failed to make EGL context current");
        logging::shutdown();
        return 1;
    }
    LOG_DEBUG("EGL context made current");

    // Initialize display to black immediately - professional boot experience
    // This ensures the first thing user sees is black, not random screen content
    glViewport(0, 0, mode.width, mode.height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    egl.swap_buffers();
    LOG_DEBUG("Display initialized to black");

    // Initialize input
    LOG_DEBUG("Initializing input...");
    InputManager input;
    if (!input.initialize()) {
        LOG_WARN("Failed to initialize input");
    } else {
        LOG_DEBUG("Input initialized");
    }
    
    // Initialize GPIO (for physical buttons, rotary encoder, LEDs, power switch)
    LOG_DEBUG("Initializing GPIO...");
    GpioManager gpio;
    if (!gpio.initialize()) {
        LOG_DEBUG("GPIO not available (normal if not on Raspberry Pi)");
    } else {
        LOG_DEBUG("GPIO initialized");
        // Stop the boot LED chase sequence now that the app is starting
        gpio.stop_boot_led_sequence();
    }

    // Initialize GStreamer player
    LOG_DEBUG("Initializing GStreamer player...");
    GstPlayer player;
    if (!player.initialize()) {
        LOG_ERROR("Failed to initialize GStreamer player");
        logging::shutdown();
        return 1;
    }
    LOG_INFO("GStreamer player initialized");

    // Initialize GStreamer renderer
    LOG_DEBUG("Initializing GStreamer renderer...");
    GstRenderer gst_renderer;
    // We don't need to pass EGL display explicitly as we handle GL context in GstRenderer with current context
    if (!gst_renderer.initialize(&player)) {
        LOG_ERROR("Failed to initialize GStreamer renderer");
        logging::shutdown();
        return 1;
    }
    gst_renderer.set_viewport_size(mode.width, mode.height);
    LOG_INFO("GStreamer renderer initialized");

    // Initialize UI renderer
    LOG_DEBUG("Initializing UI renderer...");
    Renderer ui_renderer(mode.width, mode.height);
    
    // Try multiple font paths for title font (Zen Dots)
    std::vector<std::string> title_font_paths = config::get_font_search_paths();
    title_font_paths.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf");  // Fallback
    
    // Try multiple font paths for body font (mono)
    std::vector<std::string> body_font_paths = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",  // Common system font
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",  // Another common system font
        "/usr/share/fonts/truetype/ttf-dejavu/DejaVuSansMono.ttf"  // Alternative path
    };
    
    std::string title_font_path;
    std::string body_font_path;
    
    // Find title font (Zen Dots preferred)
    for (const auto& path : title_font_paths) {
        std::ifstream test(path);
        if (test.good()) {
            title_font_path = path;
            break;
        }
    }
    
    // Find body font (mono)
    for (const auto& path : body_font_paths) {
        std::ifstream test(path);
        if (test.good()) {
            body_font_path = path;
            break;
        }
    }
    
    if (title_font_path.empty() || body_font_path.empty()) {
        LOG_ERROR("Failed to find required fonts");
        for (const auto& path : title_font_paths) {
            LOG_ERROR("  Title font tried: {}", path);
        }
        for (const auto& path : body_font_paths) {
            LOG_ERROR("  Body font tried: {}", path);
        }
        logging::shutdown();
        return 1;
    }

    if (!ui_renderer.initialize(title_font_path, body_font_path)) {
        LOG_ERROR("Failed to initialize UI renderer");
        logging::shutdown();
        return 1;
    }

    LOG_DEBUG("Title font: {}", title_font_path);
    LOG_DEBUG("Body font: {}", body_font_path);
    LOG_INFO("UI renderer initialized");
    
    // Load playlists
    // Try multiple paths: relative to executable, relative to build dir, and absolute
    std::vector<std::string> playlist_paths = config::get_playlist_search_paths();
    
    std::vector<Playlist> all_playlists;
    std::string playlist_dir;
    for (const auto& path : playlist_paths) {
        std::ifstream test(path + "/test");
        if (test.good() || fs::exists(path)) {
            test.close();
            all_playlists = PlaylistLoader::load_playlists(path);
            playlist_dir = path;
            break;
        }
    }
    
    if (all_playlists.empty()) {
        LOG_WARN("No playlists loaded");
    } else {
        LOG_INFO("Loaded {} playlists from: {}", all_playlists.size(), playlist_dir);
    }
    
    // Separate video and game playlists (matching Python version)
    std::vector<Playlist> video_playlists;
    std::vector<Playlist> game_playlists;
    for (const auto& pl : all_playlists) {
        if (pl.is_video_playlist()) {
            video_playlists.push_back(pl);
        } else if (pl.is_game_playlist()) {
            game_playlists.push_back(pl);
        }
    }
    
#ifdef MEDIA_BROWSER_ENABLED
    // Media Browser V2: append a synthetic "Movies" playlist populated from
    // the Radarr download root (/mnt/ssd/library/Movies). Only add it if at
    // least one movie has been downloaded — an empty "Movies" row would
    // confuse users on a fresh install before any content lands.
    {
        Playlist movies_pl = PlaylistLoader::load_movies_library();
        if (!movies_pl.items.empty()) {
            LOG_INFO("Media Browser: loaded {} movies from library",
                     movies_pl.items.size());
            video_playlists.push_back(movies_pl);
        } else {
            LOG_INFO("Media Browser: no movies found in library (yet)");
        }
    }
#endif

    // Insert "Master Shuffle" playlist at the beginning
    Playlist master_shuffle;
    master_shuffle.title = "Master Shuffle";
    master_shuffle.path = ""; // Virtual path
    master_shuffle.items.push_back({}); // Dummy item to make it selectable
    video_playlists.insert(video_playlists.begin(), master_shuffle);
    
    // Main UI shows only video playlists (matching Python: playlists = video_playlists)
    AppState state;
    state.playlists = video_playlists;
    state.game_playlists = game_playlists;  // Store for menu access
    
    std::cout << "Video playlists: " << video_playlists.size() << std::endl;
    std::cout << "Game playlists: " << game_playlists.size() << std::endl;
    
    // Load saved settings (CRT effects, loop, shuffle, etc.)
    SettingsPersistence::load_settings(state);

    // Detect the board we're running on (Pi 4B vs Pi 5) and reconcile
    // loaded settings with its hardware: a settings.json carried over
    // from a Pi 4 (golden image clone) may say "headphone", but the
    // Pi 5 has no analog jack — coerce to AUTO so audio resolves to
    // HDMI instead of a nonexistent sink.
    {
        state.platform_profile = platform::detect_platform();

        // Cache the box's own hostname once. Several screens tell the
        // operator where to reach the box, and hardcoding a name is how
        // both the pairing QR and the Wi-Fi screen ended up advertising
        // "magicpi.local" — which resolves on no shipped unit, since
        // first_boot.sh names every clone "magicpi-XXXX". The pairing
        // screen refreshes this (and the IP) when it opens, because the
        // address can change; this startup value makes it available to
        // screens that render earlier.
        {
            char host[256] = {0};
            if (gethostname(host, sizeof(host) - 1) == 0) {
                state.hostname = host;
            }
        }
        const platform::PlatformProfile& profile = state.platform_profile;
        const char* model_name =
            (profile.model == platform::PiModel::Pi4) ? "Raspberry Pi 4" :
            (profile.model == platform::PiModel::Pi5) ? "Raspberry Pi 5" :
            "unknown board";
        std::cout << "Platform: " << model_name
                  << " (analog audio: " << (profile.has_analog_audio ? "yes" : "no")
                  << ", rotary events/detent: " << profile.rotary_events_per_detent
                  << ")" << std::endl;
        state.audio_settings.sanitize_for_platform(profile.has_analog_audio);
        // Match the encoder accumulator to this board's pulse rate, or the
        // UI advances every other click (Pi 5) / skips items (Pi 4).
        input.set_rotary_events_per_detent(profile.rotary_events_per_detent);
    }

    // Phone-remote status writer — writes kiosk_status.json at 5 Hz so the
    // companion app always has a fresh snapshot of screen / playback state.
    app::StatusWriter status_writer(config::get_data_path() + "/kiosk_status.json");
    auto last_status_write = std::chrono::steady_clock::now();
    constexpr auto STATUS_PERIOD = std::chrono::milliseconds(200); // 5 Hz

    // Phone Remote: clear any stale pairing session left from a crashed
    // previous run. Flask only trusts a session that's been freshly issued
    // from the open pairing screen, so wiping it on boot prevents a leaked
    // QR (from a screenshot, etc.) being usable after the next reboot.
    {
        std::string stale = config::get_data_path() + "/pairing_session.json";
        std::error_code ec;
        std::filesystem::remove(stale, ec);
        if (!ec) {
            spdlog::info("[remote] cleared stale pairing_session.json from previous run");
        }
    }
    // seek_request.json and pending_revocations.txt shouldn't survive a crash either.
    for (const std::string& f : {"seek_request.json", "pending_revocations.txt"}) {
        std::error_code ec;
        std::filesystem::remove(config::get_data_path() + "/" + f, ec);
    }

#ifdef MEDIA_BROWSER_ENABLED
    // Layer 3 monitor — only meaningful when Layers 1+2 already pass.
    // Otherwise the Settings menu won't expose MB anyway, so save the
    // background polling work. Lifetime: declared here at function scope
    // so its destructor stops the worker thread cleanly on main() exit.
    std::unique_ptr<media_browser::VpnHealthMonitor> vpn_health_monitor;
    if (state.media_browser_unlocked && state.media_browser_vpn_configured) {
        vpn_health_monitor = std::make_unique<media_browser::VpnHealthMonitor>(state);
        vpn_health_monitor->start();

        // Startup safety: call playback_services_pause.sh unpause to bring
        // any stopped MB containers back up. This is the recovery path for
        // "PlaybackScreen::leave() didn't run cleanly" cases — e.g. kiosk
        // crashed mid-playback or was SIGABRT'd by systemd watchdog. With
        // the docker-stop pause behavior, missed leave() = stranded
        // containers (Docker's restart=unless-stopped doesn't auto-start
        // manually-stopped containers). Without this safety: Movies entry
        // silently vanishes on next boot. unpause is idempotent — no-op
        // for already-running containers, starts stopped ones. Backgrounded
        // (& at end) so the ~1-2 s docker start doesn't block the kiosk
        // entering its main loop.
        std::system(
            "/usr/local/bin/playback_services_pause.sh unpause "
            ">/dev/null 2>&1 &");
    }
#endif

    // Load available bezels from bezels.json using JsonCpp
    std::vector<std::string> bezel_json_paths = config::get_bezel_search_paths();

    for (const auto& bezel_path : bezel_json_paths) {
        std::ifstream bezel_file(bezel_path);
        if (bezel_file.good()) {
            Json::Value root;
            Json::CharReaderBuilder builder;
            std::string errors;

            if (!Json::parseFromStream(builder, bezel_file, &root, &errors)) {
                std::cerr << "Failed to parse bezels.json: " << errors << std::endl;
                bezel_file.close();
                continue;
            }
            bezel_file.close();

            // Parse bezels array
            if (root.isMember("bezels") && root["bezels"].isArray()) {
                const Json::Value& bezels = root["bezels"];
                for (const auto& bezel_json : bezels) {
                    app::BezelInfo bezel;
                    bezel.id = bezel_json.get("id", "").asString();
                    bezel.name = bezel_json.get("name", "").asString();

                    // Handle null file (procedural bezel)
                    if (bezel_json.isMember("file") && !bezel_json["file"].isNull()) {
                        bezel.file = bezel_json["file"].asString();
                    } else {
                        bezel.file = "";  // Procedural bezel
                    }

                    bezel.description = bezel_json.get("description", "").asString();

                    if (!bezel.name.empty()) {
                        state.available_bezels.push_back(bezel);
                    }
                }
            }

            std::cout << "Loaded " << state.available_bezels.size() << " bezels from " << bezel_path << std::endl;
            break;
        }
    }
    
    // Ensure bezel_index is valid
    if (!state.available_bezels.empty()) {
        if (state.display_settings.bezel_index < 0 || 
            state.display_settings.bezel_index >= static_cast<int>(state.available_bezels.size())) {
            state.display_settings.bezel_index = 0;
        }
    }
    
    // Initialize controller and sample mode
    Controller controller(&player);
    controller.set_display(&display);  // Set display reference for DRM cleanup
    controller.set_input_manager(&input);  // Set input manager reference for controller release
    controller.set_text_input_queue_path(
        config::get_data_path() + "/text_input_queue.jsonl");
    
    // Initialize Virtual Keyboard
    VirtualKeyboard keyboard;
    state.keyboard = &keyboard;
    
    // Initialize Wifi Manager
    utils::WifiManager::instance().initialize(); // Check for nmcli
    auto retroarch_result = controller.initialize_retroarch_launcher();
    if (!retroarch_result) {
        std::cerr << "Warning: " << retroarch_result.error() << std::endl;
    }
    
    // Apply initial system volume
    controller.set_system_volume(state.master_volume);
    
    SampleMode sample_mode;
    
    // Store playlist directory for path resolution
    std::string playlist_directory = playlist_dir;
    
    // Initialize settings menu
    ui::SettingsMenuManager settings_menu(&state);
    state.settings_menu = &settings_menu;

#ifdef MEDIA_BROWSER_ENABLED
    // Media Browser secret-unlock sequence detector. Fed each frame from
    // GPIO events; fires Toast + sets state.media_browser_unlocked when
    // the full chord → BTN2 × 3 → rotary-click sequence is entered.
    platform::SequenceDetector seq_detector;

    // Task 17: screen dispatcher for the Media Browser.
    //
    // Radarr client is the real HTTP client when we can find an API key:
    //   1. MDB_RADARR_API_KEY env var (preferred — explicit kiosk config)
    //   2. RADARR_API_KEY env var (systemd EnvironmentFile of services/.env)
    //   3. Parse /opt/magic_dingus_box/services/.env directly (fallback
    //      for when systemd env propagation isn't set up)
    // Otherwise we fall back to RadarrMockClient for dev machines.
    auto read_env_file_key = [](const std::string& path, const std::string& key) -> std::string {
        std::ifstream f(path);
        if (!f) return "";
        std::string line;
        const std::string prefix = key + "=";
        while (std::getline(f, line)) {
            if (line.rfind(prefix, 0) == 0) {
                std::string v = line.substr(prefix.size());
                while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
                return v;
            }
        }
        return "";
    };

    std::unique_ptr<media_browser::RadarrClient> radarr_owned;
    std::string radarr_key;
    if (const char* rk = std::getenv("MDB_RADARR_API_KEY"); rk && *rk) radarr_key = rk;
    else if (const char* rk2 = std::getenv("RADARR_API_KEY"); rk2 && *rk2) radarr_key = rk2;
    else radarr_key = read_env_file_key("/opt/magic_dingus_box/services/.env", "RADARR_API_KEY");

    if (!radarr_key.empty()) {
        media_browser::RadarrClient::Config radarr_cfg;
        if (const char* base = std::getenv("MDB_RADARR_BASE_URL"); base && *base) {
            radarr_cfg.base_url = base;
        }
        radarr_cfg.api_key = radarr_key;
        // Path-translation overrides: redirect /library/* between the Radarr
        // container's view and the host's actual mount point. Default in
        // radarr_cfg works for the standard /mnt/ssd setup; the env vars
        // exist so STORAGE_ROOT can change without a kiosk recompile. The
        // download/incomplete tree is not exposed via Radarr's API to the
        // kiosk, so it doesn't need translation here. normalize_prefix
        // ensures both prefixes end with '/' to avoid /library2/foo
        // falsely matching /library.
        if (const char* p = std::getenv("MDB_CONTAINER_LIBRARY_PREFIX"); p && *p) {
            radarr_cfg.container_library_prefix =
                media_browser::RadarrClient::normalize_prefix(p);
            std::cout << "[media_browser] container_library_prefix override: "
                      << radarr_cfg.container_library_prefix << std::endl;
        }
        if (const char* p = std::getenv("MDB_HOST_LIBRARY_PREFIX"); p && *p) {
            radarr_cfg.host_library_prefix =
                media_browser::RadarrClient::normalize_prefix(p);
            std::cout << "[media_browser] host_library_prefix override: "
                      << radarr_cfg.host_library_prefix << std::endl;
        }
        std::string base_url_for_log = radarr_cfg.base_url;
        radarr_owned = std::make_unique<media_browser::RadarrClient>(std::move(radarr_cfg));
        std::cout << "[media_browser] Using real RadarrClient (base_url="
                  << base_url_for_log << ")" << std::endl;
    } else {
        radarr_owned = std::make_unique<media_browser::RadarrMockClient>();
        std::cout << "[media_browser] No Radarr API key found — using RadarrMockClient" << std::endl;
    }
    media_browser::RadarrClient& radarr = *radarr_owned;

    // TMDB client — Phase A: Discover endpoints for Browse categories.
    // Radarr still handles library/add/queue; TMDB only drives discovery.
    std::string tmdb_key;
    if (const char* k = std::getenv("MDB_TMDB_API_KEY"); k && *k) {
        tmdb_key = k;
    } else if (const char* home = std::getenv("HOME"); home) {
        std::ifstream kf(std::string(home) + "/.config/magic_dingus_box/tmdb_api_key");
        if (kf) std::getline(kf, tmdb_key);
        while (!tmdb_key.empty() &&
               (tmdb_key.back() == '\n' || tmdb_key.back() == '\r' ||
                tmdb_key.back() == ' ')) {
            tmdb_key.pop_back();
        }
    }
    if (tmdb_key.empty()) {
        std::cout << "[media_browser] WARN: No TMDB API key (MDB_TMDB_API_KEY or "
                     "~/.config/magic_dingus_box/tmdb_api_key). Browse categories "
                     "will be empty until a key is configured." << std::endl;
    } else {
        std::cout << "[media_browser] TMDB API key loaded (len=" << tmdb_key.size() << ")"
                  << std::endl;
    }
    auto tmdb = std::make_unique<media_browser::TmdbClient>(tmdb_key);

    // Optional Prowlarr client for the AVAILABILITY readout on Detail.
    // Same key lookup chain as Radarr above:
    //   1. MDB_PROWLARR_API_KEY env var
    //   2. PROWLARR_API_KEY env var (systemd EnvironmentFile)
    //   3. Parse /opt/magic_dingus_box/services/.env directly
    // Falls back to nullptr (readout suppressed) if no key is found.
    std::unique_ptr<media_browser::ProwlarrClient> prowlarr_owned;
    {
        std::string prowlarr_key;
        if (const char* k = std::getenv("MDB_PROWLARR_API_KEY"); k && *k) {
            prowlarr_key = k;
        } else if (const char* k2 = std::getenv("PROWLARR_API_KEY"); k2 && *k2) {
            prowlarr_key = k2;
        } else {
            prowlarr_key = read_env_file_key(
                "/opt/magic_dingus_box/services/.env", "PROWLARR_API_KEY");
        }

        if (!prowlarr_key.empty()) {
            media_browser::ProwlarrClient::Config pcfg;
            pcfg.api_key = prowlarr_key;
            if (const char* base = std::getenv("MDB_PROWLARR_BASE_URL");
                base && *base) {
                pcfg.base_url = base;
            }
            prowlarr_owned = std::make_unique<media_browser::ProwlarrClient>(
                std::move(pcfg));
            std::cout << "[media_browser] Prowlarr client enabled "
                      << "(base_url=" << "http://localhost:9696" << ", "
                      << "key_len=" << prowlarr_key.size() << ")"
                      << std::endl;
        } else {
            std::cout << "[media_browser] Prowlarr client disabled "
                      << "(no PROWLARR_API_KEY found; AVAILABILITY readout "
                      << "on Detail will be suppressed)" << std::endl;
        }
    }

    // qBittorrent client — used by QueueScreen to overlay live
    // download progress over Radarr's stale-cached queue snapshot.
    // qBit always runs on localhost:8080 in our docker-compose setup;
    // credentials come from MDB_QBIT_USER / MDB_QBIT_PASS env vars,
    // or fall back to the docker-compose default (admin/adminadmin).
    auto qbit_owned = std::make_unique<media_browser::QbittorrentClient>(
        []() {
            media_browser::QbittorrentClient::Config cfg;
            if (const char* u = std::getenv("MDB_QBIT_USER"); u && *u) {
                cfg.username = u;
            }
            if (const char* p = std::getenv("MDB_QBIT_PASS"); p && *p) {
                cfg.password = p;
            }
            if (const char* url = std::getenv("MDB_QBIT_BASE_URL");
                url && *url) {
                cfg.base_url = url;
            }
            return cfg;
        }());
    std::cout << "[media_browser] qBittorrent client enabled "
              << "(base_url=http://localhost:8080)" << std::endl;

    // Track-1 quiet mode: silence the torrent/media stack for the whole
    // game session, mirroring PlaybackScreen's movie behavior. Gated on
    // the provisioning marker so unprovisioned Pis do exactly nothing
    // (no docker errors, no qBit timeouts in the log).
    app::GameQuietMode game_quiet_mode({
        /*pause=*/[qbit = qbit_owned.get()]() {
            if (!std::filesystem::exists(
                    "/opt/magic_dingus_box/services/.env")) {
                return;
            }
            if (qbit != nullptr && !qbit->pause_all()) {
                std::cout << "[quiet-mode] qbit pause_all failed "
                             "(best-effort)" << std::endl;
            }
            (void)std::system(
                "/usr/local/bin/playback_services_pause.sh pause "
                ">/dev/null 2>&1");
        },
        /*resume=*/[qbit = qbit_owned.get()]() {
            if (!std::filesystem::exists(
                    "/opt/magic_dingus_box/services/.env")) {
                return;
            }
            (void)std::system(
                "/usr/local/bin/playback_services_pause.sh unpause "
                ">/dev/null 2>&1");
            if (qbit != nullptr && !qbit->resume_all()) {
                std::cout << "[quiet-mode] qbit resume_all failed; "
                             "resume from web UI if needed" << std::endl;
            }
        }});

    // Six screens — dispatcher owns one instance of each.
    media_browser::ui::BrowseScreen     mb_browse(radarr, *tmdb, state);
    media_browser::ui::SearchScreen     mb_search(radarr);
    media_browser::ui::DetailScreen     mb_detail(radarr, *tmdb,
                                                  prowlarr_owned.get(),
                                                  qbit_owned.get());
    media_browser::ui::QueueScreen      mb_queue(radarr,
                                                  qbit_owned.get());
    media_browser::ui::LibraryScreen    mb_library(radarr, state);
    media_browser::ui::PlaybackScreen   mb_playback(controller, state, *tmdb,
                                                     radarr,
                                                     qbit_owned.get());
    // Manual release-picker screen — opened from Detail's "Pick a source"
    // button (Task 13) when the user wants to override Radarr's auto-pick.
    media_browser::ui::ReleasePickerScreen mb_release_picker(radarr);
    // Detail->ReleasePicker handoff: Detail forwards the (tmdb_id,
    // radarr_movie_id, title) tuple via this callback; the picker spawns
    // its own worker thread for the slow Radarr /api/v3/release call so
    // the UI stays responsive. Was synchronous in v1.7.x — froze the
    // render loop for 14-25s and would trip systemd's watchdog at the
    // old WatchdogSec=10 (we bumped to 30s in commit b2b8c4c, then
    // reverted to 10s once this async path landed).
    mb_detail.set_picker_callback(
        [&mb_release_picker](int tmdb_id, int radarr_movie_id,
                             std::string title) {
            // Set the tmdb_id first so the picker's SELECT branch can
            // register a watchdog watch keyed on it; then kick off the
            // worker thread. load_async() returns immediately — render()
            // shows the Loading state until update() drains the result.
            mb_release_picker.set_movie_tmdb_id(tmdb_id);
            mb_release_picker.load_async(radarr_movie_id, std::move(title));
        });
    // Task 23: the Movies Settings screen's "Hide Movies feature" checkbox
    // flips media_browser_unlocked=false and persists settings. The screen
    // triggers this callback AND returns Screen::Exit, so the dispatcher
    // below naturally transitions back to MainMenu on the next input tick.
    media_browser::ui::MbSettingsScreen mb_mb_settings(
        radarr,
        prowlarr_owned.get(),
        state,
        [&state]() {
            state.media_browser_unlocked = false;
            app::SettingsPersistence::save_settings(state);
            ui::Toast::show("Movie section hidden");
            LOG_INFO("Media Browser: feature re-locked via Movies Settings screen");
        });

    media_browser::ui::Screen current_mb_screen = media_browser::ui::Screen::Browse;
    media_browser::ui::MbScreen* active_mb_screen = &mb_browse;
    // enter() is called the first time we actually transition into the
    // Media Browser, so entering always starts fresh on Browse.

    // Global "Exit Marquee?" confirm modal. Owned here; intercepts BTN2
    // (PLAY_PAUSE) before any MB screen sees it. Task 7.
    media_browser::ui::ExitModal mb_exit_modal;

    // Background watchdog for stalled downloads + modal that surfaces
    // its events to the user. The watchdog is ticked once per frame in
    // the main loop; tick() rate-limits its own service polls to ~once
    // per 10s so the per-frame call is cheap. The modal traps input
    // while active and emits Pick / Dismiss via the handlers below.
    // Task 16.
    media_browser::DownloadWatchdog mb_watchdog(radarr, *qbit_owned);
    media_browser::ui::StallPromptModal mb_stall_modal;
    mb_stall_modal.set_handlers(
        [&current_mb_screen, &active_mb_screen, &mb_detail,
         &mb_release_picker]
        (int tmdb_id, int radarr_movie_id, const std::string& title) {
            // Deep-link path: when the stall event carries the movie's
            // radarr id (every grab since the v1.7.x async picker
            // refactor does), open the picker directly with a fresh
            // load_async() — no Detail intermediate flash. The picker
            // shows its Loading state instantly and the user is one
            // click away from picking a different release.
            //
            // Fallback path: older watches (registered before the id
            // plumbing landed, or any future code path that calls
            // watch() with radarr_movie_id=0) route through Detail. The
            // user can press "Pick a source" from there to open the
            // picker manually — more clicks but the same end state.
            if (radarr_movie_id > 0) {
                mb_release_picker.set_movie_tmdb_id(tmdb_id);
                mb_release_picker.load_async(radarr_movie_id, title);
                active_mb_screen->leave();
                current_mb_screen = media_browser::ui::Screen::ReleasePicker;
                active_mb_screen = &mb_release_picker;
                active_mb_screen->enter();
                return;
            }
            // Fallback: open Detail and let the user re-pick from there.
            if (tmdb_id > 0) {
                mb_detail.set_tmdb_id(tmdb_id);
                mb_detail.set_origin(media_browser::ui::Screen::Browse);
            }
            active_mb_screen->leave();
            current_mb_screen = media_browser::ui::Screen::Detail;
            active_mb_screen = &mb_detail;
            active_mb_screen->enter();
        },
        [&mb_watchdog](int tmdb_id) {
            mb_watchdog.snooze(tmdb_id, std::chrono::minutes(10));
        });

    // Wire the watchdog into the screens that initiate downloads.
    mb_detail.set_watchdog(&mb_watchdog);
    mb_release_picker.set_watchdog(&mb_watchdog);

    // Wire the VPN-health probe into Detail so its render() can show a
    // banner ("VPN tunnel down") on Add/Re-search modes when the tunnel
    // is unhealthy. Captures &state by reference; the lambda lives as
    // long as mb_detail (which owns the std::function).
    mb_detail.set_vpn_health_provider([&state]() {
        return state.media_browser_vpn_healthy.load(std::memory_order_acquire);
    });
#endif
    
    // Try to load intro video at startup
    // Look for intro video in common locations (prefer .30fps version)
    std::vector<std::string> intro_paths = config::get_intro_search_paths();
    
    std::string intro_video_path;
    std::cout << "Checking for intro video in " << intro_paths.size() << " locations..." << std::endl;
    for (const auto& path : intro_paths) {
        std::cout << "  Checking: " << path << std::endl;
        // Try direct path check first (avoids path resolver warnings)
        if (fs::exists(path)) {
            try {
                fs::path canonical_path = fs::canonical(path);
                intro_video_path = canonical_path.string();
                std::cout << "Found intro video: " << intro_video_path << std::endl;
                break;
            } catch (const std::exception& e) {
                // Canonical failed, try absolute path
                fs::path abs_path = fs::absolute(path);
                if (fs::exists(abs_path)) {
                    intro_video_path = abs_path.string();
                    std::cout << "Found intro video: " << intro_video_path << std::endl;
                    break;
                }
            }
        }
    }
    
    // Load and play intro video if found
    // IMPORTANT: Set showing_intro_video BEFORE loading to prevent UI from appearing
    if (!intro_video_path.empty()) {
        // Set intro state immediately to prevent UI from rendering
        state.showing_intro_video = true;
        state.intro_ready = false;  // Not ready until video actually starts
        state.ui_visible_when_playing = false;  // UI transparent during intro
        state.video_active = false;  // Will be set to true when video actually starts
        
        auto intro_result = controller.load_file_with_resolution(intro_video_path, playlist_directory, 0.0, 0.0, false);
        if (intro_result) {
            controller.play();
            std::cout << "Intro video loaded, waiting for playback to start..." << std::endl;
            
            // Wait for intro video to actually start playing AND render at least one frame
            // This ensures the first thing user sees is the video, not UI or blank screen
            // Increased timeout to 10s (200 * 50ms) to allow for slower startup on Pi
            int wait_count = 0;
            const int max_wait = 200;  // Wait up to 10 seconds
            bool first_frame_rendered = false;
            
            while ((!state.intro_ready || !first_frame_rendered) && wait_count < max_wait) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                controller.update_state(state);
                
                // Check if mpv has rendered a frame
                if (state.intro_ready && !first_frame_rendered) {
                // Check if frame is ready - let main loop handle actual rendering
                // This prevents race condition with main loop's buffer management
                uint64_t flags = gst_renderer.get_update_flags();
                if (flags & GstRenderer::UPDATE_FRAME) {
                    // Frame is ready - mark as rendered and let main loop handle it
                    // DON'T call gst_renderer.render() or egl.swap_buffers() here!
                    // The main loop will handle all rendering and buffer swapping
                    first_frame_rendered = true;
                    std::cout << "Intro video first frame ready, entering main loop" << std::endl;
                }
                }
                
                wait_count++;
            }
            
            if (state.intro_ready && first_frame_rendered) {
                std::cout << "Intro video ready with first frame, entering main loop" << std::endl;
            } else {
                std::cerr << "Warning: Intro video did not start within timeout, proceeding anyway" << std::endl;
                // Force skip intro if it timed out to prevent black screen
                state.showing_intro_video = false;
                state.intro_complete = true;
                state.video_active = false;
                player.stop();
            }
        } else {
            std::cerr << "Warning: Failed to load intro video, skipping intro" << std::endl;
            state.showing_intro_video = false;  // Reset if load failed
            state.intro_complete = true;  // Skip intro if file can't be loaded
        }
    } else {
        std::cout << "No intro video found, starting with UI" << std::endl;
        state.intro_complete = true;  // No intro video, show UI immediately
    }
    
#ifdef HAVE_SYSTEMD
    sd_notify(0, "READY=1");
#endif

    // Main loop
    bool running = true;
    auto last_frame = std::chrono::steady_clock::now();

    std::cout << "Entering main loop..." << std::endl;

    // Frame presentation context - encapsulates all GBM/DRM state
    // Using a struct instead of static variables allows proper cleanup and reset
    struct FrameContext {
        std::unordered_map<uint32_t, uint32_t> fb_cache;  // bo_handle -> fb_id
        struct gbm_bo* previous_bo = nullptr;             // Buffer from previous frame
        uint32_t previous_bo_handle = 0;
        uint32_t current_fb_id = 0;
        bool first_frame = true;
        int force_setcrtc_frames = 0;      // Force SetCrtc for multiple frames after reset
        int consecutive_buffer_failures = 0;  // Track consecutive buffer lock failures
        int page_flip_failures = 0;           // Track page flip failures
        int successful_page_flips = 0;        // Track successful page flips for counter reset

        // Reset all state for display recovery
        void reset(int drm_fd, struct gbm_surface* gbm_surface) {
            // Clean up framebuffers
            for (auto& pair : fb_cache) {
                drmModeRmFB(drm_fd, pair.second);
            }
            fb_cache.clear();

            // Release GBM buffer
            if (previous_bo && gbm_surface) {
                gbm_surface_release_buffer(gbm_surface, previous_bo);
            }
            previous_bo = nullptr;
            previous_bo_handle = 0;
            current_fb_id = 0;

            // Reset flags
            first_frame = true;
            force_setcrtc_frames = 10;  // Force SetCrtc for stability after reset
            consecutive_buffer_failures = 0;
            page_flip_failures = 0;
            successful_page_flips = 0;
        }
    };

    FrameContext frame_ctx;

    // Frame presentation lambda
    // Encapsulates GBM/DRM logic to be shared between main loop and loading callback
    auto present_frame = [&]() {
        // Double buffering strategy (GBM pools typically have only 2-3 buffers):
        // - previous_bo: previous frame (release after we've presented next frame)
        // - current bo: being presented now

        // Release the previous buffer BEFORE locking a new one
        // This ensures GBM pool has an available buffer
        // We release after 1 frame delay (previous frame is safe after we present next)
        if (frame_ctx.previous_bo != nullptr) {
            // CRITICAL: DO NOT remove framebuffer from cache when releasing GBM buffer
            // The framebuffer should stay in cache so we can reuse it if the buffer cycles back
            // Only remove framebuffers that are definitely no longer needed (old entries in cache)

            // CRITICAL: Release the GBM buffer BEFORE locking a new one
            // This prevents GBM from trying to allocate new buffers
            // Validate that previous_bo is actually different from what we're about to lock
            gbm_surface_release_buffer(egl.get_gbm_surface(), frame_ctx.previous_bo);
            frame_ctx.previous_bo = nullptr;
            frame_ctx.previous_bo_handle = 0;
        }

        // Clean up old framebuffers that are no longer in use
        // Keep only framebuffers for buffers we might reuse (limit cache to 4 for better stability)
        constexpr size_t MAX_FB_CACHE = 4;
        while (frame_ctx.fb_cache.size() > MAX_FB_CACHE) {
            // Remove oldest entry if it's not the current framebuffer
            auto oldest = frame_ctx.fb_cache.begin();
            uint32_t old_fb_id = oldest->second;
            if (old_fb_id != frame_ctx.current_fb_id) {
                drmModeRmFB(display.get_fd(), old_fb_id);
            }
            frame_ctx.fb_cache.erase(oldest);
        }

        // Now lock the front buffer (should succeed since we just released one)
        struct gbm_bo* bo = gbm_surface_lock_front_buffer(egl.get_gbm_surface());
        if (!bo) {
            frame_ctx.consecutive_buffer_failures++;
            std::cerr << "Failed to lock front buffer! GPU memory may be exhausted." << std::endl;
            std::cerr << "  Consecutive failures: " << frame_ctx.consecutive_buffer_failures << std::endl;
            std::cerr << "  This usually means GBM buffer pool is exhausted." << std::endl;

            // If we've had too many consecutive failures, force recovery
            if (frame_ctx.consecutive_buffer_failures > 5) {
                std::cerr << "CRITICAL: Too many consecutive buffer failures - attempting recovery" << std::endl;
                // Force cleanup of all cached framebuffers
                for (auto& pair : frame_ctx.fb_cache) {
                    if (pair.second != frame_ctx.current_fb_id) {
                        drmModeRmFB(display.get_fd(), pair.second);
                    }
                }
                frame_ctx.fb_cache.clear();
                // Re-add current framebuffer if we have one
                if (frame_ctx.current_fb_id != 0) {
                    // We can't recover the handle, so we'll lose this framebuffer
                    // But it's better than freezing
                }
                if (frame_ctx.previous_bo != nullptr) {
                    gbm_surface_release_buffer(egl.get_gbm_surface(), frame_ctx.previous_bo);
                    frame_ctx.previous_bo = nullptr;
                    frame_ctx.previous_bo_handle = 0;
                }
                frame_ctx.consecutive_buffer_failures = 0;  // Reset counter after recovery
                std::cerr << "Recovery complete - cleared framebuffer cache" << std::endl;
            }

            // Sleep a bit and try again next frame
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            return;
        }

        // Successfully locked buffer - reset failure counter
        frame_ctx.consecutive_buffer_failures = 0;

        uint32_t bo_handle = gbm_bo_get_handle(bo).u32;
        uint32_t fb_id = 0;

        // Check if we already have a framebuffer for this buffer handle
        auto it = frame_ctx.fb_cache.find(bo_handle);
        if (it != frame_ctx.fb_cache.end()) {
            // Reuse existing framebuffer
            fb_id = it->second;
        } else {
            // Create new framebuffer for this buffer
            uint32_t handles[4] = {0};
            uint32_t strides[4] = {0};
            uint32_t offsets[4] = {0};

            handles[0] = bo_handle;
            strides[0] = gbm_bo_get_stride(bo);
            offsets[0] = 0;

            // Use the format we set when creating the GBM surface (GBM_FORMAT_XRGB8888)
            uint32_t format = GBM_FORMAT_XRGB8888;

            // Try AddFB2 first (modern API)
            int ret = drmModeAddFB2(display.get_fd(), mode.width, mode.height, format,
                                    handles, strides, offsets, &fb_id, 0);
            if (ret != 0) {
                // Fallback to AddFB (legacy API)
                ret = drmModeAddFB(display.get_fd(), mode.width, mode.height, 24, 32,
                                  strides[0], handles[0], &fb_id);
                if (ret != 0 && frame_ctx.first_frame) {
                    std::cerr << "AddFB2 failed (ret=" << ret << "), AddFB also failed (ret=" << ret << ")" << std::endl;
                }
            }

            if (ret == 0 && fb_id != 0) {
                // Cache the framebuffer (cache is cleaned up above, so just add it)
                frame_ctx.fb_cache[bo_handle] = fb_id;
            } else {
                std::cerr << "ERROR: Failed to create DRM framebuffer (ret=" << ret << "): " << strerror(errno) << std::endl;
                std::cerr << "  width=" << mode.width << ", height=" << mode.height << std::endl;
                std::cerr << "  stride=" << strides[0] << ", handle=" << handles[0] << std::endl;
                std::cerr << "  fb_cache size=" << frame_ctx.fb_cache.size() << std::endl;
                gbm_surface_release_buffer(egl.get_gbm_surface(), bo);
                return;  // Skip this frame if framebuffer creation failed
            }
        }

        frame_ctx.current_fb_id = fb_id;
        
        // Present the framebuffer
        if (fb_id != 0) {
            if (frame_ctx.first_frame || frame_ctx.force_setcrtc_frames > 0) {
                // Use SetCrtc for first frame and for several frames after reset
                // This helps stabilize after RetroArch returns control
                uint32_t connector_id = display.get_connector_id();
                if (frame_ctx.first_frame) {
                    std::cout << "Setting initial CRTC: fb_id=" << fb_id << ", crtc_id=" << display.get_crtc_id() << std::endl;
                }
                int ret = drmModeSetCrtc(display.get_fd(), display.get_crtc_id(), fb_id, 0, 0,
                                       &connector_id, 1, &mode_info);
                if (ret == 0) {
                    if (frame_ctx.first_frame) {
                        std::cout << "Initial CRTC set successfully!" << std::endl;
                        frame_ctx.first_frame = false;
                    }
                    if (frame_ctx.force_setcrtc_frames > 0) {
                        frame_ctx.force_setcrtc_frames--;
                    }
                } else {
                    std::cerr << "Failed to set CRTC (ret=" << ret << "): " << strerror(errno) << std::endl;
                }
            } else {
                // Subsequent frames: use page flip
                // Static: persists across frames intentionally for DRM page flip callback
                static PageFlipContext flip_ctx;
                flip_ctx.waiting_for_flip = true;

                int ret = drmModePageFlip(display.get_fd(), display.get_crtc_id(), fb_id,
                                         DRM_MODE_PAGE_FLIP_EVENT, &flip_ctx);
                if (ret != 0) {
                    // Page flip failed - increment counter and log periodically
                    frame_ctx.page_flip_failures++;
                    int err = errno;
                    if (frame_ctx.page_flip_failures % 10 == 0 || frame_ctx.page_flip_failures < 5) {
                        std::cerr << "Warning: Page flip failed (ret=" << ret << ", errno=" << err << ": " << strerror(err) << ")" << std::endl;
                        std::cerr << "  (Failures: " << frame_ctx.page_flip_failures << ", Successes: " << frame_ctx.successful_page_flips << ")" << std::endl;
                    }

                    // If page flip fails, fall back to SetCrtc
                    uint32_t connector_id = display.get_connector_id();
                    ret = drmModeSetCrtc(display.get_fd(), display.get_crtc_id(), fb_id, 0, 0,
                                       &connector_id, 1, &mode_info);
                    if (ret != 0) {
                        std::cerr << "Failed to set CRTC: " << strerror(errno) << std::endl;
                    }
                } else {
                    // Page flip succeeded - wait for flip to complete
                    drmEventContext evctx = {};
                    evctx.version = 2;
                    evctx.page_flip_handler = page_flip_handler;

                    fd_set fds;
                    FD_ZERO(&fds);
                    FD_SET(display.get_fd(), &fds);

                    // Wait with timeout (e.g. 100ms) to avoid hanging if event is lost
                    struct timeval timeout;
                    timeout.tv_sec = 0;
                    timeout.tv_usec = 100000; // 100ms

                    while (flip_ctx.waiting_for_flip) {
                        int sret = select(display.get_fd() + 1, &fds, NULL, NULL, &timeout);
                        if (sret > 0) {
                            drmHandleEvent(display.get_fd(), &evctx);
                        } else {
                            // Timeout or error
                            if (sret == 0) std::cerr << "Warning: Page flip wait timed out" << std::endl;
                            break;
                        }
                    }

                    // Reset failure counter periodically
                    frame_ctx.successful_page_flips++;
                    if (frame_ctx.successful_page_flips >= 100) {
                        // Reset page flip failure counter every 100 successful flips
                        if (frame_ctx.page_flip_failures > 0) {
                            std::cout << "Page flip recovery: " << frame_ctx.page_flip_failures
                                      << " failures in last " << frame_ctx.successful_page_flips << " frames" << std::endl;
                        }
                        frame_ctx.page_flip_failures = 0;
                        frame_ctx.successful_page_flips = 0;
                    }
                }
            }
        }

        // Save this buffer to be released on the next frame
        // After we present the next frame, this one will be safe to release
        // We only keep 2 buffers: current (being scanned) and previous (just finished)
        frame_ctx.previous_bo = bo;
        frame_ctx.previous_bo_handle = bo_handle;
    };
    
    // Initialize resolution rendering state
    bool mode_applied = false;
    
    // Display mode was already set during initialization (before intro video).
    // Skip redundant mode probe/switch here to avoid disrupting the intro video
    // with mode changes that cause the TV to lose sync.
    // The initial mode set (lines 98-119) handles the preferred resolution cascade:
    //   1280x720 -> 1024x768 -> 640x480 -> auto-detect
    {
        auto current = display.get_current_mode();
        std::cout << "Display mode (already set): " << current.width << "x" << current.height << std::endl;
        mode_applied = true;
    }
    
    // Fallback if requested mode failed
    if (!mode_applied) {
        std::cout << "Resolution set failed. Attempting fallback to 640x480..." << std::endl;
        if (!display.set_mode(640, 480)) {
            std::cout << "Fallback failed. Attempting Auto/Preferred mode..." << std::endl;
            display.set_mode(0, 0);
        }
    }

    // Update renderers with actual mode obtained
    mode = display.get_current_mode();
    std::cout << "Final Display Mode: " << mode.width << "x" << mode.height << " @ " << (mode.refresh/1000.0) << "Hz" << std::endl;
    gst_renderer.set_screen_size(mode.width, mode.height);

    // Tell the UI renderer what the *actual* HDMI framebuffer size is.
    // This is distinct from the call to resize_screen() below — that
    // sets the *logical* UI canvas (forced to 640×480 in CRT_NATIVE
    // for chunky text/layout). The enhanced CRT pipeline needs both:
    // the framebuffer size for the scene-FBO + composite viewport,
    // the logical size for projection/layout. Without this call, the
    // enhanced CRT composite renders into a 640×480 region in the
    // bottom-left of the framebuffer instead of filling the screen.
    ui_renderer.set_framebuffer_size(mode.width, mode.height);

    // Force a LOGICAL layout canvas independent of the physical mode.
    //
    // CRT Native: 640x480 — large text, correct aspect under the
    // anamorphic squeeze. Long-shipped, unchanged.
    //
    // Modern TV: 1280x720 even when the panel is driven at 1920x1080.
    // Every layout constant in the app (theme.cpp fonts/margins, the
    // Media Browser's literal 1280s) was authored against 720p; holding
    // the logical canvas there means 1080p is a pure resolution
    // increase, not a re-layout. The scale is exactly 1.5x in both axes
    // with identical 16:9 aspect, so proportions are preserved bit for
    // bit. Passing mode.width/mode.height here instead would shrink
    // every menu element to 2/3 of its intended screen fraction.
    {
        const bool crt = (state.display_settings.mode == app::DisplayMode::CRT_NATIVE);
        const config::display::Size canvas = config::display::logical_canvas(crt);
        ui_renderer.resize_screen(canvas.w, canvas.h);
    }
    
    // Track current mode to detect changes at runtime
    app::DisplayMode current_display_mode = state.display_settings.mode;

    // One-shot audio output reapply: move GStreamer's stream to the correct sink
    // PulseAudio default sink is set in init_audio.sh, but GStreamer may still
    // connect to the wrong sink. We move the stream once after playback starts.
    bool audio_stream_move_pending = (state.audio_settings.output != app::AudioOutput::AUTO);

#ifdef MEDIA_BROWSER_ENABLED
    // Track previous Layer 3 state to detect drops (true → false transition)
    // and surface a toast. Recovery is silent — operators don't need a
    // notification when things start working again.
    bool prev_vpn_healthy = state.media_browser_vpn_healthy;
#endif

    while (running) {
#ifdef HAVE_SYSTEMD
        sd_notify(0, "WATCHDOG=1");
#endif

        // ── Invariant: kiosk overlays don't exist inside the Media Browser ─
        // Kiosk Settings menu / on-screen keyboard / pairing screen are
        // overlays that visually live above the kiosk's main playlist UI.
        // While the Media Browser owns the screen they're invisible — but
        // still rendering every frame, consuming GPU/CPU, and the
        // renderer that advances close()'s animation is itself skipped
        // (main.cpp:3014 skips ui_renderer.render(state) for MB), so a
        // close() call would leave the menu stuck in is_closing_ forever.
        // Force-close (teleport, no animation) whenever MB is active.
        //
        // NOTE: this guard is intentionally MB-only. Main-kiosk playlist
        // playback is governed by the operator's `ui_visible_when_playing`
        // preference (legacy behavior) — they may want to pop Settings
        // open while a playlist video plays, and the renderer DOES draw
        // it there, so close() works normally and this invariant should
        // not interfere.
#ifdef MEDIA_BROWSER_ENABLED
        if (state.current_screen == app::AppScreen::MediaBrowser) {
            if (settings_menu.is_active()
                || settings_menu.is_opening()
                || settings_menu.is_closing()) {
                settings_menu.force_close();
            }
            if (keyboard.is_active()) {
                keyboard.close();
            }
        }
#endif

        // Phone Remote — refresh the active text-input pointer each frame.
        // Whichever VirtualKeyboard is currently is_active() becomes the
        // destination for phone-typed characters via poll_text_input_queue
        // below. This is the only place the pointer is set/cleared, so
        // the spec's "single source of truth" invariant holds.
        state.active_text_keyboard = nullptr;
        state.active_text_title    = "";
#ifdef MEDIA_BROWSER_ENABLED
        if (state.current_screen == app::AppScreen::MediaBrowser
            && current_mb_screen == media_browser::ui::Screen::Search
            && mb_search.is_keyboard_active()) {
            state.active_text_keyboard = &mb_search.keyboard();
            state.active_text_title    = "Search movies";
        } else
#endif
        if (keyboard.is_active()) {
            // The kiosk's main VirtualKeyboard (Wi-Fi password etc.).
            state.active_text_keyboard = &keyboard;
            state.active_text_title    = keyboard.get_title();
        }

        // Mirror the settings / game-browser cursor into AppState so the
        // StatusWriter can publish it (kiosk_status.json). Read-only copy;
        // does not affect menu behavior. Enables closed-loop test automation.
        // Only populate the detail fields (incl. the string label, which
        // allocates) WHILE the settings menu is open — otherwise this ran a
        // per-frame heap allocation at 60fps for data no reader uses when
        // settings is closed. Consumers gate on sm_active first.
        state.sm_active = settings_menu.is_active();
        if (state.sm_active) {
            state.sm_highlighted_label     = settings_menu.get_current_highlighted_label();
            state.sm_selected_index        = settings_menu.get_selected_index();
            state.sm_game_browser_active   = settings_menu.is_game_browser_active();
            state.sm_viewing_games         = settings_menu.is_viewing_games_in_playlist();
            state.sm_game_browser_selected = settings_menu.get_game_browser_selected();
            state.sm_game_playlist_index   = settings_menu.get_current_game_playlist_index();
            state.sm_selected_game_index   = settings_menu.get_selected_game_in_playlist();
        } else if (!state.sm_highlighted_label.empty()) {
            // Clear stale data once on close (clear() keeps capacity, no alloc).
            state.sm_highlighted_label.clear();
            state.sm_game_browser_active = false;
            state.sm_viewing_games = false;
        }

        // Skip rendering if display is cleaned up (RetroArch is running)
        if (display.get_fd() < 0) {
            // Display is closed - RetroArch has taken over
            // Still poll GPIO so restart button works during gameplay
            gpio.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        // Check for display reset signal (e.g. after returning from RetroArch)
        if (state.reset_display) {
            std::cout << "Resetting display state after external application..." << std::endl;

            // Re-acquire DRM master (in case it was dropped or stolen)
            if (!display.acquire_master()) {
                std::cerr << "Warning: Failed to re-acquire DRM master" << std::endl;
            }

            // Force mode restoration (RetroArch might have changed resolution)
            if (!display.set_mode(mode.width, mode.height)) {
                std::cerr << "Warning: Failed to restore display mode: " << mode.width << "x" << mode.height << std::endl;
            } else {
                std::cout << "Restored display mode: " << mode.width << "x" << mode.height << std::endl;
            }

            // Reset all frame presentation state (framebuffers, GBM buffers, counters)
            frame_ctx.reset(display.get_fd(), egl.get_gbm_surface());
            state.reset_display = false;
            
            // CRITICAL: Re-make EGL context current after RetroArch released it
            // RetroArch uses its own EGL/DRM context, so we need to restore ours
            if (!egl.make_current()) {
                std::cerr << "Warning: Failed to re-make EGL context current after RetroArch exit" << std::endl;
            } else {
                std::cout << "EGL context restored after RetroArch exit" << std::endl;
            }
            
            // CRITICAL: Reset GstRenderer GL resources after context restore
            // RetroArch invalidates our textures, shaders, VAOs etc. when it takes over EGL
            // This triggers lazy re-initialization on the next video frame render
            gst_renderer.reset_gl();

            // CRITICAL CHECK: Has the player been cleaned up?
            if (!player.is_initialized()) {
                std::cout << "Re-initializing GStreamer player and linking renderer..." << std::endl;
                // Use default initialization as done in main()
                if (!player.initialize()) {
                     std::cerr << "Failed to re-initialize GStreamer player!" << std::endl;
                }
                // Re-link renderer to the new pipeline/appsink
                if (!gst_renderer.initialize(&player)) {
                    std::cerr << "Failed to re-initialize GStreamer renderer!" << std::endl;
                }
            }

            // Restore audio output after RetroArch
            // 1. Set PulseAudio default sink (for any non-GStreamer streams)
            std::cout << "Restoring audio output after display reset..." << std::endl;
            state.audio_settings.apply_output();

            // 2. Set pulsesink device directly on GStreamer pipeline
            // This bypasses PulseAudio default sink which can be overridden by
            // module-switch-on-port-available or module-default-device-restore
            {
                std::string pulse_device = state.audio_settings.resolve_output_sink();
                if (!pulse_device.empty()) {
                    player.set_audio_device(pulse_device);
                }
            }

            // CRITICAL: Also reset UI Renderer GL resources
            // The UI shaders, VAO, VBO, and logo texture also become invalid
            ui_renderer.reset_gl();
            
            // Force an immediate clear to black to ensure screen is in known state
            glViewport(0, 0, mode.width, mode.height);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            if (!egl.swap_buffers()) {
                 std::cerr << "Error: Initial swap buffers after reset failed!" << std::endl;
            } else {
                 std::cout << "Initial swap buffers after reset success." << std::endl;
            }
        }
        
        // Check for display mode changes from Settings Menu
        if (state.display_settings.mode != current_display_mode) {
            std::cout << "Display Mode changed! Switching resolution..." << std::endl;
            current_display_mode = state.display_settings.mode;
            
            // RESOLUTION CHANGES REQUIRE A RESTART.
            //
            // The GBM surface and its EGL window surface are created once
            // at boot, sized to the boot mode, and never recreated; the
            // DRM framebuffer cache is keyed on GBM buffer handles, and
            // the drmModeModeInfo passed to drmModeSetCrtc is snapshotted
            // once. Changing the CRTC mode underneath all of that renders
            // GL into a buffer whose dimensions no longer match what the
            // CRTC scans out — a torn or offset picture, with NO error
            // logged anywhere. This was latent before because both toggle
            // branches happened to land back on 1280x720; now that
            // MODERN_TV boots 1920x1080 and CRT_NATIVE stays at 720p, a
            // live toggle really would change resolution.
            //
            // So apply every UI-side change immediately (logical canvas,
            // letterbox, bezel) and defer ONLY the mode switch. The
            // setting is already persisted by the settings menu, so a
            // restart completes it. Doing the full teardown here
            // (frame_ctx.reset + GBM/EGL recreate + reset_gl on both
            // renderers + re-snapshot mode_info) is possible, but it is
            // by far the riskiest change available here and buys only the
            // avoidance of one restart.
            const config::display::Size want_mode =
                config::display::target_drm_mode(
                    current_display_mode == app::DisplayMode::CRT_NATIVE);
            const bool resolution_would_change =
                (static_cast<int>(mode.width)  != want_mode.w ||
                 static_cast<int>(mode.height) != want_mode.h);

            bool ok = false;
            if (resolution_would_change) {
                LOG_INFO("Display mode -> {}; output resolution {}x{} -> {}x{} "
                         "deferred to next restart",
                         current_display_mode == app::DisplayMode::CRT_NATIVE
                             ? "CRT_NATIVE" : "MODERN_TV",
                         mode.width, mode.height, want_mode.w, want_mode.h);
                ui::Toast::show("Restart to apply the new output resolution");
                // Fall through with ok=false: the mode is untouched, but
                // the UI-side updates below still need to run so the
                // logical canvas / letterbox / bezel match the new mode.
            } else if (current_display_mode == app::DisplayMode::CRT_NATIVE) {
                // Already at the CRT resolution — re-assert it defensively.
                ok = display.set_mode(want_mode.w, want_mode.h);
            } else {
                // Already at the Modern TV resolution.
                ok = display.set_mode(want_mode.w, want_mode.h);
            }

            if (ok) {
                // Update mode info and notify renderers
                mode = display.get_current_mode();
                std::cout << "Switched to: " << mode.name << " (" << mode.width << "x" << mode.height << ")" << std::endl;

                gst_renderer.set_screen_size(mode.width, mode.height);
                // Keep the enhanced-CRT composite path's framebuffer
                // dims in sync with the new mode — same reason as the
                // matching call at boot. Without this, toggling
                // CRT_NATIVE ↔ MODERN_TV at runtime would leave the
                // scene FBO sized for the previous mode.
                ui_renderer.set_framebuffer_size(mode.width, mode.height);
            }

            // The LOGICAL canvas follows the display mode even when the
            // physical mode change was deferred to a restart — CRT_NATIVE
            // draws into 640x480 and MODERN_TV into 1280x720 regardless
            // of the framebuffer behind them. That is the whole point of
            // the logical/physical split, and it means the operator sees
            // the new mode's layout immediately; only the output
            // resolution waits for the restart.
            {
                const bool crt =
                    (current_display_mode == app::DisplayMode::CRT_NATIVE);
                const config::display::Size canvas =
                    config::display::logical_canvas(crt);
                ui_renderer.resize_screen(canvas.w, canvas.h);
            }

            // Ensure viewport is reset
            glViewport(0, 0, mode.width, mode.height);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        
        // uint64_t (was uint32_t) — at 60 fps a uint32_t overflows after
        // ~828 days, which is a real concern for a kiosk deployed in
        // permanent installations. Two consumers depend on this counter:
        // the every-other-frame controller.update_state branch at the
        // bottom of the loop (frame_count % 2), and the first-frame
        // log at swap-buffers (frame_count == 0). Both would behave
        // erratically for a few microseconds at the wraparound point.
        // uint64_t pushes the overflow horizon out beyond the lifespan
        // of the universe, which is good enough.
        static uint64_t frame_count = 0;
        auto now = std::chrono::steady_clock::now();
        auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_frame).count();
        last_frame = now;
        
        // One-shot: move audio stream to correct sink once playback starts
        // GStreamer may connect to the wrong PulseAudio sink despite default being set
        if (audio_stream_move_pending && state.video_active) {
            state.audio_settings.apply_output();
            audio_stream_move_pending = false;
        }

#ifdef MEDIA_BROWSER_ENABLED
        // Toast on Layer 3 drops. Suppressed during initial Unknown→state
        // settling because prev_vpn_healthy is initialized to the boot value.
        if (vpn_health_monitor) {
            bool now = state.media_browser_vpn_healthy;
            if (prev_vpn_healthy && !now) {
                ui::Toast::show("Media Browser unavailable — VPN tunnel down");
            }
            prev_vpn_healthy = now;
        }
#endif

        // Update seek bar timer (countdown to auto-hide)
        if (state.seek_bar_timer > 0.0) {
            state.seek_bar_timer -= delta / 1000.0;  // delta is in ms
            if (state.seek_bar_timer <= 0.0) {
                state.seek_bar_timer = 0.0;
                state.show_seek_bar = false;
            }
        }

        // Update menu state (Wi-Fi scanning, etc.)
        settings_menu.update();

        // Recover the phone-remote virtual gamepad if the web service
        // restarted (Restart=always → new /dev/input node). Throttled to
        // ~every 3s; only ever touches the "MagicDingus Phone Remote"
        // device, so it can't disturb real controllers. Cheap no-op when
        // the remote is already healthy or absent.
        {
            static auto last_remote_reprobe = std::chrono::steady_clock::now();
            auto now_rp = std::chrono::steady_clock::now();
            if (now_rp - last_remote_reprobe >= std::chrono::seconds(3)) {
                last_remote_reprobe = now_rp;
                input.reprobe_phone_remote();
            }
        }

        // Poll input
        auto input_events = input.poll();

        // Poll GPIO (buttons, encoder) and merge with controller/keyboard events
        if (gpio.is_available()) {
            auto gpio_events = gpio.poll();
            input_events.insert(input_events.end(), gpio_events.begin(), gpio_events.end());
        }

#ifdef MEDIA_BROWSER_ENABLED
        // Feed the Media Browser unlock sequence detector. Chord wins over
        // single-button events; otherwise we translate the first matching
        // GPIO/rotary press edge of this tick into a SeqInput.
        //
        // IMPORTANT: when the sequence detector matches an event (PROGRESS
        // or UNLOCKED), that event is CONSUMED — removed from input_events
        // so the normal dispatch loop below does not also fire its action.
        // Otherwise entering the sequence would double-fire playback
        // controls (e.g. triple BTN2 toggles play/pause three times, and
        // the final RCLICK fires SELECT). Unmatched events (NO_MATCH)
        // pass through normally, so random button use is unaffected.
        {
            using namespace std::chrono;
            auto seq_now = steady_clock::now();
            platform::SeqInput seq_ev = platform::SeqInput::NONE;
            std::optional<size_t> consumed_event_idx;

            // Edge-detect the chord: gpio.is_chord_btn1_btn3() is a level
            // check, so feeding the detector every frame while the chord is
            // held would reset the state machine (BTN1_BTN3_CHORD is only
            // expected at step 0, so step>=1 would get reset). Only fire on
            // the false -> true transition.
            static bool chord_was_active = false;
            bool chord_now = gpio.is_available() && gpio.is_chord_btn1_btn3();
            if (chord_now && !chord_was_active) {
                seq_ev = platform::SeqInput::BTN1_BTN3_CHORD;
                // For the chord case we don't track a single index; if the
                // detector matches we'll sweep input_events and remove both
                // PREV and NEXT press events from this frame.
            } else if (!chord_now) {
                for (size_t i = 0; i < input_events.size(); ++i) {
                    const auto& e = input_events[i];
                    if (!e.pressed) continue;
                    platform::SeqInput mapped = platform::SeqInput::NONE;
                    switch (e.action) {
                        case platform::InputAction::PREV:
                            mapped = platform::SeqInput::BTN1_PRESS;
                            break;
                        case platform::InputAction::PLAY_PAUSE:
                            mapped = platform::SeqInput::BTN2_PRESS;
                            break;
                        case platform::InputAction::NEXT:
                            mapped = platform::SeqInput::BTN3_PRESS;
                            break;
                        case platform::InputAction::SETTINGS_MENU:
                            mapped = platform::SeqInput::BTN4_PRESS;
                            break;
                        case platform::InputAction::SELECT:
                            // Only the rotary-encoder push switch counts as
                            // ROTARY_CLICK; the A button / keyboard Enter
                            // never set this flag.
                            if (e.is_from_rotary) {
                                mapped = platform::SeqInput::ROTARY_CLICK;
                            }
                            break;
                        default:
                            break;
                    }
                    if (mapped != platform::SeqInput::NONE) {
                        seq_ev = mapped;
                        consumed_event_idx = i;
                        break;  // only feed one sequence input per frame
                    }
                }
            }

            if (seq_ev != platform::SeqInput::NONE) {
                auto seq_result = seq_detector.feed(seq_ev, seq_now);
                if (seq_result != platform::SeqResult::NO_MATCH) {
                    // Consume the matched input so the normal dispatch loop
                    // below does not also fire its action (prev/next,
                    // play/pause, select, etc.).
                    if (seq_ev == platform::SeqInput::BTN1_BTN3_CHORD) {
                        // The chord is the combination of PREV + NEXT
                        // presses arriving this frame; remove both.
                        input_events.erase(
                            std::remove_if(input_events.begin(), input_events.end(),
                                [](const platform::InputEvent& e) {
                                    return e.pressed &&
                                           (e.action == platform::InputAction::PREV ||
                                            e.action == platform::InputAction::NEXT);
                                }),
                            input_events.end());
                    } else if (consumed_event_idx.has_value() &&
                               *consumed_event_idx < input_events.size()) {
                        input_events.erase(input_events.begin() + *consumed_event_idx);
                    }
                }
                if (seq_result == platform::SeqResult::UNLOCKED) {
                    state.media_browser_unlocked = true;
                    app::SettingsPersistence::save_settings(state);
                    ui::Toast::show("Movie section unlocked");
                    LOG_INFO("Media Browser: secret sequence matched, feature unlocked");
                }
            }

            // Update chord edge-detector state for next frame
            chord_was_active = chord_now;
        }
#endif


        // Track Menu button state for volume control
        struct MenuHoldState {
            bool button_held = false;
            bool volume_changed_while_held = false;
            std::chrono::steady_clock::time_point press_time;
        };
        static MenuHoldState menu_hold;

        // Time-based check for showing slider (if held long enough)
        if (menu_hold.button_held && !state.show_volume_slider) {
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - menu_hold.press_time).count();
            if (duration > 300) {
                state.show_volume_slider = true;
            }
        }
        
#ifdef MEDIA_BROWSER_ENABLED
        // Media Browser screen dispatcher (Task 17). When the user is in
        // the Media Browser, the active MbScreen owns all input: it may
        // consume events, transition to a sibling screen, or return
        // Screen::Exit to hand control back to the main kiosk UI.
        // Input events are NOT forwarded to the main input-handling loop
        // below, which prevents stray Menu / DPad / Select events from
        // leaking into the main UI while the Media Browser is active.
        if (state.current_screen == app::AppScreen::MediaBrowser) {
            // Restore the main menu's renderable state on ANY exit from
            // the Media Browser (used by all three exit paths below:
            // exit modal, BTN4 long-press, Screen::Exit). Belt-and-braces
            // companion to the same reset done at MB entry: if playing-item
            // indexes or a stale UI fade survive into MainMenu, the
            // Renderer either early-returns (is_transitioning: indexes set
            // + no video) or draws at alpha 0 — both look like a
            // permanently blank main menu. Idempotent; matches the
            // RetroArch return path's "CRITICAL: Reset playback state".
            auto reset_main_ui_state = [&state]() {
                state.video_active = false;
                state.is_switching_playlist = false;
                state.current_playlist_index = -1;
                state.current_item_index = -1;
                state.is_fading = false;
                state.ui_visible_when_playing = false;
            };

            // Controller-free navigation on the kiosk enclosure:
            // synthesize vertical nav from BTN1 (yellow/PREV) and BTN3
            // (green/NEXT) so users can reach every UI element with only
            // the 4 hardware buttons + rotary encoder (no USB controller
            // needed). A plugged-in controller still works — its DPad
            // already produces ROTATE_VERTICAL natively.
            //
            //   BTN1 press -> ROTATE_VERTICAL delta=-1  (up)
            //   BTN3 press -> ROTATE_VERTICAL delta=+1  (down)
            //
            // The original PREV/NEXT events also remain in the queue; no
            // screen currently consumes them, but if one ever does the
            // synth is additive, not destructive.
            {
                std::vector<platform::InputEvent> synth_events;
                synth_events.reserve(input_events.size());
                for (const auto& e : input_events) {
                    if (!e.pressed) continue;
                    if (e.action == platform::InputAction::PREV) {
                        platform::InputEvent s{};
                        s.action = platform::InputAction::ROTATE_VERTICAL;
                        s.delta = -1;
                        s.pressed = true;
                        synth_events.push_back(s);
                    } else if (e.action == platform::InputAction::NEXT) {
                        platform::InputEvent s{};
                        s.action = platform::InputAction::ROTATE_VERTICAL;
                        s.delta = +1;
                        s.pressed = true;
                        synth_events.push_back(s);
                    }
                }
                for (auto& s : synth_events) input_events.push_back(std::move(s));
            }

            // BTN4 (SETTINGS_MENU) long-press detection. Short press
            // (< 500ms) forwards a synthetic `pressed` event to the active
            // screen so the existing per-screen "back" logic runs unchanged.
            // Long press (>= 500ms) bypasses the screen entirely and exits
            // the Media Browser back to the kiosk MainMenu. Centralizing
            // this in the dispatcher avoids adding ~10 lines of boilerplate
            // to every screen.
            static std::optional<std::chrono::steady_clock::time_point>
                mb_btn4_pressed_at;
            constexpr auto kLongPressThreshold =
                std::chrono::milliseconds(500);
            bool btn4_long_press_exit = false;
            {
                std::vector<platform::InputEvent> forwarded;
                forwarded.reserve(input_events.size());
                for (const auto& e : input_events) {
                    if (e.action == platform::InputAction::SETTINGS_MENU) {
                        if (e.pressed) {
                            mb_btn4_pressed_at =
                                std::chrono::steady_clock::now();
                            // Don't forward the press yet — wait for the
                            // release to decide short vs long.
                        } else {
                            if (mb_btn4_pressed_at.has_value()) {
                                auto held =
                                    std::chrono::steady_clock::now() -
                                    *mb_btn4_pressed_at;
                                mb_btn4_pressed_at.reset();
                                if (held >= kLongPressThreshold) {
                                    btn4_long_press_exit = true;
                                } else {
                                    // Synthesize the short-press "pressed"
                                    // event expected by the screens' existing
                                    // SETTINGS_MENU handlers.
                                    platform::InputEvent synth = e;
                                    synth.pressed = true;
                                    forwarded.push_back(synth);
                                }
                            }
                            // If we saw a release with no prior press, drop
                            // it — the press happened before we entered the
                            // Media Browser and is no longer meaningful.
                        }
                    } else {
                        forwarded.push_back(e);
                    }
                }
                input_events = std::move(forwarded);
            }

            // === Download stall watchdog — Task 16 ===
            // Tick every frame; the watchdog rate-limits its own service
            // polls internally to ~once per 10s, so the per-frame cost is
            // negligible. Any returned stall events surface as a
            // StallPromptModal (one-at-a-time — additional events stay
            // queued in the watchdog's internal state and the next free
            // frame raises the next one).
            //
            // Modal placement note: this runs BEFORE the exit-modal
            // intercept so the exit modal can take precedence if both
            // happen to be active in the same frame (user pressing BTN2
            // to leave the Marquee shouldn't be hijacked by a late stall
            // event firing the same tick).
            // ── Stall-prompt modal disabled (per user request) ─────────────
            // The DownloadWatchdog used to tick here every frame, polling
            // Radarr + qBit for stalled downloads and surfacing a modal
            // asking the user to pick a different release or snooze. The
            // operator found the modal pesky — it was firing on completed
            // downloads (likely a stale-watch-cleanup bug, not investigated
            // since we're removing the surface anyway). The watchdog object
            // still exists (Detail / ReleasePicker hold references and
            // register watches on grab — those become no-ops without the
            // tick), and the input/render hooks below stay since modal.is_active()
            // returns false forever now, making them cheap no-ops. If you
            // ever want to re-enable: restore the tick + show() block here.

            // === Stall modal intercept ===
            // Traps ALL input while active and consumes the queue so no
            // MB screen sees those events. Mirrors the exit-modal
            // pattern below. Must run BEFORE the exit-modal block — if
            // the stall modal is up, any BTN2 should commit/dismiss the
            // stall prompt, not open a redundant exit-confirm on top.
            if (mb_stall_modal.is_active()) {
                bool consumed = mb_stall_modal.handle_input(input_events);
                if (consumed) {
                    input_events.clear();
                }
            }

            // === Global BTN2 (PLAY_PAUSE) intercept + exit modal ===
            // The exit modal owns BTN2 entirely while the MB is active.
            // When the modal is closed, a BTN2 press opens it.
            // When the modal is open, all input is funnelled to the modal
            // and consumed so no MB screen sees it.
            // On modal commit the teardown mirrors the Screen::Exit path.
            // (Skipped when BTN4 long-press already fired — double-exit
            // would call leave() on the wrong screen pointer.)
            bool mb_modal_exited = false;
            if (!btn4_long_press_exit) {
                for (auto it = input_events.begin(); it != input_events.end(); ) {
                    const auto& e = *it;
                    bool consume = false;

                    if (mb_exit_modal.is_open()) {
                        // Modal owns all recognisable inputs while visible.
                        switch (e.action) {
                            case platform::InputAction::PLAY_PAUSE:
                                if (e.pressed) { mb_exit_modal.on_btn2(); consume = true; }
                                break;
                            case platform::InputAction::PREV:
                                if (e.pressed) { mb_exit_modal.on_btn1(); consume = true; }
                                break;
                            case platform::InputAction::NEXT:
                                if (e.pressed) { mb_exit_modal.on_btn3(); consume = true; }
                                break;
                            case platform::InputAction::SETTINGS_MENU:
                                if (e.pressed) { mb_exit_modal.on_btn4(); consume = true; }
                                break;
                            case platform::InputAction::SELECT:
                                if (e.pressed) { mb_exit_modal.on_select(); consume = true; }
                                break;
                            case platform::InputAction::ROTATE_VERTICAL:
                            case platform::InputAction::ROTATE:
                                if (e.delta != 0) { mb_exit_modal.on_rotate(e.delta); consume = true; }
                                break;
                            default:
                                break;
                        }
                    } else if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed
                               && current_mb_screen != media_browser::ui::Screen::Playback) {
                        mb_exit_modal.open();
                        consume = true;
                    }

                    if (consume) {
                        it = input_events.erase(it);
                    } else {
                        ++it;
                    }
                }

                // Handle modal commit result before dispatching remaining
                // events to the active screen.
                auto modal_result = mb_exit_modal.last_result();
                if (modal_result == media_browser::ui::ExitModal::Result::Exit) {
                    // Tear down: same path as Screen::Exit.
                    mb_exit_modal.clear_result();
                    // Guarantee the artwork worker is running on MB exit.
                    // pause() is only paired with resume() in the
                    // screen-transition branch below; exiting via the
                    // modal/long-press/Screen::Exit paths bypasses that,
                    // so a Playback->exit would otherwise leave the
                    // worker paused forever (no posters on next entry).
                    // resume() is idempotent — a no-op when not paused.
                    ui_renderer.artwork_cache().resume();
                    active_mb_screen->leave();
                    reset_main_ui_state();
                    state.current_screen = app::AppScreen::MainMenu;
                    current_mb_screen = media_browser::ui::Screen::Browse;
                    active_mb_screen = &mb_browse;
                    input_events.clear();
                    mb_modal_exited = true;
                } else if (modal_result == media_browser::ui::ExitModal::Result::Cancel) {
                    mb_exit_modal.clear_result();
                }
            }

            if (btn4_long_press_exit) {
                // Long-press exit bypasses the screen entirely. Mirror the
                // Screen::Exit return path below: leave the current screen,
                // reset dispatcher state, and let the rest of the main loop
                // run this frame (rendering, etc.) with current_screen flipped
                // to MainMenu.
                ui_renderer.artwork_cache().resume();  // un-stick if exiting Playback
                active_mb_screen->leave();
                reset_main_ui_state();
                state.current_screen = app::AppScreen::MainMenu;
                current_mb_screen = media_browser::ui::Screen::Browse;
                active_mb_screen = &mb_browse;
                input_events.clear();
            } else if (!mb_modal_exited) {
            auto next = active_mb_screen->handle_input(input_events);
            if (next == media_browser::ui::Screen::Exit) {
                ui_renderer.artwork_cache().resume();  // un-stick if exiting Playback
                active_mb_screen->leave();
                reset_main_ui_state();
                state.current_screen = app::AppScreen::MainMenu;
                // Reset to Browse so the next entry into the Media Browser
                // starts fresh on the landing screen.
                current_mb_screen = media_browser::ui::Screen::Browse;
                active_mb_screen = &mb_browse;
            } else if (next != current_mb_screen) {
                // When transitioning into Detail, forward the selected
                // tmdb_id from whichever source screen produced it so
                // Detail knows which movie to show. Task 20 will fetch
                // the full metadata; for now Detail just remembers the id.
                if (next == media_browser::ui::Screen::Detail) {
                    if (current_mb_screen == media_browser::ui::Screen::Browse) {
                        mb_detail.set_tmdb_id(mb_browse.selected_tmdb_id());
                    } else if (current_mb_screen == media_browser::ui::Screen::Search) {
                        mb_detail.set_tmdb_id(mb_search.selected_tmdb_id());
                    } else if (current_mb_screen == media_browser::ui::Screen::Library) {
                        mb_detail.set_tmdb_id(mb_library.selected_tmdb_id());
                    }
                    // Remember where we came from so BTN4 on Detail
                    // returns the user to the screen that opened it
                    // (preserves Search query/results, Library scroll,
                    // etc.) instead of always landing on Browse.
                    //
                    // Skip when returning from Playback or ReleasePicker
                    // — both are sub-screens of Detail (user opened them
                    // FROM Detail). Detail's origin should still point at
                    // whatever screen opened Detail BEFORE the user
                    // descended into the sub-screen, otherwise BTN4 on
                    // Detail loops the user right back into the
                    // sub-screen they just exited.
                    if (current_mb_screen != media_browser::ui::Screen::Playback &&
                        current_mb_screen != media_browser::ui::Screen::ReleasePicker) {
                        mb_detail.set_origin(current_mb_screen);
                    }
                }
                // Detail -> Playback: copy resolved host path + title from
                // Detail to Playback so it knows what to load on enter().
                // Also pass the rich TMDB metadata so the PlaybackOverlay
                // can show a movie-detail header and pre-fetch similar films.
                if (next == media_browser::ui::Screen::Playback &&
                    current_mb_screen == media_browser::ui::Screen::Detail) {
                    auto pt = mb_detail.get_play_target();
                    mb_playback.set_movie(pt.host_path, pt.title);
                    media_browser::ui::PlaybackOverlayMovieMeta meta;
                    meta.tmdb_id     = pt.tmdb_id;
                    meta.title       = pt.title;
                    meta.year        = pt.year;
                    meta.runtime_min = pt.runtime_min;
                    meta.synopsis    = pt.synopsis;
                    meta.genres      = pt.genres;
                    meta.poster_url  = pt.poster_url;
                    meta.cast        = pt.cast;
                    meta.director    = pt.director;
                    mb_playback.set_movie_meta(std::move(meta));
                }
                // Artwork I/O contention guard: pause the artwork worker
                // when entering Playback so it doesn't compete with
                // GStreamer for read bandwidth on the USB SSD that holds
                // the library file. Resume on the way back out.
                if (next == media_browser::ui::Screen::Playback &&
                    current_mb_screen != media_browser::ui::Screen::Playback) {
                    ui_renderer.artwork_cache().pause();
                } else if (current_mb_screen == media_browser::ui::Screen::Playback &&
                           next != media_browser::ui::Screen::Playback) {
                    ui_renderer.artwork_cache().resume();
                }
                active_mb_screen->leave();
                current_mb_screen = next;
                switch (next) {
                    case media_browser::ui::Screen::Browse:        active_mb_screen = &mb_browse;      break;
                    case media_browser::ui::Screen::Search:        active_mb_screen = &mb_search;      break;
                    case media_browser::ui::Screen::Detail:        active_mb_screen = &mb_detail;      break;
                    case media_browser::ui::Screen::ReleasePicker: active_mb_screen = &mb_release_picker; break;
                    case media_browser::ui::Screen::Queue:         active_mb_screen = &mb_queue;       break;
                    case media_browser::ui::Screen::Library:       active_mb_screen = &mb_library;     break;
                    case media_browser::ui::Screen::Playback:      active_mb_screen = &mb_playback;    break;
                    case media_browser::ui::Screen::MovieSettings: active_mb_screen = &mb_mb_settings; break;
                    case media_browser::ui::Screen::Exit: break;  // handled above
                }

                // Clear the framebuffer to black before the new screen's
                // enter() runs. Some screens' enter() (notably Detail)
                // do synchronous HTTP work that blocks the render loop
                // for 200ms-2s. Without this clear, the user sees the
                // last-rendered frame frozen on screen — and if they
                // came from Playback, that frame can include garbage
                // texture data from the just-stopped GStreamer pipeline
                // (the "green rectangle in upper-right" the user
                // reported). A single black clear gives clean visual
                // continuity into the Loading state.
                glViewport(0, 0, mode.width, mode.height);
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                egl.swap_buffers();

                active_mb_screen->enter();
            }
            active_mb_screen->update();
            // Consume all input events so the main UI's per-event loop
            // below sees an empty queue this frame.
            input_events.clear();
            }  // else if (!mb_modal_exited && !btn4_long_press_exit)
        }
#endif

        for (const auto& ev : input_events) {
            // Handle Menu button hold logic
            if (ev.action == InputAction::SETTINGS_MENU) {
                if (ev.pressed) {
                    menu_hold.button_held = true;
                    menu_hold.volume_changed_while_held = false;
                    menu_hold.press_time = std::chrono::steady_clock::now();
                    state.show_volume_slider = false; // Don't show immediately
                } else {
                    menu_hold.button_held = false;
                    state.show_volume_slider = false; // Hide immediately
                    
                    auto release_time = std::chrono::steady_clock::now();
                    auto hold_duration = std::chrono::duration_cast<std::chrono::milliseconds>(release_time - menu_hold.press_time).count();
                    
                    // Only toggle menu if we didn't change volume AND it was a short press
                    if (!menu_hold.volume_changed_while_held && hold_duration < 300) {
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
                    } else if (menu_hold.volume_changed_while_held) {
                        // Volume was changed, save settings now
                        app::SettingsPersistence::save_settings(state);
                    }
                }
                continue;
            }
            
            // If Menu button is held, hijack Rotate/Up/Down for volume
            if (menu_hold.button_held) {
                if (ev.action == InputAction::ROTATE) {
                    int vol_change = ev.delta * 5; // 5% increments
                    state.master_volume += vol_change;
                    
                    // Clamp volume
                    if (state.master_volume < 0) state.master_volume = 0;
                    if (state.master_volume > 100) state.master_volume = 100;
                    
                    // Apply volume
                    controller.set_system_volume(state.master_volume);
                    
                    // Show slider immediately on interaction and mark as changed
                    state.show_volume_slider = true;
                    menu_hold.volume_changed_while_held = true;
                } else if (ev.action == InputAction::ROTATE_VERTICAL) {
                    // Invert delta for vertical axis (Up = -1 -> Volume Up)
                    int vol_change = -ev.delta * 5; // 5% increments
                    state.master_volume += vol_change;
                    
                    // Clamp volume
                    if (state.master_volume < 0) state.master_volume = 0;
                    if (state.master_volume > 100) state.master_volume = 100;
                    
                    // Apply volume
                    controller.set_system_volume(state.master_volume);
                    
                    // Show slider immediately on interaction and mark as changed
                    state.show_volume_slider = true;
                    menu_hold.volume_changed_while_held = true;
                }
                continue; // Consume event
            }
            
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
                continue; // Consume event if keyboard is active
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
                                    
                                    // Check if "Back" button is selected (last item)
                                    if (game_idx == static_cast<int>(playlist.items.size())) {
                                        std::cout << "Back button selected - returning to playlist list" << std::endl;
                                        settings_menu.exit_game_list();
                                    } else if (game_idx >= 0 && game_idx < static_cast<int>(playlist.items.size())) {
                                        std::cout << "Launching game: " << playlist.items[game_idx].title << std::endl;
                                        
                                        // Set loading state
                                        state.is_loading_game = true;

#ifdef MEDIA_BROWSER_ENABLED
                                        // Quiet the media stack for the whole
                                        // session (async — never delays launch)
                                        // and drop poster textures while the GL
                                        // context is still current.
                                        game_quiet_mode.request_pause();
                                        if (ui_renderer.artwork_cache_initialized()) {
                                            ui_renderer.artwork_cache().pause();
                                            ui_renderer.artwork_cache().clear_textures();
                                        }
#endif

                                        // Create progress callback to keep UI alive during launch
                                        auto progress_callback = [&]() {
                                            // Clear screen
                                            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                                            glClear(GL_COLOR_BUFFER_BIT);

                                            // Render loading screen FIRST so the bezel can layer on top
                                            ui_renderer.render_loading_overlay(state);

                                            // Render bezel overlay LAST so it frames the loading screen
                                            // (same z-order as the main render path at end of frame)
                                            if (state.display_settings.mode == app::DisplayMode::MODERN_TV &&
                                                !state.available_bezels.empty() &&
                                                state.display_settings.bezel_index >= 0 &&
                                                state.display_settings.bezel_index < static_cast<int>(state.available_bezels.size())) {
                                                const auto& bezel = state.available_bezels[state.display_settings.bezel_index];
                                                if (!bezel.file.empty()) {
                                                    ui_renderer.load_bezel(bezel.file);
                                                    glViewport(0, 0, mode.width, mode.height);
                                                    ui_renderer.render_bezel();
                                                }
                                            }

                                            // Swap buffers (renders to GBM surface)
                                            egl.swap_buffers();

                                            // Present frame (flips DRM page)
                                            present_frame();
                                        };
                                        
                                        // Launch the game
                                        // Spawn GPIO polling thread so restart button works during gameplay
                                        std::atomic<bool> game_running{true};
                                        std::thread gpio_poll_thread([&gpio, &game_running]() {
                                            while (game_running.load()) {
                                                gpio.poll();
                                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                            }
                                        });

#ifdef HAVE_SYSTEMD
                                        // Disable watchdog during RetroArch (blocks on waitpid)
                                        sd_notify(0, "WATCHDOG_USEC=0");
#endif
                                        // ── Phone-remote: transition INTO RetroArch ──────────────────────
                                        // The main loop blocks on waitpid() while RetroArch runs, so the
                                        // per-frame deriver never executes.  Set the mode explicitly here
                                        // and flush status so the companion app sees "retroarch" immediately.
                                        {
                                            const auto& game_item = playlist.items[game_idx];
                                            state.retroarch_rom_name = game_item.title;
                                            state.retroarch_core     = game_item.emulator_core;
                                            state.screen_mode.store(app::ScreenMode::RetroArch);
                                            status_writer.write_now(state);
                                        }
                                        // ────────────────────────────────────────────────────────────────

                                        // RAII cleanup guard — runs whether load_playlist_item returns,
                                        // throws, or fails silently. Clears the retroarch fields so
                                        // kiosk_status.json never has stale ROM/core values.
                                        struct RetroArchExitGuard {
                                            app::AppState& state;
                                            app::StatusWriter& writer;
                                            ~RetroArchExitGuard() {
                                                state.retroarch_rom_name.clear();
                                                state.retroarch_core.clear();
                                                state.screen_mode.store(app::ScreenMode::Playlist);
                                                try { writer.write_now(state); } catch (...) {}
                                            }
                                        } ra_guard{state, status_writer};

                                        auto launch_result = controller.load_playlist_item(state, playlist, game_idx, playlist_directory, progress_callback);
                                        // ra_guard destructor runs here on normal return (and on exception)
                                        // ────────────────────────────────────────────────────────────────

#ifdef HAVE_SYSTEMD
                                        // Re-enable watchdog after RetroArch exits (10s = 10000000 usec)
                                        sd_notify(0, "WATCHDOG_USEC=10000000");
#endif

                                        game_running.store(false);
                                        if (gpio_poll_thread.joinable()) {
                                            gpio_poll_thread.join();
                                        }

                                        // Reset loading state
                                        state.is_loading_game = false;

#ifdef MEDIA_BROWSER_ENABLED
                                        if (ui_renderer.artwork_cache_initialized()) {
                                            ui_renderer.artwork_cache().resume();
                                        }
                                        game_quiet_mode.request_resume();
#endif

                                        if (launch_result) {
                                            std::cout << "Game launched successfully" << std::endl;
                                        } else {
                                            std::cout << "Game launch failed: " << launch_result.error() << std::endl;
                                            state.set_error("Unable to start game");
                                        }
                                        // Return to the playlist UI after every launch outcome;
                                        // a failed takeover must not leave the game browser open.
                                        settings_menu.close();
                                    } else {
                                        std::cout << "Invalid game index: " << game_idx << " (max: " << playlist.items.size() << ")" << std::endl;
                                    }
                                } else {
                                    std::cout << "Invalid playlist index: " << playlist_idx << " (max: " << game_playlists.size() << ")" << std::endl;
                                }
                            } else {
                                // Enter selected playlist or go back
                                int selected_playlist = settings_menu.get_game_browser_selected();
                                
                                // Check if "Back" button is selected (last item)
                                if (selected_playlist == static_cast<int>(game_playlists.size())) {
                                    settings_menu.exit_game_browser();
                                } else if (selected_playlist >= 0 && selected_playlist < static_cast<int>(game_playlists.size())) {
                                    settings_menu.enter_game_list(selected_playlist);
                                }
                            }
                            break;
                        }
                        default:
                            break;
                    }
                    continue; // Skip normal menu handling when in game browser
                }
                
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
                            continue; // consumed
                        } else if (ev.action == InputAction::PLAY_PAUSE && ev.pressed) {
                            std::vector<std::string> ids;
                            for (const auto& d : ps_cached_devices) ids.push_back(d.id);
                            ps->forget_selected_device(ids);
                            // No manual cache invalidation needed — Flask
                            // rewrites paired_remotes.json on its next
                            // broadcaster tick (~200 ms), and the cache
                            // mtime-checks every call.
                            continue; // consumed
                        } else if (ev.action == InputAction::QUIT && ev.pressed) {
                            ps->close();
                            settings_menu.close_pairing_screen();
                            continue; // consumed
                        } else if (ev.action == InputAction::SETTINGS_MENU && ev.pressed) {
                            // BTN4 (black) — close the pairing screen and return to settings.
                            ps->close();
                            settings_menu.close_pairing_screen();
                            continue; // consumed
                        }
                    }
                    continue; // eat all other inputs while pairing screen is up
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
                        } else if (section == ui::MenuSection::INFO) {
                            settings_menu.enter_submenu(ui::MenuSection::INFO);
                        } else if (section == ui::MenuSection::PHONE_REMOTE) {
                            settings_menu.open_pairing_screen();
                        } else if (section == ui::MenuSection::BROWSE_GAMES) {
                            // Enter game browser
                            settings_menu.enter_game_browser();
#ifdef MEDIA_BROWSER_ENABLED
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
                            state.video_active = false;
                            state.is_switching_playlist = false;
                            // CRITICAL: also clear the playing-item indexes
                            // and any in-flight UI fade. The Renderer's
                            // is_transitioning logic (current_item_index >= 0
                            // && !video_active) skips the ENTIRE main-UI
                            // render, and a stale is_fading with a hidden
                            // target zeroes the UI alpha — either one leaves
                            // the main menu permanently BLANK after exiting
                            // the Media Browser. Same reset the RetroArch
                            // return path does in Controller (see the
                            // "CRITICAL: Reset playback state" comment there).
                            state.current_playlist_index = -1;
                            state.current_item_index = -1;
                            state.is_fading = false;
                            state.ui_visible_when_playing = false;
                            settings_menu.close();
                            state.current_screen = app::AppScreen::MediaBrowser;
                            // Always start on the Browse landing screen.
                            current_mb_screen = media_browser::ui::Screen::Browse;
                            active_mb_screen = &mb_browse;
                            active_mb_screen->enter();
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
                continue;  // Skip normal input handling when menu is active
            }
            
            // Normal input handling (when menu is not active)
            // Disable UI navigation when video is playing and UI is completely hidden
            bool ui_available = !state.video_active || state.ui_visible_when_playing;
            
            switch (ev.action) {
                case InputAction::QUIT:
                    running = false;
                    break;
                    
                case InputAction::ROTATE:
                case InputAction::ROTATE_VERTICAL:
                    // Only allow navigation when UI is available
                    if (ui_available && !state.playlists.empty()) {
                        // Fixed max visible items (must match renderer's max_visible = 8)
                        // Use the on-screen row count the renderer published last
                        // frame, not a hardcoded 8. Hardcoding 8 (sized for CRT)
                        // forced scroll_offset to advance once the selection hit
                        // index 8 even in Modern TV mode where 14 rows are
                        // visible — the playlist would scroll prematurely and
                        // pop the top row off-screen.
                        int max_visible = std::max(1, state.playlist_max_visible);

                        // Move selection without wrapping (clamp at boundaries)
                        if (ev.delta > 0) {
                            // Moving down - don't go past last item
                            if (state.selected_index < static_cast<int>(state.playlists.size()) - 1) {
                                state.selected_index++;
                            }
                        } else if (ev.delta < 0) {
                            // Moving up - don't go below first item
                            if (state.selected_index > 0) {
                                state.selected_index--;
                            }
                        }
                        
                        // Adjust scroll offset to keep selected item visible
                        // If selection is below visible window, scroll down
                        if (state.selected_index >= state.playlist_scroll_offset + max_visible) {
                            state.playlist_scroll_offset = state.selected_index - max_visible + 1;
                        }
                        // If selection is above visible window, scroll up
                        if (state.selected_index < state.playlist_scroll_offset) {
                            state.playlist_scroll_offset = state.selected_index;
                        }
                        // Clamp scroll offset to valid range
                        int max_scroll = static_cast<int>(state.playlists.size()) - max_visible;
                        if (max_scroll < 0) max_scroll = 0;
                        if (state.playlist_scroll_offset > max_scroll) {
                            state.playlist_scroll_offset = max_scroll;
                        }
                        if (state.playlist_scroll_offset < 0) {
                            state.playlist_scroll_offset = 0;
                        }
                        
                        // If video is active, show UI briefly
                        if (state.video_active) {
                            state.ui_visible_when_playing = true;
                            state.ui_visibility_timer = 3.0; // Show for 3 seconds
                        }
                    } else if (state.video_active && !state.ui_visible_when_playing) {
                        // Seek mode: rotary encoder seeks through video when UI is hidden
                        double velocity = static_cast<double>(ev.velocity);
                        // Exponential curve: slow turn = 5s, fast turn = 30s
                        double seek_seconds = 5.0 + 25.0 * (velocity * velocity);
                        controller.seek(seek_seconds * ev.delta);

                        // Show seek bar and reset auto-hide timer
                        state.show_seek_bar = true;
                        state.seek_bar_timer = 1.5;
                    }
                    break;
                    
                case InputAction::SELECT:
                    if (!ev.pressed) break; // Only trigger on press
                    
                    // Don't allow playlist selection during intro video
                    if (state.showing_intro_video) {
                        break;  // Ignore input during intro
                    }
                    
                    // If video is playing and UI is hidden, just show UI
                    if (state.video_active && !state.ui_visible_when_playing) {
                        state.ui_visible_when_playing = true;
                        state.ui_visibility_timer = 3.0; // Show for 3 seconds
                        break; // Don't trigger selection yet
                    }
                    
                    // If video is already playing
                    if (state.video_active) {
                // Check if the selected playlist is the same as the one currently playing
                // Special case for Master Shuffle (index 0): current_playlist_index points to the source playlist,
                // so we check master_shuffle_active flag instead.
                bool is_same_playlist = (state.current_playlist_index == state.selected_index) || 
                                      (state.master_shuffle_active && state.selected_index == 0);
                                      
                if (is_same_playlist) {
                    // Same playlist: just toggle UI visibility with fade
                    state.ui_visible_when_playing = !state.ui_visible_when_playing;
                    
                    // Start fade animation (synchronized UI and audio)
                    state.fade_start_time = std::chrono::steady_clock::now();
                    state.fade_target_ui_visible = state.ui_visible_when_playing;
                    state.is_fading = true;
                } else {
                    // Different playlist: stop current and start new playlist
                    // Prevent overlapping playlist switches
                    if (state.is_switching_playlist) {
                        break;  // Skip if already switching
                    }
                    
                    state.is_switching_playlist = true;  // Set flag to prevent overlapping operations
                    state.playlist_switch_start_time = std::chrono::steady_clock::now();  // Track when switch started
                    
                    // First, update the playlist index BEFORE stopping to prevent reset
                    state.current_playlist_index = state.selected_index;
                    state.current_item_index = 0;
                    
                    // Reset advance flags when switching playlists to prevent issues
                    state.last_advanced_item_index = -1;
                    state.last_advanced_duration = 0.0;
                    state.original_volume = 100.0;  // Reset to default, will be captured when new video starts
                    
                    // Restore volume to 100% before stopping (in case UI was visible and volume was dimmed)
                    controller.set_volume(100.0);
                    
                    controller.stop();
                    // Wait longer to ensure stop completes and buffers are released
                    // Increased delay to prevent race conditions and buffer export errors
                    // The DRM driver needs time to release GEM buffers from previous video
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    
                    // Verify that mpv actually stopped before proceeding
                    // This prevents race conditions when loading new videos
                    int retry_count = 0;
                    const int max_retries = 10;
                    while (controller.is_playing() && retry_count < max_retries) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        retry_count++;
                    }
                    
                    if (controller.is_playing()) {
                        std::cerr << "Warning: Video did not stop cleanly after " << (max_retries * 50) << "ms, proceeding anyway" << std::endl;
                    }
                    
                    // Then start the new playlist
                    bool load_success = false;
                    
                    // Check for Master Shuffle (index 0)
                    if (state.selected_index == 0) {
                        std::cout << "Master Shuffle selected!" << std::endl;
                        state.master_shuffle_active = true;
                        controller.play_random_global_video(state, playlist_directory);
                        load_success = true; // Assume success for now (play_random_global_video handles retries)
                        
                        // When starting video, hide UI completely so video shows through fully
                        state.ui_visible_when_playing = false;
                    } else if (!state.playlists.empty() && state.selected_index < static_cast<int>(state.playlists.size())) {
                        state.master_shuffle_active = false; // Disable master shuffle for normal playlists
                        const auto& pl = state.playlists[state.selected_index];
                        if (!pl.items.empty() && pl.is_video_playlist()) {
                            // Load first item of new playlist
                            auto load_result = controller.load_playlist_item(state, pl, 0, playlist_directory);
                            load_success = static_cast<bool>(load_result);
                            if (!load_result) {
                                std::cerr << "Failed to load playlist item: " << load_result.error() << std::endl;
                                state.set_error("Could not load: " + pl.title);
                            }
                            if (load_success) {
                                // Only hide UI when video actually loaded
                                state.ui_visible_when_playing = false;
                            }
                        } else if (!pl.items.empty() && pl.is_game_playlist()) {
                            // Game playlists are launched from Settings > Video Games
                            state.set_error("Use Settings to launch games");
                            load_success = false;
                        } else {
                            state.set_error("No content in playlist");
                            load_success = false;
                        }
                    }

                    // If load failed, clear the flag and restore UI state
                    if (!load_success) {
                        state.is_switching_playlist = false;
                        state.ui_visible_when_playing = true;
                        state.video_active = false;
                        std::cerr << "Playlist switch failed - flag cleared, ready for retry" << std::endl;
                    }
                    // Otherwise, the flag will be cleared when the new video becomes active
                    // If video doesn't become active within timeout, flag will be cleared by timeout mechanism
                }
                } else {
                    // No video playing: start the selected playlist
                    // Prevent overlapping playlist switches
                    if (state.is_switching_playlist) {
                        break;  // Skip if already switching
                    }
                    
                    // Check for Master Shuffle (index 0)
                    if (state.selected_index == 0) {
                        std::cout << "Master Shuffle selected (from stopped)!" << std::endl;
                        state.is_switching_playlist = true;
                        state.playlist_switch_start_time = std::chrono::steady_clock::now();
                        state.master_shuffle_active = true;
                        
                        controller.play_random_global_video(state, playlist_directory);
                        
                        // When starting video, hide UI completely so video shows through fully
                        state.ui_visible_when_playing = false;
                        
                        // Note: play_random_global_video handles loading, but doesn't return success/fail
                        // We assume it works or retries.
                    } else if (!state.playlists.empty() && state.selected_index < static_cast<int>(state.playlists.size())) {
                        state.master_shuffle_active = false; // Disable master shuffle for normal playlists
                        const auto& pl = state.playlists[state.selected_index];
                        if (!pl.items.empty() && pl.is_video_playlist()) {
                            state.is_switching_playlist = true;  // Set flag
                            state.playlist_switch_start_time = std::chrono::steady_clock::now();

                            // Load first item of playlist
                            auto load_result = controller.load_playlist_item(state, pl, 0, playlist_directory);
                            if (load_result) {
                                // Track which playlist and item is playing
                                state.current_playlist_index = state.selected_index;
                                state.current_item_index = 0;
                                // When starting video, hide UI completely so video shows through fully
                                state.ui_visible_when_playing = false;
                            } else {
                                // Load failed - clear flag and show error
                                std::cerr << "Failed to load playlist item: " << load_result.error() << std::endl;
                                state.set_error("Could not load: " + pl.title);
                                state.is_switching_playlist = false;
                            }
                        } else if (!pl.items.empty() && pl.is_game_playlist()) {
                            state.set_error("Use Settings to launch games");
                        } else {
                            state.set_error("No content in playlist");
                        }
                    }
                }
                    break;
                    
                case InputAction::PLAY_PAUSE:
                    if (!ev.pressed) break; // Only trigger on press, not release
                    // Only allow play/pause if intro is complete and we have an active video
                    if (state.intro_complete && state.video_active) {
                        controller.toggle_pause();
                    }
                    break;
                    
                case InputAction::NEXT:
                    if (!ev.pressed) break; // Only trigger on press, not release
                    // If video is playing, advance to next playlist item
                    // In Master Shuffle mode, pick another random video
                    // Otherwise, seek forward in current video
                    // Don't allow if we're switching playlists
                    if (!state.is_switching_playlist && state.video_active && state.current_playlist_index >= 0) {
                        if (state.master_shuffle_active) {
                            // In Master Shuffle, NEXT triggers another random video
                            // Save current position to shuffle history for "Previous" support
                            if (state.current_playlist_index >= 0 && state.current_item_index >= 0) {
                                state.push_shuffle_history(state.current_playlist_index, state.current_item_index);
                            }
                            controller.play_random_global_video(state, playlist_directory);
                        } else {
                            controller.load_next_item(state, playlist_directory);
                        }
                    } else if (!state.is_switching_playlist && state.video_active) {
                        // Only seek if we have an active video (not intro)
                        controller.seek(10.0);
                    }
                    break;
                    
                case InputAction::PREV:
                    if (!ev.pressed) break; // Only trigger on press, not release
                    // If video is playing, go to previous playlist item
                    // In Master Shuffle mode, pick another random video
                    // Otherwise, seek backward in current video
                    // Don't allow if we're switching playlists
                    if (!state.is_switching_playlist && state.video_active && state.current_playlist_index >= 0) {
                        if (state.master_shuffle_active) {
                            // In Master Shuffle, PREV goes back through shuffle history
                            int prev_playlist, prev_item;
                            if (state.pop_shuffle_history(prev_playlist, prev_item)) {
                                if (prev_playlist >= 0 && prev_playlist < static_cast<int>(state.playlists.size())) {
                                    const auto& pl = state.playlists[prev_playlist];
                                    if (prev_item >= 0 && prev_item < static_cast<int>(pl.items.size())) {
                                        state.current_playlist_index = prev_playlist;
                                        state.current_item_index = prev_item;
                                        state.last_advanced_item_index = -1;
                                        state.last_advanced_duration = 0.0;
                                        state.playback_started_ = false;
                                        controller.load_playlist_item(state, pl, prev_item, playlist_directory);
                                    }
                                }
                            } else {
                                // No history - pick another random video
                                controller.play_random_global_video(state, playlist_directory);
                            }
                        } else {
                            controller.load_previous_item(state, playlist_directory);
                        }
                    } else if (!state.is_switching_playlist && state.video_active) {
                        // Only seek if we have an active video (not intro)
                        controller.seek(-10.0);
                    }
                    break;
                    
                case InputAction::SEEK_LEFT:
                    controller.seek(-5.0);
                    break;
                    
                case InputAction::SEEK_RIGHT:
                    controller.seek(5.0);
                    break;
                    
                default:
                    break;
            }
        }
        
        // Update player state (polls GStreamer pipeline state)
        player.update_state();

               // Update state from controller
               // Always update during intro, switching, the seek bar
               // visible window (so the scrub overlay tracks position
               // smoothly at 60 Hz instead of 30 Hz, which felt
               // stuttery on the kiosk's progress bar), and otherwise
               // every other frame to keep CPU cost down.
               bool seek_bar_window = state.show_seek_bar
                                      || state.seek_bar_timer > 0.0;
               if (!state.intro_complete || state.is_switching_playlist
                   || seek_bar_window || frame_count % 2 == 0) {
                   controller.update_state(state);
               }

               // GStreamer remains alive after intro, just ensure proper state management
        sample_mode.update_state(state);
        
        // Clear playlist switching flag if it's been stuck for too long (timeout safety)
        // This prevents the flag from getting stuck if video fails to load or gets into bad state
        if (state.is_switching_playlist) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.playlist_switch_start_time);
            if (elapsed.count() > 2000) {  // 2 second timeout
                std::cerr << "CRITICAL: Playlist switch timeout after " << elapsed.count() << "ms - clearing flag and resetting state" << std::endl;
                std::cerr << "  Debug info: video_active=" << state.video_active
                          << ", is_playing=" << controller.is_playing()
                          << ", current_playlist=" << state.current_playlist_index
                          << ", current_item=" << state.current_item_index << std::endl;
                state.is_switching_playlist = false;

                // Also reset video state to ensure clean recovery
                if (!controller.is_playing() && !state.video_active) {
                    std::cerr << "MPV appears stuck - attempting recovery by stopping and clearing state" << std::endl;
                    controller.stop();
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }
        }
        
        // Handle intro video completion
        // When intro video ends, fade it out first, then fade in the UI
        if (state.showing_intro_video && state.video_active && state.get_duration() > 0.0) {
            const double intro_position = state.get_position();
            const double intro_duration = state.get_duration();
            // Update LED dance animation during intro video
            // Use video position as elapsed time (in milliseconds)
            gpio.update_intro_animation(static_cast<uint64_t>(intro_position * 1000));
            // Check if intro video has ended
            // Use multiple conditions to ensure reliable detection
            bool video_ended = false;

            // Primary check: position near end (tight margin so video plays fully)
            if (intro_position >= intro_duration - 0.05) {
                video_ended = true;
            }

            // Fallback check: if video stopped playing but we're still showing intro
            if (!controller.is_playing() && intro_duration > 0.0 && intro_position > 0.0) {
                video_ended = true;
            }

            if (video_ended && !state.intro_fading_out) {
                state.intro_fading_out = true;
                state.intro_fade_out_start_time = std::chrono::steady_clock::now();
                std::cout << "Intro video completed, starting fade-out..." << std::endl;
            }
        }
        
        // Handle intro video fade-out
        if (state.intro_fading_out) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.intro_fade_out_start_time);
            std::chrono::milliseconds fade_out_duration(300);  // 300ms fade-out duration


            if (elapsed >= fade_out_duration) {
                // Fade-out complete - stop video and start UI fade-in
                state.intro_fading_out = false;
                state.showing_intro_video = false;
                state.intro_complete = true;

                // Stop the intro video completely
                controller.stop();

                std::cout << "Intro video stopped, transition to UI complete" << std::endl;

                // Force immediate UI rendering by clearing and ensuring clean transition
                glViewport(0, 0, mode.width, mode.height);
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

                // Force multiple buffer swaps to ensure the clear takes effect
                for (int i = 0; i < 3; i++) {
                    if (!egl.swap_buffers()) {
                        std::cerr << "Failed to swap buffers during intro transition!" << std::endl;
                    }
                }

                std::cout << "Intro transition complete - video stopped, renderer cleaned, screen cleared, UI ready" << std::endl;
                
                // Stop LED intro animation
                gpio.stop_animation();

                // Verify that video actually stopped before proceeding
                int retry_count = 0;
                const int max_retries = 20;  // Increased retries
                while (controller.is_playing() && retry_count < max_retries) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // Longer delay
                    controller.update_state(state);  // Update state to get current playing status
                    retry_count++;
                    if (retry_count % 5 == 0) {
                        std::cout << "DEBUG: Waiting for video to stop... attempt " << retry_count << ", is_playing=" << controller.is_playing() << std::endl;
                    }
                }

                if (controller.is_playing()) {
                    std::cerr << "Warning: Intro video did not stop cleanly after " << (max_retries * 100) << "ms, proceeding anyway" << std::endl;
                } else {
                    std::cout << "DEBUG: Intro video stopped successfully after " << retry_count << " attempts" << std::endl;
                }
                
                // Force video_active to false immediately (don't wait for update_state)
                state.video_active = false;
                state.update_playback_state(0.0, 0.0);
                state.current_playlist_index = -1;
                state.current_item_index = -1;
                
                // Start fade-in animation for UI (from transparent to visible)
                // Since there's no video active after intro, we fade in the UI
                state.fade_start_time = std::chrono::steady_clock::now();
                state.fade_target_ui_visible = true;  // Fade to visible
                state.is_fading = true;
                
                std::cout << "Intro video fade-out complete, fading in UI..." << std::endl;
            } else {
                // Fade-out in progress - interpolate volume from 100% to 0%
                float fade_progress = static_cast<float>(elapsed.count()) / static_cast<float>(fade_out_duration.count());
                fade_progress = std::min(1.0f, std::max(0.0f, fade_progress));  // Clamp to [0, 1]
                
                // Fade volume from original_volume to 0
                double current_volume = state.original_volume * (1.0 - fade_progress);
                controller.set_volume(current_volume);
            }
        }
        
        // Clear fade flag when fade animation completes
        if (state.is_fading) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.fade_start_time);
            if (elapsed >= state.fade_duration) {
                state.is_fading = false;  // Fade complete
            }
        }
        
        // Auto-advance to next item in playlist when current video ends
        if (state.video_active && state.current_playlist_index >= 0 && state.current_item_index >= 0) {
            // Snapshot the (position, duration) pair so the whole advance
            // decision sees a consistent view rather than reading the
            // mutex-protected fields multiple times.
            const double adv_position = state.get_position();
            const double adv_duration = state.get_duration();
            // Check if video has ended (position >= duration with small tolerance)
            // Only advance once per item (check that we haven't already advanced from this item)
            if (adv_duration > 0.0 && adv_position >= adv_duration - 0.5) {
                // Video has ended - advance to next item
                // Only advance if we haven't already advanced from this item
                // Check that we haven't already advanced from this specific item index
                // AND that playback has actually started (prevents double-trigger from stale state)
                bool can_advance = false;
                if (state.master_shuffle_active) {
                    // In Master Shuffle we always advance to a new random video
                    // BUT we must ensure the new video has actually started playing
                    // to avoid double-triggering on stale state from the previous video
                    can_advance = state.playback_started_;
                } else {
                    // Normal playlist advance logic
                    // Advance when video ends, even if menu overlay is visible
                    can_advance = (state.current_item_index != state.last_advanced_item_index &&
                                   state.playback_started_);
                }

                if (can_advance) {
                    std::cout << "Auto-advancing from item " << state.current_item_index
                              << " at position " << adv_position << "/" << adv_duration << std::endl;
                    // Set flag BEFORE calling load_next_item to prevent race conditions
                    state.last_advanced_item_index = state.current_item_index;
                    state.last_advanced_duration = adv_duration;
                    // Note: load_next_item handles errors internally (skips broken files)
                    if (state.master_shuffle_active) {
                        // Save current position to shuffle history before auto-advancing
                        if (state.current_playlist_index >= 0 && state.current_item_index >= 0) {
                            state.push_shuffle_history(state.current_playlist_index, state.current_item_index);
                        }
                        controller.play_random_global_video(state, playlist_directory);
                    } else {
                        controller.load_next_item(state, playlist_directory);
                    }
                } else if (adv_position >= adv_duration - 0.5) {
                    if (!state.master_shuffle_active) {
                        std::cout << "NOT auto-advancing: item=" << state.current_item_index
                                  << ", last_advanced=" << state.last_advanced_item_index
                                  << ", playback_started=" << state.playback_started_ << std::endl;
                    }
                }
            } else {
                // Reset the flags when video is playing normally (not at end)
                // Reset unconditionally when we're well away from the end
                // This allows auto-advance to work even after manual navigation
                if (adv_position < adv_duration - 1.0) {
                    state.last_advanced_item_index = -1;
                    state.last_advanced_duration = 0.0;
                    // Master Shuffle stays active - only exits when user selects a different playlist
                }
            }
        }
        
        // Update fade animation (UI only - no audio changes)
        if (state.is_fading && state.video_active) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.fade_start_time);
            
            if (elapsed >= state.fade_duration) {
                // Fade complete
                state.is_fading = false;
            }
            // Note: Volume stays constant during menu overlay (no dimming)
        }
        
        // Render video (if playing or loading)
        // gst_renderer.render() will only render when UPDATE_FRAME is set, improving performance
        // IMPORTANT: Render video first, then UI overlay on top
        // During intro fade-out, don't render video - let UI fade in
        // After intro completes, never render video until explicitly started again

        // Render video appropriately for current state
        bool should_render_video = false;
        if (!state.intro_complete) {
            // During intro phase: render intro video
            should_render_video = (state.video_active || state.showing_intro_video || controller.is_playing()) &&
                                 !state.intro_fading_out;
        } else {
            // After intro: render regular videos when active or switching playlists
            should_render_video = state.video_active || (state.is_switching_playlist && controller.is_playing());
        }

        // Debug video rendering decision
        static std::optional<bool> last_render_decision;
        if (!last_render_decision.has_value() || should_render_video != *last_render_decision) {
            std::cout << "Video render decision changed: should_render=" << should_render_video
                     << ", intro_complete=" << state.intro_complete
                     << ", video_active=" << state.video_active
                     << ", is_switching=" << state.is_switching_playlist
                     << ", is_playing=" << controller.is_playing() << std::endl;
            last_render_decision = should_render_video;
        }


        
        // Clear screen in these cases:
        // 1. Intro video not ready yet
        // 2. No video should be rendered (after intro completes, during UI)
        // 3. During intro fade-out
        // 4. After intro completes (ensure clean background)
        // 5. When showing UI (ensure clean background)
        if ((state.showing_intro_video && !state.intro_ready) ||
            (!should_render_video && !state.video_active) ||
            state.intro_fading_out ||
            state.intro_complete ||
            (!state.showing_intro_video && !state.video_active)) {
            glViewport(0, 0, mode.width, mode.height);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        
        // Calculate 4:3 viewport for Modern TV mode
        // This is used for both video and UI rendering
        bool use_letterbox = (state.display_settings.mode == app::DisplayMode::MODERN_TV);

        // Media Browser content is modern 16:9 (movie posters, movie
        // playback) — skip the 4:3 pillarbox viewport that wraps the
        // main UI's 4:3 playlist videos. Both the per-screen layouts
        // and ad-hoc movie playback assume the full 1280x720 viewport.
        // Same gating idea as the bezel skip below (kept as a separate
        // flag so future tweaks to either don't have to touch the other).
#ifdef MEDIA_BROWSER_ENABLED
        bool letterbox_active = use_letterbox &&
            state.current_screen != app::AppScreen::MediaBrowser;
#else
        bool letterbox_active = use_letterbox;
#endif
        // The 4:3 content rect exists in TWO coordinate spaces and they
        // are no longer the same thing:
        //
        //   PHYSICAL (content_*)      -> glViewport, real framebuffer px
        //   LOGICAL  (logical_content_*) -> set_content_viewport, which
        //                                overwrites the renderer's
        //                                projection size
        //
        // They coincided while the framebuffer was always 1280x720. In
        // Modern TV at 1920x1080 they diverge by 1.5x, and feeding the
        // PHYSICAL size to set_content_viewport would make the logical
        // canvas 1440x1080 instead of 960x720 — every menu element would
        // render at 2/3 of its intended screen fraction with dead margin
        // right and bottom. The logical rect is derived from the UI's own
        // logical canvas so it stays 960x720 at any output resolution.
        int content_x = 0, content_y = 0, content_w = mode.width, content_h = mode.height;
        const config::display::Size ui_canvas = config::display::logical_canvas(
            state.display_settings.mode == app::DisplayMode::CRT_NATIVE);
        int logical_content_w = ui_canvas.w, logical_content_h = ui_canvas.h;

        if (letterbox_active) {
            // Calculate 4:3 content area centered in screen
            // Height stays the same, width is adjusted for 4:3 aspect ratio
            content_h = mode.height;
            content_w = mode.height * 4 / 3;  // 4:3 aspect ratio
            content_x = (mode.width - content_w) / 2;  // Center horizontally
            content_y = 0;

            // If calculated width is larger than screen, pillarbox based on width
            if (content_w > static_cast<int>(mode.width)) {
                content_w = mode.width;
                content_h = mode.width * 3 / 4;
                content_x = 0;
                content_y = (mode.height - content_h) / 2;
            }

            // Same 4:3 math, logical space. Integer-exact at both
            // resolutions: 720 -> 960x720, 1080 -> 1440x1080.
            logical_content_h = ui_canvas.h;
            logical_content_w = ui_canvas.h * 4 / 3;
            if (logical_content_w > ui_canvas.w) {
                logical_content_w = ui_canvas.w;
                logical_content_h = ui_canvas.w * 3 / 4;
            }
        }
        
        // Enhanced CRT pipeline (Phase 1+): redirect video+UI rendering
        // into an offscreen scene FBO, then composite back to the
        // default framebuffer through a shader that can sample the
        // scene pixels. Returns true only when:
        //   - display_settings.enhanced_crt_enabled is on,
        //   - we're NOT on the Media Browser screen,
        //   - at least one CRT effect intensity is > 0,
        //   - and FBO creation succeeded.
        // Otherwise falls back to direct-to-default-FB rendering for
        // pixel-identical legacy behavior. The paired
        // end_scene_fbo_and_composite() call is below, AFTER the UI
        // render but BEFORE the bezel — the bezel and toast remain in
        // their existing draw order and are unaffected by CRT effects.
        bool scene_fbo_used = ui_renderer.begin_scene_fbo(state);

        // Render video - gst will fill the entire framebuffer, so no need to clear if video is ready
        // This handles both intro video and regular video playback
        if (should_render_video) {
            // Set letterbox mode based on display settings
            gst_renderer.set_letterbox_mode(letterbox_active);

            // Aspect-preserve mode: enabled for Modern TV (so Media
            // Browser's wide movies don't stretch vertically), disabled
            // for CRT_NATIVE (the CRT TV's HDMI input does its own
            // 16:9→4:3 conversion, so the Pi should send a fully-filled
            // 1280×720 framebuffer; pre-pillarboxing on the Pi side
            // produces a windowboxed image with margins on all four
            // sides on the CRT — exactly the operator's complaint).
            const bool crt_mode =
                (state.display_settings.mode == app::DisplayMode::CRT_NATIVE);
            gst_renderer.set_aspect_preserve(!crt_mode);

            // In Modern TV mode, set viewport to 4:3 content area for video
            if (letterbox_active) {
                glViewport(content_x, content_y, content_w, content_h);
            }

#ifdef MEDIA_BROWSER_ENABLED
            // Marquee playback: tell gst_renderer to render INTO an
            // inset rect that matches the wood-frame asset's edge
            // thickness (40 px on every side). The video fills the
            // canvas INSIDE the frame with no gap, no overlap — edges
            // of the video sit flush with the inner edge of the
            // cabinet art. Aspect-preserve / letterbox math runs
            // WITHIN this rect so widescreen movies still letterbox
            // correctly (Pulp Fiction's 2.39:1 → ~80 px black bars
            // top+bottom inside the inset rect, all visible inside
            // the wood frame).
            //
            // Setting inset directly on gst_renderer is what was
            // missing before: a bare `glViewport()` call from main
            // gets immediately overwritten by gst_renderer's own
            // glViewport() at the end of render_quad(), which is why
            // the earlier 30 px / 50 px attempts looked like nothing
            // had changed.
            //
            // CRITICAL: clear the WHOLE framebuffer to black before
            // render() so the ring of pixels between the wood frame's
            // inner edge (x=40) and any letterbox bars don't keep
            // stale content from the previous frame. The earlier-in-
            // frame clear is gated on `!state.video_active`, which is
            // false during active playback.
            //
            // Non-Marquee paths (intro video, playlist playback in the
            // main kiosk) reset the inset to (0,0,0,0) which means
            // "use the full screen" — legacy behavior preserved.
            //
            // CRT shader is already gated off for MediaBrowser (see
            // begin_scene_fbo's AppScreen check) so playback shows a
            // clean unfiltered image either way.
            if (state.current_screen == app::AppScreen::MediaBrowser &&
                current_mb_screen == media_browser::ui::Screen::Playback &&
                state.display_settings.mb_playback_show_frame) {
                // Wood-frame overlay enabled for playback (operator
                // setting in MovieSettings → Library → Wood frame
                // during playback). Inset the video INSIDE the frame
                // so the cabinet art doesn't crop the movie.
                // The frame art is a 1280x720 PNG drawn as a full-screen
                // quad in LOGICAL space, so its 40px border stretches
                // with the framebuffer: at 1080p the inner edge lands at
                // 60 physical px. set_render_inset() takes FRAMEBUFFER
                // pixels, so the inset must scale to match or the
                // cabinet art overlaps and crops ~20px off every side of
                // the movie. Identity at 720p (40 -> 40).
                const int inset_px = config::display::to_physical_px(
                    40, static_cast<int>(mode.height),
                    config::display::logical_canvas(false).h);
                gst_renderer.set_render_inset(
                    inset_px, inset_px,
                    static_cast<int>(mode.width)  - 2 * inset_px,
                    static_cast<int>(mode.height) - 2 * inset_px);
                glViewport(0, 0, mode.width, mode.height);
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            } else {
                // Either non-Marquee playback (intro / playlist) OR
                // Marquee playback with wood frame toggled OFF —
                // either way, render full-screen with no inset.
                gst_renderer.set_render_inset(0, 0, 0, 0);
            }
#endif

            gst_renderer.render();
        }
        
        // Render UI overlay (will skip if intro video is showing - handled in renderer.cpp)
        // Only render UI if intro is not showing, or if intro is fading out
        // Note: ui_renderer.render() includes CRT effects at the end
        if (!state.showing_intro_video || state.intro_fading_out) {
            // Set viewport to 4:3 content area for UI in Modern TV mode
            // Also update UI renderer's internal dimensions for proper projection matrix
            glViewport(content_x, content_y, content_w, content_h);
            if (letterbox_active) {
                // LOGICAL size — see the two-coordinate-space note where
                // these are computed. Passing content_w/content_h (the
                // physical rect) here would shrink the whole main menu.
                ui_renderer.set_content_viewport(logical_content_w, logical_content_h);
            }
#ifdef MEDIA_BROWSER_ENABLED
            // Skip main kiosk UI when the Media Browser screen is active;
            // the placeholder (drawn below) replaces it. This avoids
            // rendering the main UI underneath the placeholder every frame.
            if (state.current_screen != app::AppScreen::MediaBrowser) {
                ui_renderer.render(state);
            }
#else
            ui_renderer.render(state);
#endif

            // Reset viewport after UI render
            if (letterbox_active) {
                ui_renderer.reset_content_viewport();
            }
        }

        // Pair to begin_scene_fbo above. If we were rendering into the
        // offscreen scene FBO, this binds the default framebuffer back
        // and composites the scene through the enhanced CRT shader
        // (sampling-based effects). Safe no-op if begin_scene_fbo
        // returned false. Must run BEFORE the bezel/toast/Media-Browser
        // overlay draws below so they end up on top of the composite,
        // exactly as they do in the legacy direct-to-default-FB path.
        if (scene_fbo_used) {
            ui_renderer.end_scene_fbo_and_composite(state);
        }

        // Render bezel overlay LAST in Modern TV mode (on top of EVERYTHING including CRT effects)
        // The bezel PNG is stretched fullscreen - content is visible through the transparent center.
        //
        // Skip the bezel entirely while the Media Browser owns the screen.
        // The Media Browser's content is modern 16:9 (movie posters, movie
        // playback) — the CRT-frame bezel was designed for the main UI's
        // 4:3 playlist videos and would just clip widescreen content.
#ifdef MEDIA_BROWSER_ENABLED
        bool bezel_allowed = state.current_screen != app::AppScreen::MediaBrowser;
#else
        bool bezel_allowed = true;
#endif
        if (bezel_allowed && letterbox_active && !state.available_bezels.empty() &&
            state.display_settings.bezel_index >= 0 &&
            state.display_settings.bezel_index < static_cast<int>(state.available_bezels.size())) {
            const auto& bezel = state.available_bezels[state.display_settings.bezel_index];
            if (!bezel.file.empty()) {
                // load_bezel() dedupes internally (skips if path matches AND texture is still valid).
                // We must call it every frame so the bezel is re-uploaded after reset_gl()
                // (e.g. after returning from RetroArch) — a stale path tracker here would block that.
                ui_renderer.load_bezel(bezel.file);
                // Reset viewport to fullscreen for bezel overlay
                glViewport(0, 0, mode.width, mode.height);
                ui_renderer.render_bezel();
            }
        }
        
#ifdef MEDIA_BROWSER_ENABLED
        // Media Browser screen dispatcher: render the currently active
        // MbScreen full-screen. Each stub screen draws only a centered
        // identifier label for now; Tasks 18-23 replace with real UI.
        if (state.current_screen == app::AppScreen::MediaBrowser) {
            glViewport(0, 0, mode.width, mode.height);

            // Bind the UI shader, enable blending, and set screen-size
            // uniform before any MB screen draws. This is normally done
            // inside ui_renderer.render(state), but that's skipped for
            // Media Browser — and on the Playback screen, gst_renderer
            // runs immediately before this block and leaves its YUV
            // sampler bound. Without resetting state here, all MB draw
            // calls silently target the wrong shader (invisible scrub
            // overlay; "green rect" garbage on Playback→Detail exit).
            ui_renderer.mb_begin_2d_state();

            // LOGICAL canvas size, not the physical mode. mb_begin_2d_state
            // pins the shader's screenSize uniform to the renderer's
            // logical canvas, so the screens must lay out in that same
            // space. These matched only because the framebuffer used to
            // always be 1280x720; at 1920x1080 they diverge by 1.5x and
            // every right-/bottom-anchored element (tab strip, footer
            // hints, filter panel) would fly off-screen. This is also
            // what keeps library_screen.cpp's literal 1280 constants and
            // mb_filter_overlay.cpp's kScreenW correct with no edits.
            const int mb_w = static_cast<int>(ui_renderer.get_width());
            const int mb_h = static_cast<int>(ui_renderer.get_height());

            active_mb_screen->render(ui_renderer, mb_w, mb_h);

            // "Exit Marquee?" confirm modal — rendered above the active MB
            // screen but below the wood frame and CRT effects so it reads
            // as a native UI element. No-op when the modal is not open.
            mb_exit_modal.render(ui_renderer, mb_w, mb_h);

            // Download-stall prompt modal — same compositing slot as the
            // exit modal. is_active() inside render() makes it a no-op
            // when not shown. Task 16.
            mb_stall_modal.render(ui_renderer, mb_w, mb_h);

            // Drain any completed poster fetches and upload them to GL.
            // Must happen on the GL-owning main thread. Without this
            // call the background fetcher thread's decoded images would
            // never make it onto screen.
            //
            // SKIP during Playback — texture allocation + GPU upload is
            // the most expensive non-decode work in the per-frame budget,
            // and there's no point loading new poster art for a screen
            // the operator can't see anyway. This shaves CPU+GPU off the
            // critical path during the GStreamer pipeline's first ~3
            // seconds (where most QoS frame drops were happening) and
            // prevents the Pi 4 from accumulating thermal headroom debt
            // that would otherwise translate into mid-movie throttling.
            // Pending fetches stay queued and resume the moment the
            // operator exits playback back to a menu screen.
            if (current_mb_screen != media_browser::ui::Screen::Playback) {
                ui_renderer.pump_artwork();
            }

            // CRT effects overlay on Media Browser menu screens (Browse,
            // Library, Search, Detail, Queue, Settings). Same legacy
            // procedural overlay that wraps the kiosk main UI's playlist
            // grid — gives the Marquee section the same nostalgic CRT
            // look operators get on the home menu.
            //
            // SKIPPED during Playback so movies stay clean and unfiltered
            // (operator direction). Also skipped if all MB CRT intensity
            // values are 0 — render_crt_effects has its own any-active
            // early-out, and we don't pay for the call.
            //
            // Wood frame draws AFTER this (next block) so the cabinet
            // art always reads as foreground over the CRT haze.
            //
            // The Marquee CRT intensities live in
            // state.display_settings.mb_* — independent from the kiosk
            // values that drive the home-menu look. We swap the mb_*
            // values into the kiosk fields just for this render call,
            // then restore — so render_crt_effects (which reads the
            // standard fields) renders the MB look without needing to
            // know about the dual-store. Restoration is unconditional
            // so the rest of the frame (and the home menu when the
            // user pops out of MB) sees the original kiosk values.
            if (current_mb_screen != media_browser::ui::Screen::Playback) {
                auto& s = state.display_settings;
                const float k_scan  = s.scanline_intensity;
                const float k_warm  = s.warmth_intensity;
                const float k_glow  = s.glow_intensity;
                const float k_mask  = s.rgb_mask_intensity;
                const float k_bloom = s.bloom_intensity;
                const float k_inter = s.interlacing_intensity;
                const float k_flick = s.flicker_intensity;
                s.scanline_intensity    = s.mb_scanline_intensity;
                s.warmth_intensity      = s.mb_warmth_intensity;
                s.glow_intensity        = s.mb_glow_intensity;
                s.rgb_mask_intensity    = s.mb_rgb_mask_intensity;
                s.bloom_intensity       = s.mb_bloom_intensity;
                s.interlacing_intensity = s.mb_interlacing_intensity;
                s.flicker_intensity     = s.mb_flicker_intensity;

                ui_renderer.render_crt_effects(state, /*scanlines_enabled=*/true);

                s.scanline_intensity    = k_scan;
                s.warmth_intensity      = k_warm;
                s.glow_intensity        = k_glow;
                s.rgb_mask_intensity    = k_mask;
                s.bloom_intensity       = k_bloom;
                s.interlacing_intensity = k_inter;
                s.flicker_intensity     = k_flick;
            }

            // Marquee wood-frame overlay — the "TV cabinet" that gives the
            // Media Browser its distinct visual identity from the rest of
            // the kiosk. Drawn ON TOP of MB content (after pump_artwork)
            // so the frame visually covers the outer pixels of any
            // CRT-processed output, producing the design's inset-CRT
            // effect without needing a real CRT region change. Toasts
            // still render on top of the frame (intentional — toast text
            // must always be readable).
            //
            // load_marquee_frame() dedupes by path internally, like
            // load_bezel(). Calling it every frame is correct: it
            // re-uploads the texture after reset_gl() (e.g. RetroArch
            // return), and a static path tracker would defeat that.
            //
            // Gated on the MovieSettings "Wood frame during playback"
            // toggle for the Playback screen specifically. When that
            // toggle is OFF and we're playing a movie, the frame is
            // skipped so the video fills the whole framebuffer. Other
            // MB screens (Browse, Library, Search, Detail, Settings,
            // Queue) ALWAYS render the frame regardless of the toggle —
            // it's only the playback experience that operators can
            // choose to show full-screen.
            const bool show_frame =
                (current_mb_screen != media_browser::ui::Screen::Playback) ||
                state.display_settings.mb_playback_show_frame;
            if (show_frame) {
                ui_renderer.load_marquee_frame("marquee_frame.png");
                ui_renderer.render_marquee_frame();
            }
        }

        // Render toast overlay (fades in/out over 3s). Drawn last-in-UI
        // so it sits above bezel and CRT effects. No-op when no toast
        // is active.
        //
        // Pass the renderer's CURRENT content-viewport dims (which
        // can be 1280×720, 960×720 for Modern TV letterbox, or
        // 640×480 for CRT_NATIVE) — NOT the raw HDMI mode dims.
        // The renderer's projection matrix is set up for the content
        // viewport, so a Toast computing its panel position from the
        // HDMI dims would project past the visible logical area and
        // clip to a corner of the screen. Operator-reported symptom
        // in CRT mode: "Wi-Fi connected" panel appeared in the
        // lower-right with only a corner visible because (1280-480)/2
        // = 400 logical was being interpreted in 640×480 space.
        glViewport(0, 0, mode.width, mode.height);
        ui::Toast::render(ui_renderer,
                          ui_renderer.get_width(),
                          ui_renderer.get_height());
#endif

        // ── Phone-remote: derive screen mode + 5 Hz status write ─────────────
        // Derives screen_mode for the four "live main-loop" states.
        // NOTE: RetroArch mode is NOT derived here — it is set explicitly at
        // the fork/waitpid transition point above so the companion app sees
        // "retroarch" immediately even though the main loop blocks on waitpid.
        state.screen_mode = [&]() -> app::ScreenMode {
            // Settings overlay first — covers the entire screen when active
            // and is conceptually a modal layer on top of any underlying
            // mode, so the phone remote should reflect Settings even if a
            // movie is playing or the Media Browser is open underneath.
            if (settings_menu.is_active() ||
                settings_menu.is_opening() ||
                settings_menu.is_closing())
                return app::ScreenMode::Settings;
            // Playback before MediaBrowser. When the user is watching a
            // Movies-section film, both `current_screen == MediaBrowser`
            // and `controller.is_playing()` are true — but for the phone
            // remote we want the Playback UI (scrub bar, PAUSE/PLAY label
            // tracking is_paused) regardless of where playback was launched
            // from. MediaBrowser mode means "browsing the library, no movie
            // playing yet."
            if (controller.is_playing()) return app::ScreenMode::Playback;
#ifdef MEDIA_BROWSER_ENABLED
            if (state.current_screen == app::AppScreen::MediaBrowser)
                return app::ScreenMode::MediaBrowser;
#endif
            return app::ScreenMode::Playlist;
        }();

        {
            auto sw_now = std::chrono::steady_clock::now();
            if (sw_now - last_status_write >= STATUS_PERIOD) {
                status_writer.write_now(state);
                last_status_write = sw_now;
            }
        }

        // Phone Remote — drain pending tap-to-seek requests each frame.
        // Cheap (single fs::exists check) when nothing is queued.
        controller.poll_seek_request();
        controller.poll_text_input_queue(state);

        // ── Phone Remote: 1 Hz tick for active pairing screen ────────────────
        if (settings_menu.is_pairing_screen_active()) {
            static auto last_pairing_tick = std::chrono::steady_clock::time_point{};
            auto pt_now = std::chrono::steady_clock::now();
            if (pt_now - last_pairing_tick >= std::chrono::seconds(1)) {
                settings_menu.pairing_screen()->tick();
                last_pairing_tick = pt_now;
            }
        }
        // ─────────────────────────────────────────────────────────────────────

        // Swap EGL buffers
        if (!egl.swap_buffers()) {
            std::cerr << "Failed to swap buffers!" << std::endl;
        }
        
        if (frame_count == 0) {
            std::cout << "  Buffers swapped, locking front buffer..." << std::endl;
        }
        
        // Present the GBM buffer to the display using page flip
        // Use shared lambda
        present_frame();
        
        // BARE BONES: Removed periodic audio checks - let MPV handle audio
        
        frame_count++;

        // Frame rate limiting. 60 FPS (16ms) is the default target for
        // crisp UI animations. During Media Browser movie playback we
        // drop to 30 FPS (33ms) so the render thread doesn't hog CPU
        // away from libav's HEVC decode threads — Pi 4 hardware HEVC
        // decode isn't reachable through GStreamer (rpivid driver
        // outputs SAND-format buffers that mainline gst-v4l2codecs
        // can't consume; fix in flight upstream but not on Bookworm
        // yet), so HEVC content is software-decoded at ~30-50% of one
        // core. The kiosk is rendering UI overlay every frame even
        // during movies, and at 60Hz that competes with decode for
        // CPU on the Pi 4's 4 cores. Halving the kiosk render rate
        // gives software HEVC decode roughly 25% more headroom on the
        // shared cores. Source content is 23.976 fps so 30 FPS UI is
        // already finer-grained than the underlying video — no
        // perceived motion difference, just less CPU pressure on
        // sustained-load scenes.
#ifdef MEDIA_BROWSER_ENABLED
        const bool mb_movie_active =
            state.video_active &&
            state.current_screen == app::AppScreen::MediaBrowser;
#else
        const bool mb_movie_active = false;
#endif
        const int target_ms = mb_movie_active ? 33 : 16;
        if (delta < target_ms) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(target_ms - delta));
        }
    }
    
#ifdef HAVE_SYSTEMD
    sd_notify(0, "STOPPING=1");
#endif

    // Cleanup
    LOG_INFO("Shutting down...");
    ui_renderer.cleanup();
    gst_renderer.cleanup();
    player.cleanup();
    input.cleanup();
    egl.cleanup();
    gbm.cleanup();
    display.cleanup();

    LOG_INFO("Shutdown complete");
    logging::shutdown();

    return 0;
}
