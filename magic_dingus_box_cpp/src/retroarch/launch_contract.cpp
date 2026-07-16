#include "launch_contract.h"

#include <ostream>

#include "utils/config.h"

namespace retroarch {

void write_video_config(std::ostream& out, const LaunchOptions& options) {
    // --- Common video settings (apply to both modes) ---
    out << "video_driver = \"vulkan\"\n";
    // The kiosk and RetroArch both run without X11/Wayland. Pinning the
    // Vulkan context to KMS prevents an inherited desktop environment from
    // sending RetroArch down a compositor path that does not exist.
    out << "video_context_driver = \"kms\"\n";
    // video_threaded MUST be false on this Pi 4B / V3D + Vulkan-KMS combo.
    // With threaded video ON, RetroArch's video thread races the V3D
    // KMS present path and the Vulkan swapchain thrashes:
    //   "[Vulkan]: QueuePresent failed, destroying swapchain" repeating,
    //   ~19 times per launch, with the driver rebuilding the swapchain
    //   over and over. When that recovery loop doesn't converge the
    //   screen stays black and the game never appears — the intermittent
    //   "launch didn't reach RetroArch" failure. Measured live: threaded
    //   ON => QueuePresent-failed count 1+ and climbing; threaded OFF =>
    //   0, consistently across repeated launches. Isolated to THIS flag
    //   (swapchain-images=2 and hard_sync alone did NOT fix it).
    //
    // Tradeoff: threaded video decouples the GPU present from the
    // emulation core, which can smooth framepacing for heavier cores
    // (PS1). A game that runs is strictly better than one that
    // black-screens, and on this hardware 2D/PS1 content stays full-
    // speed single-threaded anyway. If a specific heavy title ever needs
    // it back, do it per-core, not globally.
    out << "video_threaded = \"false\"\n";
    out << "video_fullscreen = \"true\"\n";
    out << "video_windowed_fullscreen = \"false\"\n";
    out << "video_gpu_screenshot = \"false\"\n";
    // Allow cores to request display rotation via RETRO_ENVIRONMENT_SET_
    // ROTATION. In practice only FBNeo uses this — it ships a curated
    // per-game rotation database covering the classic vertical-monitor
    // arcade cabinets (Pac-Man, Galaga, Donkey Kong, 1942, Bombjack,
    // Xevious, Pole Position, etc.). With this set to false, those games
    // render sideways because the vertical native framebuffer (e.g.
    // Pac-Man's 224x288) gets stretched into our horizontal viewport.
    // True lets RetroArch rotate them 90° so they display correctly,
    // letterboxed inside the bezel cutout (mimics the side-curtain
    // border real arcade cabinets had around vertical CRTs).
    //
    // Safe for non-arcade cores: nestopia / snes9x2010 / pcsx_rearmed /
    // genesis_plus_gx / mednafen_pce_fast / prosystem all leave rotation
    // at 0 (no horizontal/vertical mix in their game catalogs), so this
    // flag is effectively a no-op for them.
    out << "video_allow_rotate = \"true\"\n";
    out << "video_crop_overscan = \"false\"\n";
    out << "video_refresh_rate = \"60.000000\"\n";
    out << "video_aspect_ratio = \"1.333\"\n";
    out << "video_force_aspect = \"true\"\n";
    out << "video_scale = \"1.0\"\n";
    out << "video_scale_integer = \"false\"\n";
    out << "video_scale_filter = \"0\"\n";
    out << "video_smooth = \"false\"\n";
    out << "video_rotation = \"0\"\n";
    out << "video_hard_sync = \"false\"\n";
    out << "video_vsync = \"true\"\n";
    out << "video_frame_delay = \"4\"\n";
    // 2 (double-buffer) is the more stable swapchain depth for the V3D
    // KMS Vulkan path; 3 gave no measured benefit here and pairs with the
    // threaded-video thrash above. Belt-and-suspenders alongside
    // video_threaded=false.
    out << "video_max_swapchain_images = \"2\"\n";
    out << "video_shader_enable = \"false\"\n";
    out << "video_filter = \"\"\n";
    out << "video_frame_blend = \"false\"\n";
    out << "video_gpu_record = \"false\"\n";
    out << "video_record = \"false\"\n";
    out << "video_disable_composition = \"false\"\n";

    // --- Mode-specific resolution + viewport + overlay ---
    if (options.display_mode == app::DisplayMode::MODERN_TV) {
        // 1920x1080 output, game rendered into a 4:3 viewport centered
        // on the bezel's screen cutout. Cutout intersection across both
        // bezel families is (251, 10, 1415, 1059). RetroArch enforces
        // strict 4:3 inside that viewport via video_force_aspect above.
        out << "video_fullscreen_x = \"1920\"\n";
        out << "video_fullscreen_y = \"1080\"\n";
        out << "video_windowed_width = \"1920\"\n";
        out << "video_windowed_height = \"1080\"\n";
        out << "video_custom_viewport_enable = \"true\"\n";
        out << "video_custom_viewport_x = \"251\"\n";
        out << "video_custom_viewport_y = \"10\"\n";
        out << "video_custom_viewport_width = \"1415\"\n";
        out << "video_custom_viewport_height = \"1059\"\n";
        out << "aspect_ratio_index = \"22\"\n";  // 22 = custom viewport

        // Bezel overlay (optional — skipped when user has procedural "Simple" selected)
        if (!options.bezel_file.empty()) {
            // Swap .png -> .cfg (RetroArch overlay configs live alongside PNGs)
            std::string cfg_name = options.bezel_file;
            const auto dot = cfg_name.rfind('.');
            if (dot != std::string::npos) {
                cfg_name.replace(dot, std::string::npos, ".cfg");
            } else {
                cfg_name += ".cfg";
            }
            const std::string cfg_path = config::get_bezels_dir() + "/" + cfg_name;

            out << "input_overlay = \"" << cfg_path << "\"\n";
            out << "input_overlay_enable = \"true\"\n";
            out << "input_overlay_opacity = \"1.0\"\n";
            // Hide overlay when RetroArch's in-game menu is open so it doesn't
            // obscure the menu UI (Z+Start hotkey). Overlay returns on menu close.
            out << "input_overlay_hide_in_menu = \"true\"\n";
        }
    } else {
        // CRT_NATIVE: 640x480, no custom viewport (unchanged from pre-change behavior)
        out << "video_fullscreen_x = \"640\"\n";
        out << "video_fullscreen_y = \"480\"\n";
        out << "video_windowed_width = \"640\"\n";
        out << "video_windowed_height = \"480\"\n";
        out << "video_custom_viewport_enable = \"false\"\n";
        out << "aspect_ratio_index = \"23\"\n";
    }
}

}  // namespace retroarch
