#include "platform/drm_display.h"
#include "platform/gbm_context.h"
#include "platform/egl_context.h"
#include "platform/input_manager.h"
#include "platform/gpio_manager.h"
#include "platform/platform_profile.h"
#include "platform/frame_presenter.h"
#include "video/gst_player.h"
#include "video/gst_renderer.h"
#include "ui/renderer.h"
#include "ui/settings_menu.h"
#include "ui/controller_wizard.h"
#include "ui/pairing_screen.h"
#include "ui/pairing_screen_renderer.h"
#include "retroarch/controller_profile.h"
// NOT under MEDIA_BROWSER_ENABLED: Toast::show() is called from the
// display-mode change path below (~line 1533), which is core kiosk code and
// compiles in every configuration. While this include sat inside the #ifdef,
// -DENABLE_MEDIA_BROWSER=OFF failed outright with "'ui::Toast' has not been
// declared". Nothing caught it: production always deploys MEDIA_BROWSER=true,
// and the OFF config on macOS never builds this target at all (it needs
// DRM/GLES), so the break only reproduced when building the kiosk binary on a
// Pi with the flag off. toast.cpp is likewise unconditionally in UI_SOURCES --
// see the note there, which fixed the matching LINK error and missed this one.
#include "ui/toast.h"
#ifdef MEDIA_BROWSER_ENABLED
#include "media_browser/prowlarr/prowlarr_client.h"
#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/radarr/radarr_client.h"
#include "media_browser/sonarr/sonarr_client.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/ui/playback_screen.h"
#include "media_browser/health/vpn_health_monitor.h"
#include "media_browser/artwork/artwork_cache.h"
#include "media_browser/library/watch_store.h"
#include "media_browser/mb_entry_gate.h"
#include "media_browser/mb_host.h"
#include "media_browser/mb_services.h"
#endif
#include "app/app_state.h"
#include "app/game_quiet_mode.h"
#include "app/game_handoff.h"
#include "app/game_handoff_kiosk.h"
#include "app/intro_sequence.h"
#include "app/intro_sequence_kiosk.h"
#include "app/settings_input.h"
#include "app/movie_quiet_mode.h"
#include "app/playlist_loader.h"
#include "app/controller.h"
#include "app/controller_transport.h"
#include "app/playlist_playback.h"
#include "app/playlist_reload.h"
#include "app/sample_mode.h"
#include "app/settings_persistence.h"
#include "app/status_writer.h"
#include "app/playback_reset.h"
#include "app/redraw_gate.h"
#include "app/post_game_gate.h"
#include "debug/screenshot_capture.h"
#include "ui/crt_time.h"
#include "utils/config.h"
#include "utils/frame_pacing.h"
#include "utils/path_resolver.h"
#include "utils/services_env.h"
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
#include <cstdio>   // std::fflush before _exit skips static teardown

#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#include <csignal>
#include "retroarch/retroarch_launcher.h"

// `systemctl stop` (and therefore every OTA restart) delivers SIGTERM.
// Without a handler the default action killed the kiosk mid-frame: the
// STOPPING=1 notification was never sent and the GL/EGL/DRM/pipeline
// cleanup below the main loop never ran — the clean shutdown path
// existed but was unreachable from the one place that stops the service.
// The handler only requests a loop exit; the normal end-of-main path
// does the rest, and TimeoutStopSec (20 s) still bounds a wedged cleanup
// with SIGKILL. SA_RESTART keeps blocking syscalls (poll in the input
// layer) from surfacing EINTR to code that never expected it — the render
// loop notices the flag within a frame anyway.
static volatile sig_atomic_t g_shutdown_requested = 0;
static void handle_shutdown_signal(int) {
    g_shutdown_requested = 1;
    // Mid-game the main thread is in the launcher's supervision loop, which
    // polls this request and runs retroarch::stop_game_session() (SIGTERM
    // RetroArch, wait for its auto-save, then SIGKILL its group). A request
    // that lands during the pre-launch teardown aborts the launch. Only an
    // atomic store here — async-signal-safe.
    retroarch::request_session_stop();
}

using namespace platform;
using namespace video;
using namespace ui;
using namespace app;

namespace fs = std::filesystem;

// Rebuild the per-model kiosk MENU-navigation overlays from the captured
// profile store and hand them to InputManager. Called once at startup and
// again whenever the Controller Setup wizard closes, so a pad the operator
// just captured can drive the menus immediately instead of after a restart.
// Passing the full map replaces the previous set wholesale (see
// InputManager::set_menu_overlays).
static void reload_menu_overlays(platform::InputManager& input) {
    std::map<uint32_t, platform::MenuNavOverlay> overlays;
    for (const auto& [key, prof] : retroarch::load_profile_store()) {
        (void)key;
        overlays[(static_cast<uint32_t>(prof.vid) << 16) | prof.pid] =
            retroarch::menu_overlay_from_profile(prof);
    }
    input.set_menu_overlays(std::move(overlays));
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
        // A missing/asleep TV exits 69, not 1: update.sh accepts 69 as
        // "the new binary runs" instead of rolling a good OTA back, and
        // systemd keeps retrying so plugging the TV in later still works.
        // See platform/kiosk_exit.h.
        const int rc = platform::exit_code_for_display_init_failure(display.init_failure());
        if (rc == platform::kExitNoDisplay) {
            LOG_ERROR("No connected display found - exiting {} so systemd retries until one is", rc);
        }
        logging::shutdown();
        return rc;
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
    // The exact timing pick_mode() chose and set; the size search below is
    // only a fallback for a display that never recorded one.
    drmModeModeInfo mode_info = display.get_current_mode_info();
    if (mode_info.hdisplay == 0 && conn && conn->count_modes > 0) {
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

    // Apply whatever the operator has already captured with the Controller
    // Setup wizard, so a non-standard pad can drive the menus from boot.
    // An empty/missing store is the normal case and leaves InputManager on
    // pure built-in behavior.
    reload_menu_overlays(input);

    // Initialize GPIO (for physical buttons, rotary encoder, LEDs, power switch)
    LOG_DEBUG("Initializing GPIO...");
    GpioManager gpio;
    if (!gpio.initialize()) {
        LOG_DEBUG("GPIO not available (normal if not on Raspberry Pi)");
    } else {
        LOG_DEBUG("GPIO initialized");
    }
    // Stop the boot LED chase sequence now that the app is starting.
    // UNCONDITIONAL — when GPIO init fails on a real Pi (2026-08-03: a
    // kernel-owned line failed the whole bulk request), the animation
    // service otherwise keeps pinctrl-poking the LED pins all session,
    // fighting whoever owns them next. Off-Pi the systemctl inside is a
    // harmless no-op.
    gpio.stop_boot_led_sequence();

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
            // Platform gate runs at load time: one golden image serves
            // Pi 4B and Pi 5, and the image carries every system's
            // content — the Pi 4 profile hides Pi 5-only systems
            // (N64/Dreamcast). detect_platform() here is a plain file
            // read; state.platform_profile is populated later in the
            // init sequence from the same source.
            all_playlists = PlaylistLoader::filter_for_platform(
                PlaylistLoader::load_playlists(path),
                platform::detect_platform());
            playlist_dir = path;
            break;
        }
    }
    
    if (all_playlists.empty()) {
        LOG_WARN("No playlists loaded");
    } else {
        LOG_INFO("Loaded {} playlists from: {}", all_playlists.size(), playlist_dir);
    }
    
    // Partition playlists between the two UI surfaces and prepend the
    // virtual Master Shuffle row: app::split_for_ui_with_master_shuffle,
    // the ONE sequence the runtime reload (playlists_reload_request, in the
    // main loop below) also runs, so the boot menu and the reloaded menu
    // cannot drift apart. Replaces the old whole-playlist classification,
    // under which a mixed playlist rode the main menu INTACT (any video
    // item made it a "video playlist") and its games were meanwhile
    // invisible to the Settings browser (which required ALL items to be
    // games).
    auto ui_split = app::split_for_ui_with_master_shuffle(all_playlists);

    // NOTE: downloaded movies do NOT surface here. An earlier Media Browser
    // pass synthesized a "Movies" playlist from the Radarr library into this
    // wheel; removed by operator decision — movies play only through
    // Settings -> Media Browser -> Library, keeping the main menu curated
    // playlists only (and keeping movie rows out of Master Shuffle's pool).

    // Main UI shows only video playlists (matching Python: playlists = video_playlists)
    AppState state;
    state.playlists = std::move(ui_split.video);
    state.game_playlists = std::move(ui_split.games);

    // THE game list — a reference to the AppState member, deliberately not
    // a copy. This used to be a local `game_playlists` vector that the
    // Settings game browser navigated and launched ROMs from, while the
    // renderer drew state.game_playlists. Two vectors agreed only because
    // nothing ever rewrote one of them; a runtime playlist reload does
    // rewrite it, and the stale copy would have launched a different ROM
    // than the one highlighted on screen.
    std::vector<Playlist>& game_playlists = state.game_playlists;

    std::cout << "Video playlists: " << state.playlists.size() << std::endl;
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
        // TEST-ONLY MDB_PLATFORM_POLICY_OVERRIDE: one loud WARN line, logged
        // exactly here (detect_platform() itself is called from several
        // places and stays silent). Active or ignored, a set override must
        // never be invisible — kiosk_status.json and verify_box.sh carry it
        // too.
        if (const std::string ovr = platform::policy_override_log_line(profile);
            !ovr.empty()) {
            LOG_WARN("{}", ovr);
        }
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
    // Layer 3 monitor (started only when Layers 1+2 already pass) plus the
    // backgrounded container-unpause startup safety net — see
    // media_browser::start_vpn_health_monitor. Lifetime: declared here at
    // function scope so its destructor stops the worker thread cleanly on
    // main() exit.
    std::unique_ptr<media_browser::VpnHealthMonitor> vpn_health_monitor =
        media_browser::start_vpn_health_monitor(state);
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
    // Warm the cached status snapshot (kicks an off-thread refresh) so
    // the first Settings/INFO open shows real state instead of lagging
    // one refresh cycle.
    (void)utils::WifiManager::instance().get_status_cached();
    auto retroarch_result = controller.initialize_retroarch_launcher();
    if (!retroarch_result) {
        std::cerr << "Warning: " << retroarch_result.error() << std::endl;
    }
    
    // Apply initial system volume
    controller.set_system_volume(state.master_volume);
    
    SampleMode sample_mode;
    
    // Store playlist directory for path resolution
    std::string playlist_directory = playlist_dir;

    // Main-menu playlist playback: start/switch from SELECT, NEXT/PREV,
    // auto-advance, the stuck-switch timeout, failed-item skip/give-up and
    // the playback stall watchdog. Every Controller/GstPlayer call goes
    // through the transport. See app/playlist_playback.h.
    app::ControllerTransport playlist_transport(controller, player);
    app::PlaylistPlayback playlist_playback(state, playlist_transport,
                                            playlist_directory);
    
    // Initialize settings menu
    ui::SettingsMenuManager settings_menu(&state);
    state.settings_menu = &settings_menu;

#ifdef MEDIA_BROWSER_ENABLED
    // Task 17: service clients for the Media Browser (the screens and
    // their dispatcher live in MediaBrowserHost, constructed below).
    // Radarr / Sonarr / TMDB / Prowlarr / qBittorrent, in that order, with
    // the mock fallbacks for a box without keys — see
    // media_browser/mb_services.h for the key chains and path prefixes.
    media_browser::MbServiceClients mb_clients =
        media_browser::make_service_clients();

    // Trickle-limit bootstrap: converge qBit's alt-limit rates and clear a
    // crash-stranded cap. Best-effort, gated on the provisioning marker.
    media_browser::bootstrap_qbit_alt_limits(*mb_clients.qbit,
                                             utils::kServicesEnvPath);

    // Torrents the kiosk paused (movie FullPause / game quiet mode) and
    // never resumed — kiosk crashed, was stopped mid-movie, or an OTA
    // restarted it. Resumed in the background with retries, never on the
    // render thread; an operator's manual pause is never undone. See
    // app::TorrentResumeRecovery.
    const std::string kTorrentPauseMarker =
        config::get_data_path() + "/qbit_paused_by_kiosk";
    std::function<bool()> qbit_pause_all;
    std::function<bool()> qbit_resume_all;
    if (auto* qbit = mb_clients.qbit.get(); qbit != nullptr) {
        qbit_pause_all = [qbit]() { return qbit->pause_all(); };
        qbit_resume_all = [qbit]() { return qbit->resume_all(); };
    }
    app::TorrentResumeRecovery torrent_resume_recovery;
    torrent_resume_recovery.start_if_needed(kTorrentPauseMarker, qbit_resume_all);

    // Track-1 quiet mode: silence the torrent/media stack for the whole
    // game session, mirroring PlaybackScreen's movie behavior. Gated on
    // the provisioning marker so unprovisioned Pis do exactly nothing
    // (no docker errors, no qBit timeouts in the log).
    //
    // Cross-ordering with the MOVIE executor (MovieQuietMode, below): the
    // two run on separate workers, so each action first waits (bounded)
    // for the other to be idle — a game launched seconds after leaving a
    // movie must not have its docker-stop race that movie's docker-start,
    // and vice versa. Bounded on both sides so two workers each waiting on
    // the other can only ever stall 20 s, never deadlock.
    std::atomic<app::MovieQuietMode*> movie_quiet_ptr{nullptr};
    auto wait_movie_quiet = [&movie_quiet_ptr]() {
        if (auto* mq = movie_quiet_ptr.load()) {
            (void)mq->wait_until_idle_for(std::chrono::seconds(20));
        }
    };
    app::GameQuietMode game_quiet_mode(app::make_game_quiet_actions({
        /*services_env_path=*/"/opt/magic_dingus_box/services/.env",
        /*torrent_pause_marker=*/kTorrentPauseMarker,
        /*wait_for_movie_quiet=*/wait_movie_quiet,
        /*pause_torrents=*/qbit_pause_all,
        /*resume_torrents=*/qbit_resume_all,
        /*run_command=*/[](const char* cmd) { return std::system(cmd); },
    }));

    // Movie playback contention guard executor. PlaybackScreen::enter()/
    // leave() only QUEUE pause/resume here; the qBit round-trips and the
    // docker stop/start script run on this worker, never on the render
    // thread (WatchdogSec=10). See app/movie_quiet_mode.h.
    app::MovieQuietMode movie_quiet_mode(
        media_browser::ui::PlaybackScreen::make_quiet_actions(
            mb_clients.qbit.get(),
            /*barrier=*/[&game_quiet_mode]() {
                (void)game_quiet_mode.wait_until_idle_for(
                    std::chrono::seconds(20));
            }));
    movie_quiet_ptr.store(&movie_quiet_mode);
    // Unpublish before movie_quiet_mode is destroyed (only reachable on an
    // early-return path; the normal exit is _exit): after this, no game
    // action can start waiting on it, and waiting for the game worker to
    // go idle guarantees none is mid-wait on it.
    struct MovieQuietUnpublish {
        std::atomic<app::MovieQuietMode*>& ptr;
        app::GameQuietMode& game;
        ~MovieQuietUnpublish() {
            ptr.store(nullptr);
            game.wait_until_idle();
        }
    } movie_quiet_unpublish{movie_quiet_ptr, game_quiet_mode};

    // Watch-state store (Phase 3): resume positions + watched flags for
    // movies and TV, in the media_browser.db SQLite file. Main/render-
    // thread-only by construction — workers never touch it. Best-effort:
    // open() failure leaves ok()==false, every method degrades to a safe
    // no-op (no resume, no checkpoints, no watched marks) and the store
    // logs its own one-shot warning. Declared BEFORE the screens so it
    // outlives every pointer handed to them.
    media_browser::library::WatchStore watch_store;
    (void)watch_store.open(config::get_media_db_file());

    // The Media Browser itself: its screens, modals, dispatcher and
    // per-frame steps (media_browser/mb_host.h). Everything above stays
    // owned here and is borrowed by reference; declared after all of it,
    // so the host never outlives what it points at.
    //
    // sonarr_configured is exactly the fallback-to-SonarrMockClient
    // condition (media_browser/mb_services.h) — the screens must not
    // present the mock's fixtures as real TV on a box that never had
    // Sonarr set up.
    media_browser::MediaBrowserHost mb_host(media_browser::MediaBrowserHost::Deps{
        *mb_clients.radarr,
        *mb_clients.sonarr,
        /*sonarr_configured=*/mb_clients.sonarr_configured,
        *mb_clients.tmdb,
        mb_clients.prowlarr.get(),
        *mb_clients.qbit,
        watch_store,
        movie_quiet_mode,
        controller,
        state,
        /*player_error_probe=*/[&player]() { return player.has_error(); },
        ui_renderer,
        egl,
    });
#endif

    // ── RetroArch session bracketing ─────────────────────────────────────
    // Installed on the controller so EVERY route into an emulated_game
    // item gets it — main-UI SELECT on a mixed playlist, NEXT/PREV,
    // auto-advance at video end, Master Shuffle, and the Settings game
    // browser. See app/game_handoff.h for what begin/end do and why.
    //
    // sd_notify(0, msg) on a libsystemd build; empty (nothing sent) otherwise.
    app::SystemdNotify systemd_notify;
#ifdef HAVE_SYSTEMD
    systemd_notify = [](const char* msg) { sd_notify(0, msg); };
#endif
    // Return-from-game window: armed by the end hook below, released by the
    // main loop once the post-game reset has run (app/post_game_gate.h).
    app::PostGameGate post_game_gate;
    controller.set_session_watchdog([systemd_notify](retroarch::SessionWatchdog ev) {
        app::notify_session_watchdog(ev, systemd_notify);
    });
    app::GameSessionBracket::Ops game_session_ops;
    game_session_ops.systemd_notify = systemd_notify;
#ifdef MEDIA_BROWSER_ENABLED
    // Quiet the media stack for the whole session (async — never delays
    // launch) and drop poster textures while the GL context is still
    // current; the reverse on the way out.
    game_session_ops.quiet_media_stack = [&game_quiet_mode, &ui_renderer]() {
        game_quiet_mode.request_pause();
        if (ui_renderer.artwork_cache_initialized()) {
            ui_renderer.artwork_cache().pause();
            ui_renderer.artwork_cache().clear_textures();
        }
    };
    game_session_ops.restore_media_stack = [&game_quiet_mode, &ui_renderer]() {
        if (ui_renderer.artwork_cache_initialized()) {
            ui_renderer.artwork_cache().resume();
        }
        game_quiet_mode.request_resume();
    };
#endif
    game_session_ops.poll_gpio = [&gpio]() { (void)gpio.poll(); };
    game_session_ops.write_status_now = [&status_writer, &state]() {
        status_writer.write_now(state);
    };
    app::GameSessionBracket game_session_bracket(state, post_game_gate,
                                                 std::move(game_session_ops));
    controller.set_game_session_hooks(
        [&game_session_bracket](const app::PlaylistItem& item) {
            game_session_bracket.begin(item);
        },
        [&game_session_bracket]() { game_session_bracket.end(); });

    // Boot intro video: find it, load it and wait (bounded) for its first
    // frame, so the first thing on screen is the video — or skip straight
    // to the menu when there is none. See app/intro_sequence_kiosk.h.
    app::start_intro(app::find_intro_video(), state, controller, player,
                     gst_renderer, playlist_directory);

#ifdef HAVE_SYSTEMD
    sd_notify(0, "READY=1");
#endif

    // Graceful-stop signals — see handle_shutdown_signal at the top of
    // this file.
    {
        struct sigaction sa{};
        sa.sa_handler = handle_shutdown_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGINT, &sa, nullptr);
    }

    // Main loop
    bool running = true;
    auto last_frame = std::chrono::steady_clock::now();

    std::cout << "Entering main loop..." << std::endl;

    // Scan-out of each rendered frame (GBM buffer -> DRM fb -> SetCrtc /
    // page flip). See platform/frame_presenter.h for the lifecycle. The
    // lambda keeps one name for both callers (main loop + game-loading
    // progress callback) and reads `mode` live, as the old inline code did.
    platform::FramePresenter frame_presenter(display, egl, mode_info);
    auto present_frame = [&]() {
        frame_presenter.present(mode.width, mode.height);
    };
    // The display stack the game hand-off tears down and restores
    // (app/game_handoff_kiosk.h).
    app::KioskGraphics kiosk_graphics{display, egl, frame_presenter,
                                      player, gst_renderer, ui_renderer};
    
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
    // Remember the real kiosk mode so the game-exit path can restore it
    // directly (one mode change, not a 640x480 round-trip).
    controller.set_kiosk_display_mode(mode.width, mode.height);

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

    // Skips render/swap/flip on iterations where the picture cannot have
    // changed — today only on the bare main menu. See app/redraw_gate.h.
    // Read once: MDB_REDRAW_GATE=0 (systemd drop-in Environment=) restores
    // draw-every-iteration without a rebuild.
    app::RedrawGate redraw_gate(
        app::redraw_gate_enabled_from_env(std::getenv("MDB_REDRAW_GATE")));
    LOG_INFO("Redraw gate: {} (idle main menu skips unchanged frames; "
             "MDB_REDRAW_GATE=0 disables)",
             redraw_gate.enabled() ? "ON" : "OFF");

    // Debug screenshots: `touch <data>/screenshot_request` -> the next drawn
    // frame is saved to <data>/screenshots/<UTC>.bmp (newest 10 kept). See
    // debug/screenshot_capture.h.
    debug::ScreenshotCapture screenshot_capture(config::get_data_path());

    // CRT field rate (redraw gate, static CRT menu at 30 fps): the field
    // the last drawn frame showed, and when that frame's present returned
    // (field_rate_skip_sleep paces the skipped iteration off it).
    int64_t crt_last_drawn_field = 0;
    bool crt_field_rate_logged = false;
    auto last_present_done = std::chrono::steady_clock::now();

    // UI draw calls per drawn frame (MDB_BATCH_UI A/B), reported per minute.
    struct {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        uint64_t frames = 0;
        uint64_t sum = 0;
        uint64_t max = 0;
        bool reported_once = false;
    } ui_draw_window;
    LOG_INFO("UI batching: {} (MDB_BATCH_UI=0 restores one draw per primitive)",
             ui_renderer.ui_batching_enabled() ? "ON" : "OFF");

    // Settings-menu input dispatch (app/settings_input.h): BTN4 toggle /
    // hold-for-volume, the on-screen keyboard, and the Settings menu with
    // its game browser, Controller Setup wizard and pairing screen. `mode`
    // and `playlist_directory` are borrowed live.
    app::MenuButtonHold menu_hold;
    app::SettingsInputContext settings_input{
        state,
        settings_menu,
        keyboard,
        controller,
        input,
        kiosk_graphics,
        mode,
        playlist_directory,
#ifdef MEDIA_BROWSER_ENABLED
        /*enter_media_browser=*/[&mb_host]() { mb_host.enter_from_settings(); },
#endif
    };

    while (running && !g_shutdown_requested) {
        // DRM master could not be re-acquired after a game: the screen is
        // gone for good, but the loop would keep pinging the watchdog and
        // hold a black screen forever. Leave with a failure status instead
        // so systemd's Restart=on-failure brings the kiosk back.
        if (controller.display_lost()) {
            LOG_ERROR("Display lost after game — exiting for a systemd restart");
            running = false;
            break;
        }
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
        if (auto* mb_search_keyboard = mb_host.search_text_keyboard()) {
            // The MB Search screen's on-screen keyboard.
            state.active_text_keyboard = mb_search_keyboard;
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

        // The redraw gate must draw the iteration that rebuilds the display
        // below: its clear is swapped but not presented until the next drawn
        // frame. (Read before the block, which clears the flag.)
        const bool display_reset_this_iteration = state.reset_display;

        // Check for display reset signal (e.g. after returning from
        // RetroArch): DRM master, mode, EGL, GL resources, GStreamer, audio.
        // See app/game_handoff_kiosk.h.
        if (state.reset_display) {
            app::restore_display_after_game(kiosk_graphics, state, mode);
        }

        // Return-from-game ready edge (app/post_game_gate.h): drain the
        // input queued at a black screen unseen, then stop publishing
        // "retroarch".
        app::finish_post_game_if_ready(post_game_gate, state, input, gpio);

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
            // (frame_presenter.reset + GBM/EGL recreate + reset_gl on both
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
                // Keep the game-exit restore target in sync with the new mode.
                controller.set_kiosk_display_mode(mode.width, mode.height);

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
                input.rescan_devices();  // pads/keyboards plugged in after boot
            }
        }

        // Poll input
        auto input_events = input.poll();

        // ── Controller Setup wizard: raw-event pump + watchdog ───────────────
        //
        // Placed HERE deliberately: after input.poll() (which is what fills
        // the raw queue) and before the dispatch loop below (which is where a
        // cancel/save can call set_raw_capture(false)). set_raw_capture(false)
        // DISCARDS the pending queue — see the ordering requirement on
        // InputManager::set_raw_capture — so draining first is what keeps the
        // user's final capture from vanishing.
        //
        // The active→inactive edge is detected here rather than at each close
        // site so that EVERY way out (cancel, save+dismiss, idle timeout, pad
        // unplugged, Settings dismissed from under it) refreshes the menu-nav
        // overlays exactly once.
        {
            static bool wizard_was_active = false;
            if (settings_menu.is_controller_wizard_active()) {
                auto* wiz = settings_menu.controller_wizard();
                if (wiz) {
                    for (const auto& raw : input.drain_raw_events()) {
                        wiz->on_raw_event(raw);
                    }
                    if (!wiz->tick() || !wiz->is_active()) {
                        settings_menu.close_controller_wizard();
                    }
                } else {
                    settings_menu.close_controller_wizard();
                }
            }
            if (wizard_was_active && !settings_menu.is_controller_wizard_active()) {
                // Profiles may have changed — refresh menu-nav overlays.
                reload_menu_overlays(input);
            }
            wizard_was_active = settings_menu.is_controller_wizard_active();
            // Settings → System → "Reset Controller Setup" erases the captured
            // store without the wizard ever opening, so the edge above cannot
            // see it. A stale overlay would keep remapping menu buttons from a
            // profile that no longer exists.
            if (settings_menu.take_controller_profiles_dirty()) {
                reload_menu_overlays(input);
            }
        }
        // ─────────────────────────────────────────────────────────────────────

        // Poll GPIO (buttons, encoder) and merge with controller/keyboard events
        if (gpio.is_available()) {
            auto gpio_events = gpio.poll();
            input_events.insert(input_events.end(), gpio_events.begin(), gpio_events.end());
        }
        // For the redraw gate: ANY input draws this iteration. Captured
        // before the unlock-sequence detector and the MB dispatcher
        // consume events, so a swallowed press still counts.
        const bool input_this_iteration = !input_events.empty();

#ifdef MEDIA_BROWSER_ENABLED
        // Feed the Media Browser unlock sequence detector; events it
        // matches are CONSUMED so the dispatch below does not also fire
        // them. See MediaBrowserHost::feed_unlock_sequence.
        //
        // Suppressed entirely while the Controller Setup wizard is up. The
        // wizard binds BTN1 (redo) and BTN2 (skip), which overlap the unlock
        // sequence's chord and triple-press; letting the detector run would
        // let it swallow the very presses the wizard is asking for — silent
        // dead buttons on a screen the user is already unsure about.
        if (!settings_menu.is_controller_wizard_active()) {
            mb_host.feed_unlock_sequence(input_events, gpio);
        }
#endif


        // Time-based check for showing the volume slider (BTN4 held
        // long enough — app::MenuButtonHold::slider_due).
        if (!state.show_volume_slider &&
            menu_hold.slider_due(std::chrono::steady_clock::now())) {
            state.show_volume_slider = true;
        }

#ifdef MEDIA_BROWSER_ENABLED
        // Media Browser screen dispatcher (Task 17). While the Media
        // Browser owns the screen, the active MbScreen owns ALL input:
        // the host consumes this frame's events (so the main UI's loop
        // below sees an empty queue), runs screen hand-offs, and — on the
        // way out — hands control back to MainMenu. It also evicts the MB
        // first if the display mode can no longer host it. See
        // MediaBrowserHost::handle_input.
        mb_host.handle_input(input_events, mode.width, mode.height);
#endif

        for (const auto& ev : input_events) {
            // A game session ended inside an earlier event of THIS batch:
            // the remaining events were pressed before the game ran, and
            // the screen they were aimed at is gone (app/post_game_gate.h).
            if (!post_game_gate.accepts_input()) break;
            // BTN4: short press toggles Settings, hold turns the rotary
            // into master volume. See app/settings_input.h.
            if (app::handle_menu_button(settings_input, menu_hold, ev)) continue;

            // On-screen keyboard / Settings menu (and every sub-screen it
            // drives) own the event while shown. See app/settings_input.h.
            if (app::dispatch_overlay_input(settings_input, ev)) continue;
            
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
                    // Intro guard, reveal hidden UI, same-playlist UI toggle,
                    // or switch to / start the highlighted playlist (row 0 =
                    // Master Shuffle). See PlaylistPlayback::on_select.
                    playlist_playback.on_select();
                    break;
                    
                case InputAction::PLAY_PAUSE:
                    if (!ev.pressed) break; // Only trigger on press, not release
                    playlist_playback.on_play_pause();
                    break;
                    
                case InputAction::NEXT:
                    if (!ev.pressed) break; // Only trigger on press, not release
                    // Next item / Master Shuffle pick; seek +10 s when no
                    // playlist is playing.
                    playlist_playback.on_next();
                    break;
                    
                case InputAction::PREV:
                    if (!ev.pressed) break; // Only trigger on press, not release
                    // Previous item / Master Shuffle history; seek -10 s
                    // when no playlist is playing.
                    playlist_playback.on_prev();
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
        
        // Advances a pending mid-playback playlist switch (settle wait →
        // load, without blocking this thread) and clears a switching flag
        // stuck for over 2 s. See PlaylistPlayback::tick_switch_timeout.
        playlist_playback.tick_switch_timeout();
        
        // Intro video: LED dance + end detection, the 300 ms audio
        // fade-out, then the hand-off to the menu fade-in. No-op once the
        // intro is over. See app/intro_sequence_kiosk.h.
        app::tick_intro(state, controller, gpio, egl, mode);
        
        // Clear fade flag when fade animation completes
        if (state.is_fading) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.fade_start_time);
            if (elapsed >= state.fade_duration) {
                state.is_fading = false;  // Fade complete
            }
        }
        
        // Pipeline error on a playlist item -> skip it (or give up after a
        // run of failures). See PlaylistPlayback::tick_pipeline_error.
        playlist_playback.tick_pipeline_error();

        // Auto-advance to next item in playlist when current video ends
        // (honoring the item's `end:` trim). See
        // PlaylistPlayback::tick_auto_advance.
        playlist_playback.tick_auto_advance();
        
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

        // Render video appropriately for current state (intro phase vs
        // after it) — see app::should_render_video.
        const bool should_render_video =
            app::should_render_video(state, controller.is_playing());

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

        // ── Redraw gate ──────────────────────────────────────────────────
        // Everything above (input, settings/wizard pumps, pipeline and
        // playlist state) has run; from here to the present is drawing,
        // plus a tail of non-drawing work (status file, watch checkpoints,
        // stall watchdog, phone-remote queues, reload pokes) that runs
        // every iteration regardless. Skipping is opt-in: the main menu, an
        // idle open Settings menu, and idle MB Browse/Search/Library/
        // Detail/SeriesDetail (app::is_static_main_menu over the activity
        // flags below) may skip, and even there a frame is drawn on any
        // input, any change in what it shows, and at least every
        // RedrawGate::kDefaultMaxIdle. See app/redraw_gate.h.
        bool draw_this_frame = true;
        bool crt_field_rate_this_iteration = false;
        {
            app::MainMenuActivity act;
            act.intro = !state.intro_complete || state.showing_intro_video ||
                        state.intro_fading_out;
            act.video = should_render_video || state.video_active ||
                        state.is_switching_playlist || controller.is_playing();
            bool mb_menu_screen = false;  // MB, not Playback: MB CRT look
            [[maybe_unused]] bool mb_screen_static = false;
#ifdef MEDIA_BROWSER_ENABLED
            const bool mb_on =
                state.current_screen == app::AppScreen::MediaBrowser;
            mb_menu_screen =
                mb_on && mb_host.current_screen() != media_browser::ui::Screen::Playback;
            // Poster uploads run HERE, before the decision, not in the
            // drawing block: a skipped frame must still upload, and an
            // upload must draw the frame that shows it (the host counts
            // them into its redraw signature).
            if (mb_menu_screen) {
                mb_host.pump_artwork();
            }
            // The active MB screen may opt out of continuous drawing
            // (MbScreen::wants_continuous_redraw — Browse, Search,
            // Library, Detail, SeriesDetail when idle); the dispatcher's
            // modals keep it continuous while shown.
            mb_screen_static = mb_on && mb_host.wants_static_frame();
            act.media_browser = mb_on && !mb_screen_static;
#endif
            // Includes the wizard and pairing screen (both live inside it).
            // The open/close slide only advances inside a render, so it is
            // continuous; an open, idle menu may skip
            // (SettingsMenuManager::is_static_for_redraw).
            const bool settings_shown = settings_menu.is_active() ||
                                        settings_menu.is_opening() ||
                                        settings_menu.is_closing();
            act.settings_menu =
                settings_shown && !settings_menu.is_static_for_redraw();
            act.keyboard = keyboard.is_active();
            act.ui_fade = state.is_fading ||
                          state.post_game_fade_start_ms.load() != 0;
            // Toast::post()'s mailbox drains inside Toast::render, hence
            // has_pending(). The volume slider appears 300 ms into a BTN4
            // hold with no new input — hence button_held.
            act.transient_overlay =
                ui::Toast::is_active() || ui::Toast::has_pending() ||
                state.has_error_message() || state.show_volume_slider ||
                state.show_seek_bar || state.seek_bar_timer > 0.0 ||
                menu_hold.button_held || state.is_loading_game;
            // The MB menu screens draw the Marquee CRT look (mb_* values,
            // swapped in around render_crt_effects below), not the kiosk's.
            act.crt_time_effects =
                mb_menu_screen
                    ? (state.display_settings.mb_flicker_intensity > 0.0f ||
                       state.display_settings.mb_interlacing_intensity > 0.0f)
                    : (state.display_settings.flicker_intensity > 0.0f ||
                       state.display_settings.interlacing_intensity > 0.0f);

            // What the static menu draws that can change with no input and
            // no activity flag. The blink phase is the one that changes on
            // its own (2 Hz); the rest catch background state changes (a
            // web-admin playlist reload, a status line) without waiting for
            // the idle safety net.
            const auto gate_now = std::chrono::steady_clock::now();
            auto as_u64 = [](double v) {
                return static_cast<uint64_t>(static_cast<int64_t>(v));
            };
            app::ContentSignature sig;
            // The Media Browser covers the main menu, so its 2 Hz blink
            // must not redraw a static MB screen.
            bool main_menu_visible = true;
#ifdef MEDIA_BROWSER_ENABLED
            main_menu_visible = !mb_on;
#endif
            if (main_menu_visible) {
                sig.add(static_cast<uint64_t>(
                    ui_renderer.main_menu_blink_phase(gate_now)));
            }
            sig.add(static_cast<uint64_t>(state.selected_index));
            sig.add(static_cast<uint64_t>(state.playlist_scroll_offset));
            sig.add(static_cast<uint64_t>(state.playlists.size()));
            sig.add(static_cast<uint64_t>(state.current_playlist_index));
            sig.add(static_cast<uint64_t>(state.current_item_index));
            sig.add(state.status_text);
            sig.add(as_u64(state.get_position()));
            sig.add(as_u64(state.get_duration()));
            sig.add(static_cast<uint64_t>(state.display_settings.mode));
            sig.add(static_cast<uint64_t>(state.display_settings.bezel_index));
            // An open, static Settings menu: page, cursor, row labels.
            if (settings_shown) {
                sig.add(settings_menu.redraw_signature());
            }
#ifdef MEDIA_BROWSER_ENABLED
            // A static MB screen: which screen, what it shows, posters.
            if (mb_on) {
                mb_host.add_redraw_signature(sig, mb_screen_static);
            }
#endif

            app::RedrawInputs gate_in;
            gate_in.input_event = input_this_iteration;
            gate_in.video_frame = act.video;
            gate_in.animation_active = act.ui_fade || act.transient_overlay;
            // CRT flicker/interlacing on the otherwise static menu: the
            // shaders change the picture once per interlace field (30 Hz),
            // so draw every other vblank instead of every one. Only on a
            // >= 48 Hz mode (a 24/30 Hz mode already draws at <= 30), and
            // only with the gate on (MDB_REDRAW_GATE=0 = every vblank).
            gate_in.crt_field_rate = redraw_gate.enabled() &&
                                     mode_info.vrefresh >= 48 &&
                                     app::is_crt_field_rate_main_menu(act);
            gate_in.screen_requests_continuous =
                !app::is_static_main_menu(act) && !gate_in.crt_field_rate;
            // A pending screenshot must be drawn, or it would capture
            // whatever stale buffer the skip streak left behind.
            gate_in.forced = display_reset_this_iteration ||
                             screenshot_capture.poll(gate_now);
            gate_in.content_signature = sig.value();
            draw_this_frame = redraw_gate.should_draw(gate_in, gate_now);
            crt_field_rate_this_iteration = gate_in.crt_field_rate;

            // Which interlace field the CRT shaders draw. At field rate the
            // frame is pinned to the field after the last drawn one
            // (opposite parity, within one field of the clock) — frames are
            // vblank-locked and fields are not, so the raw clock can show
            // the same field twice. Otherwise: wall clock, as before.
            if (draw_this_frame) {
                const int64_t wall_field = ui::crt_field_index(gate_now);
                if (gate_in.crt_field_rate) {
                    crt_last_drawn_field =
                        ui::crt_render_field(wall_field, crt_last_drawn_field);
                    ui_renderer.set_crt_field_override(crt_last_drawn_field);
                } else {
                    crt_last_drawn_field = wall_field;
                    ui_renderer.set_crt_field_override(-1);
                }
            }
            if (gate_in.crt_field_rate && !crt_field_rate_logged) {
                crt_field_rate_logged = true;
                LOG_INFO("Redraw gate: CRT field rate active — static CRT menu "
                         "drawn every other vblank ({} Hz mode)",
                         static_cast<int>(mode_info.vrefresh));
            }

            // Per-minute counts at DEBUG (file log only). The first window
            // goes to INFO as well, so `journalctl -u magic-dingus-box-cpp`
            // alone shows whether the gate is actually skipping on a box.
            if (auto report = redraw_gate.take_report(gate_now)) {
                static bool first_report = true;
                // crt30 = iterations on the CRT-only static menu, drawn
                // at field rate (~half of them should be skipped).
                if (first_report) {
                    first_report = false;
                    LOG_INFO("Redraw gate: drew {} / skipped {} iterations in the last {}s "
                             "(crt30 {})",
                             report->drawn, report->skipped,
                             app::RedrawGate::kReportInterval.count(),
                             report->crt_field_rate);
                } else {
                    LOG_DEBUG("Redraw gate: drew {} / skipped {} iterations in the last {}s "
                              "(crt30 {})",
                              report->drawn, report->skipped,
                              app::RedrawGate::kReportInterval.count(),
                              report->crt_field_rate);
                }
            }
        }

        // Drawing, part 1: video, main UI, CRT composite, bezel, Media
        // Browser, toast. Body deliberately NOT re-indented under this `if`
        // (same convention as the MB dispatcher's `else if
        // (!mb_modal_exited)` in mb_host.cpp) so the gate's diff stays reviewable;
        // the closing brace is marked.
        if (draw_this_frame) {

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
            if (mb_host.in_playback() &&
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

        // Post-game fade-up is drawn at the very END of the frame, just
        // before the swap — see the block above "Swap EGL buffers".

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
        // Media Browser: render the currently active MbScreen
        // full-screen, then its modals, the Marquee CRT look (menu
        // screens only) and the wood frame. See MediaBrowserHost::render.
        if (state.current_screen == app::AppScreen::MediaBrowser) {
            mb_host.render(mode.width, mode.height);
        }
#endif

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
        //
        // NOT under MEDIA_BROWSER_ENABLED: core kiosk paths (Wi-Fi,
        // display mode, settings/playlist restore pokes) toast in every
        // build. This block used to sit inside the MB #ifdef, so an OFF
        // build never drew a toast — and never drained Toast::post()'s
        // mailbox, which kept the redraw gate drawing every frame.
        glViewport(0, 0, mode.width, mode.height);
        {
            ui::Renderer::BatchScope toast_batch_scope(ui_renderer);
            ui::Toast::render(ui_renderer,
                              ui_renderer.get_width(),
                              ui_renderer.get_height());
        }
        }  // if (draw_this_frame) — drawing, part 1

        // ── Phone-remote: derive screen mode + 5 Hz status write ─────────────
        // Derives screen_mode for the four "live main-loop" states.
        // NOTE: RetroArch mode is NOT derived here — it is set explicitly at
        // the fork/waitpid transition point above so the companion app sees
        // "retroarch" immediately even though the main loop blocks on waitpid.
        // It also STAYS "retroarch" after the game, until the post-game
        // reset is done (post_game_gate) — see the session end hook.
        state.screen_mode = post_game_gate.published_screen([&]() -> app::ScreenMode {
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
        }());

        {
            auto sw_now = std::chrono::steady_clock::now();
            if (sw_now - last_status_write >= STATUS_PERIOD) {
                status_writer.write_now(state);
                last_status_write = sw_now;
            }
        }

#ifdef MEDIA_BROWSER_ENABLED
        // ── Watch-state checkpoint + EOS watched marking (Task 4) ────────
        // 30 s resume checkpoints during MB Playback; one watched mark per
        // end-of-stream. See MediaBrowserHost::tick_watch_state.
        mb_host.tick_watch_state();
#endif

        // ── Playback stall watchdog ──────────────────────────────────────
        // Catches the pipeline silently stalling while the kiosk still
        // believes it is playing: restart it, or give up on the item after
        // repeated restarts. See PlaylistPlayback::tick_stall_watchdog.
        playlist_playback.tick_stall_watchdog(
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());

        // Phone Remote — drain pending tap-to-seek requests each frame.
        // Cheap (single fs::exists check) when nothing is queued.
#ifdef MEDIA_BROWSER_ENABLED
        if (controller.poll_seek_request()) {
            // The seek landed on the SHARED pipeline; during MB playback
            // PlaybackScreen must hear about it (EOS-flicker suppression).
            mb_host.on_external_seek();
        }
#else
        controller.poll_seek_request();
#endif
        controller.poll_text_input_queue(state);

        // Web-admin settings restore poke. The restore endpoint replaces
        // settings.json on disk and then writes this marker — without the
        // reload, the running kiosk's next operator-action save would
        // clobber the restored file with its stale in-memory state.
        // Checked ~once a second (restore is rare; no need to stat every
        // frame like the seek queue).
        {
            static int settings_reload_check = 0;
            if (++settings_reload_check >= 60) {
                settings_reload_check = 0;
                const std::string reload_marker =
                    config::get_data_path() + "/settings_reload_request";
                if (fs::exists(reload_marker)) {
                    std::error_code rm_ec;
                    fs::remove(reload_marker, rm_ec);
                    const auto prev_mode = state.display_settings.mode;
                    app::SettingsPersistence::load_settings(state);
                    // Same per-board reconcile the boot path runs (e.g. a
                    // Pi 4 backup restored onto a Pi 5 saying "headphone").
                    state.audio_settings.sanitize_for_platform(
                        state.platform_profile.has_analog_audio);
                    // Apply what applies at runtime.
                    state.audio_settings.apply_output();
                    controller.set_system_volume(state.master_volume);
                    if (state.display_settings.mode != prev_mode) {
                        // The DRM mode + logical canvas are chosen at boot
                        // (peek_is_crt_native) — be honest about that.
                        ui::Toast::show(
                            "Settings restored — restart to apply display mode");
                    } else {
                        ui::Toast::show("Settings restored");
                    }
                    LOG_INFO("Settings reloaded from disk (web-admin restore poke)");
                }
            }
        }

        // Web-admin playlist reload poke. The Content Manager writes this
        // marker once an imported/edited playlist is on disk; without it a
        // new playlist only reached the TV on a power cycle. Same protocol
        // as the settings poke above — poll ~1 Hz, delete the marker, then
        // reload — and deliberately the same shape: a kiosk binary that
        // predates the marker just ignores the file, which is what makes
        // the web-side change safe to ship first. The swap and every index
        // repair live in app/playlist_reload.h.
        {
            static int playlists_reload_check = 0;
            if (++playlists_reload_check >= 60) {
                playlists_reload_check = 0;
                const std::string playlists_marker =
                    config::get_data_path() + "/playlists_reload_request";
                if (fs::exists(playlists_marker)) {
                    if (state.is_switching_playlist || state.is_loading_game) {
                        // Hold the request: leave the marker in place and
                        // retry in a second. A playlist switch has ALREADY
                        // written current_playlist_index for a load that
                        // hasn't happened yet, so swapping the vector out
                        // from under it would send that load to a different
                        // playlist. Both windows are bounded (the switch
                        // flag self-clears after 2s; a game launch blocks
                        // this loop entirely while RetroArch owns the box).
                        LOG_DEBUG("playlists_reload_request held — playlist switch / game launch in flight");
                    } else {
                        std::error_code rm_ec;
                        fs::remove(playlists_marker, rm_ec);

                        // Reuse the directory BOOT resolved, never a fresh
                        // search-path walk: every item path this session
                        // resolves goes through playlist_directory, so
                        // loading playlists from some other directory would
                        // list files the player then cannot find. The only
                        // exception is a box where boot found no playlist
                        // directory at all — which is exactly the box an
                        // import is trying to populate — so resolve once
                        // there and keep both uses in step.
                        if (playlist_directory.empty()) {
                            for (const auto& search_path : config::get_playlist_search_paths()) {
                                if (fs::exists(search_path)) {
                                    playlist_directory = search_path;
                                    break;
                                }
                            }
                        }

                        if (playlist_directory.empty()) {
                            LOG_WARN("Playlist reload requested but no playlist directory exists");
                            ui::Toast::show("Playlist reload failed - no playlist folder");
                        } else {
                            // Identity snapshot, taken BEFORE the swap:
                            // what is playing, the menu cursor, and the open
                            // game list are all indexes into the vectors
                            // about to be replaced.
                            const bool game_list_open =
                                settings_menu.is_game_browser_active() &&
                                settings_menu.is_viewing_games_in_playlist();
                            const app::PlaylistReloadSnapshot reload_snap =
                                app::snapshot_for_reload(
                                    state, game_list_open
                                               ? settings_menu.get_current_game_playlist_index()
                                               : -1);

                            // The boot sequence, re-run: load -> platform
                            // filter -> UI split -> Master Shuffle row,
                            // through the same function boot uses. Playback
                            // is re-anchored, never interrupted — unless its
                            // playlist is gone (then controller.stop() +
                            // stop_to_menu).
                            app::apply_reloaded_playlists(
                                state,
                                app::split_for_ui_with_master_shuffle(
                                    PlaylistLoader::filter_for_platform(
                                        PlaylistLoader::load_playlists(playlist_directory),
                                        state.platform_profile)),
                                reload_snap,
                                [&controller]() { controller.stop(); });

                            // ── Settings game browser ────────────────────
                            if (settings_menu.is_game_browser_active()) {
                                if (settings_menu.is_viewing_games_in_playlist()) {
                                    const app::OpenGameListRemap remap =
                                        app::remap_open_game_list(
                                            game_playlists, reload_snap.open_game_id,
                                            settings_menu.get_current_game_playlist_index());
                                    if (remap.action == app::OpenGameListRemap::Action::Exit) {
                                        settings_menu.exit_game_list();
                                    } else if (remap.action == app::OpenGameListRemap::Action::Enter) {
                                        settings_menu.enter_game_list(remap.index);
                                    }
                                }
                                // navigate(0, ...) re-clamps both game
                                // cursors against the new sizes without
                                // moving them.
                                int games_in_current = 0;
                                if (settings_menu.is_viewing_games_in_playlist()) {
                                    games_in_current = app::games_in_game_playlist(
                                        game_playlists,
                                        settings_menu.get_current_game_playlist_index());
                                }
                                settings_menu.navigate(
                                    0, static_cast<int>(game_playlists.size()),
                                    games_in_current);
                            }

                            LOG_INFO("Playlists reloaded from disk: {} video, {} game "
                                     "(web-admin import poke)",
                                     state.playlists.size(), game_playlists.size());
                            ui::Toast::show("Playlists updated");
                        }
                    }
                }
            }
        }

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

        // Post-game fade-up: a black quad over the WHOLE finished frame, its
        // alpha falling to zero over kPostGameFadeMs, so the menu, the CRT
        // effects and the bezel all rise out of black together as one image.
        //
        // It used to be drawn before the bezel, on the reasoning that a bezel
        // held solid across the round trip reads as one continuous frame. On
        // the TV it does not: RetroArch owned the whole screen a moment
        // earlier, so the bezel has nothing to be continuous WITH, and it
        // snapped to full opacity over a menu that was still fading — the
        // frame arrived before the picture it frames. Drawing last is what
        // "the whole thing eases in" actually looks like.
        //
        // Deliberately the last draw of the frame, after the Media Browser
        // dispatcher and the marquee, so nothing can appear over the top of
        // it at full brightness. Costs nothing when idle: the block is
        // skipped entirely unless a fade is in flight.
        //
        // A separate mechanism from is_fading/ui_overlay_alpha on purpose:
        // that path is entangled with video-active and UI-visibility
        // semantics and early-returns on is_transitioning, which
        // prepare_kiosk_state_after_game only incidentally avoids.
        //
        // -1 is the "requested" sentinel from prepare_kiosk_state_after_game;
        // the clock starts at the FIRST FRAME WE ACTUALLY DRAW, not when the
        // request was made — the reset_display work (frame_presenter/EGL/GStreamer
        // re-init) between the two can eat 200ms+, and a wall-clock start
        // would leave the fade mostly over before the first frame rendered.
        //
        // Drawing, part 2 — skipped with part 1 by the redraw gate. The
        // fade's own clock (the -1 sentinel above) therefore also starts at
        // the first DRAWN frame; the gate always draws while it is pending.
        if (draw_this_frame) {
            {
                int64_t fade_start = state.post_game_fade_start_ms.load();
                if (fade_start != 0) {
                    constexpr int64_t kPostGameFadeMs = 250;
                    const int64_t now_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
                    if (fade_start < 0) {
                        state.post_game_fade_start_ms.store(now_ms);
                        fade_start = now_ms;
                    }
                    const int64_t elapsed = now_ms - fade_start;
                    if (elapsed >= kPostGameFadeMs) {
                        state.post_game_fade_start_ms.store(0);
                    } else {
                        glViewport(0, 0, mode.width, mode.height);
                        ui_renderer.render_post_game_fade(
                            1.0f - static_cast<float>(elapsed) /
                                       static_cast<float>(kPostGameFadeMs));
                    }
                }
            }

            // Debug screenshot: read back the finished frame (everything
            // above, post-game fade included) before the swap hands the
            // buffer to the presenter. No-op unless requested. The frame's
            // UI draw-call count is logged with it — the batching A/B for
            // one specific screen.
            if (screenshot_capture.pending()) {
                ui_renderer.flush_ui_batch();
                LOG_INFO("Screenshot frame: {} UI draw calls (UI batching {})",
                         ui_renderer.ui_draw_calls(),
                         ui_renderer.ui_batching_enabled() ? "ON" : "OFF");
            }
            screenshot_capture.capture_before_swap(mode.width, mode.height);

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
            last_present_done = std::chrono::steady_clock::now();

            // UI draw calls per drawn frame, reported per minute (first
            // window at INFO so the journal alone shows it; DEBUG after).
            {
                const uint64_t draws = ui_renderer.take_ui_draw_calls();
                ui_draw_window.frames++;
                ui_draw_window.sum += draws;
                if (draws > ui_draw_window.max) ui_draw_window.max = draws;
                if (last_present_done - ui_draw_window.start >= std::chrono::seconds(60)) {
                    if (ui_draw_window.frames > 0) {
                        const uint64_t avg = ui_draw_window.sum / ui_draw_window.frames;
                        if (!ui_draw_window.reported_once) {
                            LOG_INFO("UI draw calls/frame (last 60s): avg {} max {} over {} "
                                     "frames (UI batching {})",
                                     avg, ui_draw_window.max, ui_draw_window.frames,
                                     ui_renderer.ui_batching_enabled() ? "ON" : "OFF");
                        } else {
                            LOG_DEBUG("UI draw calls/frame (last 60s): avg {} max {} over {} "
                                      "frames (UI batching {})",
                                      avg, ui_draw_window.max, ui_draw_window.frames,
                                      ui_renderer.ui_batching_enabled() ? "ON" : "OFF");
                        }
                        ui_draw_window.reported_once = true;
                    }
                    ui_draw_window.start = last_present_done;
                    ui_draw_window.frames = ui_draw_window.sum = ui_draw_window.max = 0;
                }
            }
        }  // if (draw_this_frame) — drawing, part 2
        
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
        // Vblank-anchored: sleeps from THIS iteration's present completion
        // (utils/frame_pacing.h). The old `target - delta` used the
        // previous iteration's period, so 30 fps flips alternated
        // 16/33/50 ms. An iteration the redraw gate skipped never blocked
        // on a flip, so the min_iteration floor is what paces it: ~one
        // refresh per iteration, keeping input polling at frame cadence
        // without spinning a core.
        const auto pacing = utils::frame_pacing_for(
            mb_movie_active ? 30 : 60, static_cast<int>(mode_info.vrefresh));
        // CRT field rate: a skipped iteration sleeps until just past the
        // vblank it skips, so the drawing iteration after it has a full
        // refresh to render and flips exactly two vblanks after the last
        // flip (utils::field_rate_skip_sleep).
        const auto pace_now = std::chrono::steady_clock::now();
        const auto iteration_elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(pace_now - now);
        const auto pace_sleep =
            (crt_field_rate_this_iteration && !draw_this_frame)
                ? utils::field_rate_skip_sleep(
                      pacing, static_cast<int>(mode_info.vrefresh),
                      std::chrono::duration_cast<std::chrono::microseconds>(
                          pace_now - last_present_done),
                      iteration_elapsed)
                : utils::frame_cap_sleep(pacing, iteration_elapsed);
        if (pace_sleep.count() > 0) std::this_thread::sleep_for(pace_sleep);
    }
    
#ifdef HAVE_SYSTEMD
    sd_notify(0, "STOPPING=1");
#endif

    // Cleanup
    if (g_shutdown_requested) {
        LOG_INFO("Shutdown requested by signal (systemctl stop / SIGTERM)");
    }

#ifdef MEDIA_BROWSER_ENABLED
    // SHUTDOWN watch-state flush (a movie or episode still on screen).
    // MUST run before the cleanup below: player.cleanup() stops the
    // pipeline and zeroes position, and flushing after it would write
    // (0, 0) over a real resume point. The resume it queues (Playback's
    // leave()) is drained at the bottom of main. See
    // MediaBrowserHost::flush_before_shutdown.
    mb_host.flush_before_shutdown();
#endif

    LOG_INFO("Shutting down...");
    ui_renderer.cleanup();
    gst_renderer.cleanup();
    player.cleanup();
    input.cleanup();
    // Before egl/gbm: the buffer it still holds belongs to their surface.
    frame_presenter.shutdown();
    egl.cleanup();
    gbm.cleanup();
    display.cleanup();

    LOG_INFO("Shutdown complete");
    logging::shutdown();

    // Leave NOW, without unwinding static destructors.
    //
    // Everything above this line is the real teardown, and it finishes fast.
    // What came after it did not: measured on the box, the process logged
    // "Shutdown complete" and then sat there for another 4.8 SECONDS until
    // systemd's TimeoutStopSec=5 ran out and SIGKILLed it — on every single
    // stop. Static teardown of the process-lifetime singletons (background
    // pollers, GStreamer, the HTTP clients' worker threads) blocks; none of
    // it has anything left to save.
    //
    // That 5 seconds is not cosmetic. It is charged to the standby switch,
    // where the owner is watching the screen and waiting, and to the restart
    // button, and it made systemd mark a perfectly clean shutdown as
    // "Failed with result 'timeout'" every time — noise that hides a real
    // failure the day there is one.
    //
    // Safe because nothing is left to persist: watch position is flushed
    // above, settings save at each change site rather than at exit (14 call
    // sites in the settings menu alone), and logging::shutdown() has already
    // flushed and closed the log. The kernel reclaims the rest, exactly as it
    // would after the SIGKILL this replaces.
#ifdef MEDIA_BROWSER_ENABLED
    // The game-end hook restores the services it quieted (docker start of
    // the arr containers, then qBit resume) asynchronously. A stop mid-game
    // — or the display-lost exit — reaches here right after the game, and
    // KillMode=mixed SIGKILLs whatever is still running once main exits:
    // measured on a Pi 5, the stop killed the in-flight `docker start`.
    // Bounded so the whole shutdown stays inside TimeoutStopSec (20 s);
    // anything slower is still recovered at the next start.
    (void)game_quiet_mode.wait_until_idle_for(std::chrono::seconds(10));
    // A movie resume queued by leave() (above, or a user exit moments
    // before the stop) runs asynchronously; give it a bounded window so
    // the qBit resume / cap clear land. Bounded well inside
    // TimeoutStopSec (20 s) — anything left is covered at the next start:
    // the startup unpause (containers), the alt-limit clear (cap) and the
    // qbit_paused_by_kiosk marker recovery (paused torrents).
    (void)movie_quiet_mode.wait_until_idle_for(std::chrono::seconds(3));
#endif
    std::fflush(nullptr);
    _exit(controller.display_lost() ? 1 : 0);
}
