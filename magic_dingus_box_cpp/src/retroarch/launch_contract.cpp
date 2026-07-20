#include "launch_contract.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <ostream>
#include <sstream>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

#include "utils/config.h"

namespace retroarch {

namespace {

std::string shell_single_quote(const std::string& value) {
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    quoted += "'";
    return quoted;
}

bool is_ps1_core(const std::string& core_name) {
    return core_name.find("pcsx") != std::string::npos ||
           core_name.find("beetle_psx") != std::string::npos ||
           core_name.find("swanstation") != std::string::npos;
}

}  // namespace

void write_video_config(std::ostream& out, const LaunchOptions& options) {
    // --- Common video settings (apply to both modes) ---
    out << "video_driver = \"vulkan\"\n";
    // The kiosk and RetroArch both run without X11/Wayland. RetroArch's
    // Vulkan driver names its direct DRM/KMS context "khr_display" (the
    // separate "kms" identifier belongs to the EGL/OpenGL context). Pinning
    // the correct Vulkan context avoids a compositor probe and goes straight
    // to VK_KHR_display on the Pi's V3D device.
    out << "video_context_driver = \"khr_display\"\n";
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
    //
    // Pi 5 note: this workaround (and max_swapchain_images=2 below) was
    // measured on the Pi 4's V3D 4.2 Vulkan driver. The Pi 5's V3D 7.1
    // driver may not need either — when a Pi 5 is available to bench,
    // branch on options.pi_model here and update the parity test in
    // test_launch_contract.cpp ("identical across Pi models").
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
    // Auto mode (RetroArch >= 1.9.13): treat 4 as the target and back the
    // effective delay off automatically if a heavy scene makes frames run
    // long — converts a would-be stutter into a 4 ms latency give-back.
    out << "video_frame_delay_auto = \"true\"\n";
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

namespace {

std::string lowercase(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

// Per-title PS1 performance overrides. THPS4's engine chugs on REAL
// PlayStation hardware (30fps target, low-20s in big parks) — the
// emulator reproduces that authentic slowdown at the native clock while
// using <20% of one Pi core. A modest emulated-CPU overclock plus
// disabled stall emulation lets the game engine hit its frame target
// the way it never could on the original console. Scoped per-title
// because both knobs can break timing-sensitive games.
struct Ps1TitleOverride {
    const char* rom_name_substring;  // matched lowercase against rom_path
    const char* psxclock;
    bool nostalls;
};

constexpr Ps1TitleOverride kPs1TitleOverrides[] = {
    {"tony hawk's pro skater 4", "65", true},
};

const Ps1TitleOverride* find_ps1_override(const std::string& rom_path) {
    const std::string haystack = lowercase(rom_path);
    for (const auto& override_entry : kPs1TitleOverrides) {
        if (haystack.find(override_entry.rom_name_substring) !=
            std::string::npos) {
            return &override_entry;
        }
    }
    return nullptr;
}

}  // namespace

void write_core_options(std::ostream& out, const std::string& core_name,
                        const std::string& rom_path) {
    if (!is_ps1_core(core_name)) {
        return;
    }
    const Ps1TitleOverride* title_override = find_ps1_override(rom_path);

    out << "pcsx_rearmed_pad1type = \"analog\"\n";
    // Offload SPU audio to a separate CPU core (Pi 4B has four cores).
    out << "pcsx_rearmed_spu_thread = \"enabled\"\n";
    // Retain CD audio and XA decoding for the complete game soundtrack.
    out << "pcsx_rearmed_nocdaudio = \"disabled\"\n";
    out << "pcsx_rearmed_noxadecoding = \"disabled\"\n";
    // The Pi has measured emulation headroom. Threshold frame skipping made
    // the core drop up to three consecutive frames and request 128 ms audio
    // latency even at full speed, producing the observed choppy response.
    out << "pcsx_rearmed_frameskip_type = \"disabled\"\n";
    // Fast GPU linked-list processing.
    out << "pcsx_rearmed_gpu_slow_llists = \"disabled\"\n";
    // ARM64 dynamic recompilation is critical for PS1 performance.
    out << "pcsx_rearmed_drc = \"enabled\"\n";
    // Preserve compatibility for games that depend on cache behavior.
    out << "pcsx_rearmed_icache_emulation = \"enabled\"\n";
    // Native PSX clock by default; heavy titles get a per-title bump.
    out << "pcsx_rearmed_psxclock = \""
        << (title_override ? title_override->psxclock : "57") << "\"\n";
    if (title_override && title_override->nostalls) {
        // Skip CPU stall-cycle emulation: faster-than-real-hardware
        // execution for engine-bound titles, at a small accuracy cost.
        out << "pcsx_rearmed_nostalls = \"enabled\"\n";
    }
    // Retain the established low-overhead audio options.
    out << "pcsx_rearmed_spu_interpolation = \"off\"\n";
    out << "pcsx_rearmed_spu_reverb = \"disabled\"\n";
    // Render at native 1x resolution; RetroArch scales into the bezel.
    out << "pcsx_rearmed_neon_enhancement_enable = \"disabled\"\n";
    out << "pcsx_rearmed_dithering = \"enabled\"\n";
}

const char* audio_driver_for_gameplay() {
    return "alsathread";
}

int audio_latency_ms_for_core(const std::string& core_name) {
    // PS1 was 64 during the alsathread migration; walked back down to 48
    // after a clean zero-retrigger soak (Track 2, 2026-07-16). If PS1
    // crackle ever returns, raise the PS1 branch back to 64 first.
    (void)core_name;
    return 48;
}

std::string build_kms_ready_watch_block(const std::string& command,
                                        const ReadyWatchOptions& options) {
    std::ostringstream block;
    block << "unset DISPLAY WAYLAND_DISPLAY XDG_SESSION_TYPE SDL_VIDEODRIVER\n";
    block << "RETROARCH_READY_FILE=" << shell_single_quote(options.ready_file)
          << "\n";
    block << "rm -f \"$RETROARCH_READY_FILE\"\n";
    block << command << " &\n";
    block << "RETROARCH_PID=$!\n";
    block << "while kill -0 \"$RETROARCH_PID\" 2>/dev/null; do\n";
    block << "    for fd in /proc/$RETROARCH_PID/fd/*; do\n";
    block << "        target=$(readlink \"$fd\" 2>/dev/null || true)\n";
    block << "        case \"$target\" in\n";
    block << "            " << options.drm_card_pattern << ")\n";
    block << "                printf '%s\\n' \"$RETROARCH_PID\" > "
             "\"$RETROARCH_READY_FILE\"\n";
    block << "                break 2\n";
    block << "                ;;\n";
    block << "        esac\n";
    block << "    done\n";
    block << "    sleep 0.05\n";
    block << "done\n";
    block << "wait \"$RETROARCH_PID\"\n";
    block << "RETROARCH_EXIT=$?\n";
    return block.str();
}

StartupStatus wait_for_startup(pid_t launcher_pid,
                               const std::string& ready_file,
                               std::chrono::milliseconds timeout,
                               std::chrono::milliseconds poll_interval) {
    if (launcher_pid <= 0) {
        return StartupStatus::WaitError;
    }

    if (poll_interval <= std::chrono::milliseconds::zero()) {
        poll_interval = std::chrono::milliseconds(1);
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        int status = 0;
        const pid_t wait_result = waitpid(launcher_pid, &status, WNOHANG);
        if (wait_result == launcher_pid) {
            return StartupStatus::Exited;
        }
        if (wait_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                return StartupStatus::Exited;
            }
            return StartupStatus::WaitError;
        }

        std::error_code marker_error;
        if (std::filesystem::exists(ready_file, marker_error) && !marker_error) {
            return StartupStatus::Ready;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            return StartupStatus::TimedOut;
        }
        std::this_thread::sleep_for(poll_interval);
    }
}

bool terminate_process_group(pid_t launcher_pid,
                             std::chrono::milliseconds grace) {
    if (launcher_pid <= 0) {
        return false;
    }

    if (kill(-launcher_pid, SIGTERM) != 0 && errno != ESRCH) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + grace;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t wait_result = waitpid(launcher_pid, &status, WNOHANG);
        if (wait_result == launcher_pid) {
            return true;
        }
        if (wait_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno == ECHILD;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (kill(-launcher_pid, SIGKILL) != 0 && errno != ESRCH) {
        return false;
    }

    while (true) {
        int status = 0;
        const pid_t wait_result = waitpid(launcher_pid, &status, 0);
        if (wait_result == launcher_pid) {
            return true;
        }
        if (wait_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno == ECHILD;
        }
    }
}

}  // namespace retroarch
