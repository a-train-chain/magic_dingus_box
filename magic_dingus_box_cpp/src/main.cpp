#include "platform/drm_display.h"
#include "platform/gbm_context.h"
#include "platform/egl_context.h"
#include "platform/input_manager.h"
#include "platform/gpio_manager.h"
#include "video/gst_player.h"
#include "video/gst_renderer.h"
#include "ui/renderer.h"
#include "ui/settings_menu.h"
#include "app/app_state.h"
#include "app/playlist_loader.h"
#include "app/controller.h"
#include "app/sample_mode.h"
#include "app/settings_persistence.h"
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
#include <unordered_map>
#include <vector>
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
    
    // Set display mode (auto-detect preferred mode)
    // For Modern TV mode with bezels, we need a 16:9 resolution
    // Try preferred resolution first, then fallbacks, then auto-detect
    if (!display.set_mode(config::display::PREFERRED_WIDTH, config::display::PREFERRED_HEIGHT)) {
        LOG_INFO("{}x{} not available, trying {}x{}...",
                 config::display::PREFERRED_WIDTH, config::display::PREFERRED_HEIGHT,
                 config::display::FALLBACK_WIDTH_1, config::display::FALLBACK_HEIGHT_1);
        if (!display.set_mode(config::display::FALLBACK_WIDTH_1, config::display::FALLBACK_HEIGHT_1)) {
            LOG_INFO("{}x{} not available, trying {}x{} (CRT native)...",
                     config::display::FALLBACK_WIDTH_1, config::display::FALLBACK_HEIGHT_1,
                     config::display::FALLBACK_WIDTH_2, config::display::FALLBACK_HEIGHT_2);
            if (!display.set_mode(config::display::FALLBACK_WIDTH_2, config::display::FALLBACK_HEIGHT_2)) {
                LOG_INFO("{}x{} not available, using auto-detect...",
                         config::display::FALLBACK_WIDTH_2, config::display::FALLBACK_HEIGHT_2);
                if (!display.set_mode(0, 0)) {
                    LOG_ERROR("Failed to set display mode");
                    logging::shutdown();
                    return 1;
                }
            }
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
    
    // Insert "Master Shuffle" playlist at the beginning
    Playlist master_shuffle;
    master_shuffle.title = "[S] Master Shuffle";
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
    
    // Probe hardware resolution but RESPECT saved user settings.
    // Adapters often report High Res (720p) even for CRTs, so we shouldn't auto-switch based on resolution alone.
    // Instead, we rely on our Fallback Logic and Logical Scaling to handle whatever the user chose.
    if (display.set_mode(0, 0)) {
        auto probe = display.get_current_mode();
        std::cout << "Hardware Probe: " << probe.width << "x" << probe.height << std::endl;
    } else {
        std::cerr << "Hardware Probe failed. Defaulting to safe CRT_NATIVE resolution logic." << std::endl;
    }
    
    // Apply Mode-Specific Resolution logic using SAVED state
    if (state.display_settings.mode == app::DisplayMode::CRT_NATIVE) {
        // CRT Native: Use Auto/Preferred resolution
        if (display.set_mode(0, 0)) {
             // SAFETY CHECK: If Auto mode picked a very high resolution (e.g. 4K),
             // it might exceed Pi 5 bandwidth/plane limits with our overlay setup.
             // Clamp to safe resolution to ensure image is visible.
             auto current = display.get_current_mode();
             if (current.height > config::display::CRT_MAX_HEIGHT) {
                 std::cout << "Native resolution " << current.height << "p too high for CRT Native mode safely. Clamping to "
                           << config::display::PREFERRED_HEIGHT << "p." << std::endl;
                 if (display.set_mode(config::display::PREFERRED_WIDTH, config::display::PREFERRED_HEIGHT)) {
                     std::cout << "Clamped to " << config::display::PREFERRED_WIDTH << "x"
                               << config::display::PREFERRED_HEIGHT << "." << std::endl;
                 } else {
                     std::cerr << "Clamp to " << config::display::PREFERRED_HEIGHT << "p failed. Reverting to Auto." << std::endl;
                     display.set_mode(0, 0);
                 }
             }
             mode_applied = true;
        } else {
             std::cerr << "CRT Auto-Mode failed!" << std::endl;
        }
    } else {
         // Modern TV: Force preferred resolution for best performance/scaling integration
         if (display.set_mode(config::display::PREFERRED_WIDTH, config::display::PREFERRED_HEIGHT)) {
             std::cout << "Forcing Modern TV resolution: " << config::display::PREFERRED_WIDTH << "x"
                       << config::display::PREFERRED_HEIGHT << std::endl;
             mode_applied = true;
         } else {
             std::cerr << "Failed to set Modern TV " << config::display::PREFERRED_WIDTH << "x"
                       << config::display::PREFERRED_HEIGHT << "!" << std::endl;
         }
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
    
    // For CRT Native, force logical 640x480 layout regardless of physical resolution
    // This ensures large text and correct aspect ratio (assuming anamorphic squeeze)
    if (state.display_settings.mode == app::DisplayMode::CRT_NATIVE) {
        ui_renderer.resize_screen(640, 480);
    } else {
        ui_renderer.resize_screen(mode.width, mode.height);
    }
    
    // Track current mode to detect changes at runtime
    app::DisplayMode current_display_mode = state.display_settings.mode;

    while (running) {
        // Skip rendering if display is cleaned up (RetroArch is running)
        if (display.get_fd() < 0) {
            // Display is closed - RetroArch has taken over
            // Just sleep and continue loop (don't render, don't poll input, don't do anything)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
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
            
            bool ok = false;
            if (current_display_mode == app::DisplayMode::CRT_NATIVE) {
                // Switch to Auto/Preferred (Native)
                ok = display.set_mode(0, 0);
                if (ok) {
                     auto current = display.get_current_mode();
                     if (current.height > config::display::CRT_MAX_HEIGHT) {
                         std::cout << "Native resolution too high. Clamping to "
                                   << config::display::PREFERRED_HEIGHT << "p." << std::endl;
                         display.set_mode(config::display::PREFERRED_WIDTH,
                                          config::display::PREFERRED_HEIGHT); // Best effort clamp
                     }
                }
            } else {
                // Switch to preferred resolution for Modern TV
                ok = display.set_mode(config::display::PREFERRED_WIDTH, config::display::PREFERRED_HEIGHT);
                if (!ok) ok = display.set_mode(config::display::FALLBACK_WIDTH_1, config::display::FALLBACK_HEIGHT_1);
            }
            
            if (ok) {
                // Update mode info and notify renderers
                mode = display.get_current_mode();
                std::cout << "Switched to: " << mode.name << " (" << mode.width << "x" << mode.height << ")" << std::endl;
                
                gst_renderer.set_screen_size(mode.width, mode.height);
                
                if (current_display_mode == app::DisplayMode::CRT_NATIVE) {
                    ui_renderer.resize_screen(640, 480);
                } else {
                    ui_renderer.resize_screen(mode.width, mode.height);
                }
                
                // Ensure viewport is reset
                glViewport(0, 0, mode.width, mode.height);
                glClear(GL_COLOR_BUFFER_BIT);
            }
        }
        
        static int frame_count = 0;
        auto now = std::chrono::steady_clock::now();
        auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_frame).count();
        last_frame = now;
        
        // Update menu state (Wi-Fi scanning, etc.)
        settings_menu.update();

        // Poll input
        auto input_events = input.poll();
        
        // Poll GPIO (buttons, encoder) and merge with controller/keyboard events
        if (gpio.is_available()) {
            auto gpio_events = gpio.poll();
            input_events.insert(input_events.end(), gpio_events.begin(), gpio_events.end());
        }
        
        // Track Menu button state for volume control
        static bool menu_button_held = false;
        static bool volume_changed_while_held = false;
        static std::chrono::steady_clock::time_point menu_press_time;
        
        // Time-based check for showing slider (if held long enough)
        if (menu_button_held && !state.show_volume_slider) {
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - menu_press_time).count();
            if (duration > 300) {
                state.show_volume_slider = true;
            }
        }
        
        for (const auto& ev : input_events) {
            // Handle Menu button hold logic
            if (ev.action == InputAction::SETTINGS_MENU) {
                if (ev.pressed) {
                    menu_button_held = true;
                    volume_changed_while_held = false;
                    menu_press_time = std::chrono::steady_clock::now();
                    state.show_volume_slider = false; // Don't show immediately
                } else {
                    menu_button_held = false;
                    state.show_volume_slider = false; // Hide immediately
                    
                    auto release_time = std::chrono::steady_clock::now();
                    auto hold_duration = std::chrono::duration_cast<std::chrono::milliseconds>(release_time - menu_press_time).count();
                    
                    // Only toggle menu if we didn't change volume AND it was a short press
                    if (!volume_changed_while_held && hold_duration < 300) {
                        // Only allow settings menu when UI is visible
                        bool ui_available = !state.video_active || state.ui_visible_when_playing;
                        if (ui_available) {
                            settings_menu.toggle();
                        }
                    } else if (volume_changed_while_held) {
                        // Volume was changed, save settings now
                        app::SettingsPersistence::save_settings(state);
                    }
                }
                continue;
            }
            
            // If Menu button is held, hijack Rotate/Up/Down for volume
            if (menu_button_held) {
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
                    volume_changed_while_held = true;
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
                    volume_changed_while_held = true;
                }
                continue; // Consume event
            }
            
            // New input handling structure
            // Check for toggle settings menu (always available unless keyboard open)
            if (ev.action == InputAction::SETTINGS_MENU && ev.pressed && !keyboard.is_active()) {
                settings_menu.toggle();
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
                                        
                                        // Create progress callback to keep UI alive during launch
                                        auto progress_callback = [&]() {
                                            // Clear screen
                                            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                                            // Render UI to show loading screen
                                            ui_renderer.render_loading_overlay(state);
                                            
                                            // Swap buffers (renders to GBM surface)
                                            egl.swap_buffers();
                                            
                                            // Present frame (flips DRM page)
                                            present_frame();
                                        };
                                        
                                        // Launch the game
                                        auto launch_result = controller.load_playlist_item(state, playlist, game_idx, playlist_directory, progress_callback);

                                        // Reset loading state
                                        state.is_loading_game = false;

                                        if (launch_result) {
                                            std::cout << "Game launched successfully" << std::endl;
                                            // Exit settings menu after successful launch
                                            // The wrapper script will stop the UI service, so we don't need to do anything else
                                            settings_menu.close();
                                        } else {
                                            std::cout << "Game launch failed: " << launch_result.error() << std::endl;
                                        }
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
                        } else if (section == ui::MenuSection::BROWSE_GAMES) {
                            // Enter game browser
                            settings_menu.enter_game_browser();
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
                        if (ev.delta > 0) {
                            state.selected_index = (state.selected_index + 1) % state.playlists.size();
                        } else if (ev.delta < 0) {
                            state.selected_index = (state.selected_index - 1 + state.playlists.size()) % state.playlists.size();
                        }
                        
                        // If video is active, show UI briefly
                        if (state.video_active) {
                            state.ui_visible_when_playing = true;
                            state.ui_visibility_timer = 3.0; // Show for 3 seconds
                        }
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
                            }
                            // Track which playlist and item is playing (already set above)
                            // When starting video, hide UI completely so video shows through fully
                            state.ui_visible_when_playing = false;
                            // Original volume will be captured in update_state when video becomes active
                        }
                    }

                    // If load failed, clear the flag immediately to allow retry
                    if (!load_success) {
                        state.is_switching_playlist = false;
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
                                // Original volume will be captured in update_state when video becomes active
                            } else {
                                // Load failed - clear flag immediately
                                std::cerr << "Failed to load playlist item: " << load_result.error() << std::endl;
                                state.is_switching_playlist = false;
                            }
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
                            // In Master Shuffle, PREV also triggers another random video
                            controller.play_random_global_video(state, playlist_directory);
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
               // Always update during intro, and after intro update very frequently to ensure video_active is set
               if (!state.intro_complete || state.is_switching_playlist || frame_count % 2 == 0) {
                   controller.update_state(state);
               }

               // GStreamer remains alive after intro, just ensure proper state management
        sample_mode.update_state(state);
        
        // Clear playlist switching flag if it's been stuck for too long (timeout safety)
        // This prevents the flag from getting stuck if video fails to load or MPV gets into bad state
        // Increased timeout to handle slow storage and MPV initialization issues
        if (state.is_switching_playlist) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.playlist_switch_start_time);
            if (elapsed.count() > 5000) {  // 5 second timeout (increased for robustness)
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
        if (state.showing_intro_video && state.video_active && state.duration > 0.0) {
            // Update LED dance animation during intro video
            // Use video position as elapsed time (in milliseconds)
            gpio.update_intro_animation(static_cast<uint64_t>(state.position * 1000));
            // Check if intro video has ended
            // Use multiple conditions to ensure reliable detection
            bool video_ended = false;

            // Primary check: position near end
            if (state.position >= state.duration - 0.5) {
                video_ended = true;
            }

            // Fallback check: if video stopped playing but we're still showing intro
            if (!controller.is_playing() && state.duration > 0.0 && state.position > 0.0) {
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
                state.duration = 0.0;
                state.position = 0.0;
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
            // Check if video has ended (position >= duration with small tolerance)
            // Only advance once per item (check that we haven't already advanced from this item)
            if (state.duration > 0.0 && state.position >= state.duration - 0.5) {
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
                              << " at position " << state.position << "/" << state.duration << std::endl;
                    // Set flag BEFORE calling load_next_item to prevent race conditions
                    state.last_advanced_item_index = state.current_item_index;
                    state.last_advanced_duration = state.duration;
                    // Note: load_next_item handles errors internally (skips broken files)
                    if (state.master_shuffle_active) {
                controller.play_random_global_video(state, playlist_directory);
            } else {
                controller.load_next_item(state, playlist_directory);
            }
                } else if (state.position >= state.duration - 0.5) {
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
                if (state.position < state.duration - 1.0) {
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
        static int last_render_decision = -1;
        if (should_render_video != last_render_decision) {
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
        int content_x = 0, content_y = 0, content_w = mode.width, content_h = mode.height;
        
        if (use_letterbox) {
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
        }
        
        // Render video - gst will fill the entire framebuffer, so no need to clear if video is ready
        // This handles both intro video and regular video playback
        if (should_render_video) {
            // Set letterbox mode based on display settings
            gst_renderer.set_letterbox_mode(use_letterbox);
            
            // In Modern TV mode, set viewport to 4:3 content area for video
            if (use_letterbox) {
                glViewport(content_x, content_y, content_w, content_h);
            }
            
            gst_renderer.render();
        }
        
        // Render UI overlay (will skip if intro video is showing - handled in renderer.cpp)
        // Only render UI if intro is not showing, or if intro is fading out
        // Note: ui_renderer.render() includes CRT effects at the end
        if (!state.showing_intro_video || state.intro_fading_out) {
            // Set viewport to 4:3 content area for UI in Modern TV mode
            // Also update UI renderer's internal dimensions for proper projection matrix
            glViewport(content_x, content_y, content_w, content_h);
            if (use_letterbox) {
                ui_renderer.set_content_viewport(content_w, content_h);
            }
            ui_renderer.render(state);
            
            // Reset viewport after UI render
            if (use_letterbox) {
                ui_renderer.reset_content_viewport();
            }
        }
        
        // Render bezel overlay LAST in Modern TV mode (on top of EVERYTHING including CRT effects)
        // The bezel PNG is stretched fullscreen - content is visible through the transparent center
        if (use_letterbox && !state.available_bezels.empty() && 
            state.display_settings.bezel_index >= 0 && 
            state.display_settings.bezel_index < static_cast<int>(state.available_bezels.size())) {
            const auto& bezel = state.available_bezels[state.display_settings.bezel_index];
            if (!bezel.file.empty()) {
                ui_renderer.load_bezel(bezel.file);
                // Reset viewport to fullscreen for bezel overlay
                glViewport(0, 0, mode.width, mode.height);
                ui_renderer.render_bezel();
            }
        }
        
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
        
        // Frame rate limiting (target 60 FPS)
        if (delta < 16) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16 - delta));
        }
    }
    
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

