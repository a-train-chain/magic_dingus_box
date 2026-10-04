#include "retroarch_launcher.h"
#include "controller_detector.h"
#include "controller_mapping.h"
#include "controller_profile.h"
#include "game_session.h"
#include "../utils/config.h"
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <filesystem>
#include <thread>
#include <chrono>
#include <fstream>
#include <iterator>
#include <vector>
#include <errno.h>
#include <ctime>
#include <sstream>
#include <cmath>

extern char** environ;

namespace fs = std::filesystem;

namespace retroarch {

namespace {

// Run a helper (udevadm, pkill) to completion with its output discarded.
int run_quiet(std::vector<const char*> argv) {
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
        execvp(argv[0], const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    if (pid < 0) return -1;
    int status = 0;
    pid_t r;
    do { r = waitpid(pid, &status, 0); } while (r < 0 && errno == EINTR);
    if (r != pid) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string timestamp_now() {
    const std::time_t t = std::time(nullptr);
    char buf[64];
    std::tm tm_buf{};
    if (localtime_r(&t, &tm_buf) == nullptr ||
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf) == 0) {
        return "?";
    }
    return buf;
}

// Write `contents` to `path` (truncating). false + log on failure.
bool write_file(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "Failed to write " << path << std::endl;
        return false;
    }
    out << contents;
    out.close();
    if (!out) {
        std::cerr << "Failed to write " << path << std::endl;
        return false;
    }
    return true;
}

// Delete the per-core .opt files that would shadow the options we write.
// RetroArch's config/<Core Name>/<Core Name>.opt takes precedence over
// core_options_path, so a stale one silently wins and every value in
// write_core_options() becomes a no-op — no error, no log line, just the
// core's defaults.
//
// This used to name PCSX-ReARMed's file literally, which meant PS1 worked
// and nothing else did. Verified on hardware 2026-07-28: with Flycast.opt in
// place a Dreamcast launch ran on "TV (Composite)" with DCNet enabled; with
// it parked, the same launch came up "VGA" with DCNet disabled.
// Mupen64Plus-Next.opt had been shadowing the N64 options the same way.
//
// Matched by option-key prefix rather than by core display name: the
// directory names are RetroArch's, not ours ("ParaLLEl N64", "Beetle PCE
// Fast"), and a hardcoded map silently stops matching when one is renamed
// upstream. The prefix comes from core_options_key_prefix(), which is
// unit-tested to cover every line write_core_options() emits.
void remove_shadowing_opt_files(const std::string& home,
                                const std::string& prefix) {
    if (prefix.empty()) return;
    std::error_code ec;
    const fs::path config_dir = fs::path(home) / ".config/retroarch/config";
    for (fs::directory_iterator core_dir(config_dir, ec), end;
         !ec && core_dir != end; core_dir.increment(ec)) {
        std::error_code inner_ec;
        if (!core_dir->is_directory(inner_ec)) continue;
        for (fs::directory_iterator f(core_dir->path(), inner_ec), fend;
             !inner_ec && f != fend; f.increment(inner_ec)) {
            if (f->path().extension() != ".opt") continue;
            std::ifstream in(f->path());
            const std::string contents((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
            if (opt_file_shadows_options(contents, prefix)) {
                std::error_code rm_ec;
                fs::remove(f->path(), rm_ec);
                std::cout << "Removed shadowing core options file: "
                          << f->path().string() << std::endl;
            }
        }
    }
}

std::vector<std::string> current_environment() {
    std::vector<std::string> env;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) env.emplace_back(*e);
    return env;
}

const char* outcome_name(SessionOutcome o) {
    switch (o) {
        case SessionOutcome::Exited: return "exited";
        case SessionOutcome::ExitedBeforeReady: return "exited before taking over KMS";
        case SessionOutcome::StartupTimedOut: return "did not take over KMS in time";
        case SessionOutcome::StopRequested: return "stopped on kiosk shutdown request";
    }
    return "?";
}

const char* stop_name(StopResult r) {
    switch (r) {
        case StopResult::AlreadyExited: return "quit on its own";
        case StopResult::Terminated: return "exited after SIGTERM";
        case StopResult::Killed: return "ignored SIGTERM; SIGKILLed";
    }
    return "?";
}

}  // namespace


RetroArchLauncher::RetroArchLauncher() : retroarch_available_(false) {
}

std::optional<std::string> RetroArchLauncher::find_core_dir(
        const std::string& core_name) {
    return resolve_core_dir(core_name, kSystemLibretroDir,
                            config::retroarch::get_cores_dir());
}

bool RetroArchLauncher::initialize() {
    retroarch_bin_ = find_retroarch();
    
    retroarch_available_ = retroarch_bin_.has_value();
    
    if (retroarch_available_) {
        std::cout << "RetroArch found at: " << retroarch_bin_.value() << std::endl;
    } else {
        std::cerr << "RetroArch not found. Install with: sudo apt install retroarch" << std::endl;
    }
    
    return retroarch_available_;
}

std::optional<std::string> RetroArchLauncher::find_retroarch() {
    std::vector<std::string> paths = {
        "/opt/retropie/emulators/retroarch/bin/retroarch",  // RetroPie
        "/usr/bin/retroarch",                              // Linux standard
        "/Applications/RetroArch.app/Contents/MacOS/RetroArch"  // macOS
    };
    
    for (const auto& path : paths) {
        if (fs::exists(path)) {
            return path;
        }
    }
    
    return std::nullopt;
}

bool RetroArchLauncher::launch_game(const GameLaunchInfo& game_info, int system_volume_percent, float volume_offset_db, int audio_output, const LaunchOptions& opts) {
    if (!retroarch_available_) {
        std::cerr << "RetroArch not available" << std::endl;
        return false;
    }
    
    // Validate ROM exists
    if (!fs::exists(game_info.rom_path)) {
        std::cerr << "ROM not found: " << game_info.rom_path << std::endl;
        return false;
    }

    // Missing core: refuse before release_controllers() or any teardown.
    if (!find_core_dir(game_info.core_name)) {
        std::cerr << "Core not installed: "
                  << libretro_core_name(game_info.core_name) << std::endl;
        return false;
    }

    release_controllers();
    
    // Always use DRM/KMS launch (matches app architecture)
    std::cout << "Launching RetroArch in DRM/KMS mode" << std::endl;
    return launch_drm(game_info, system_volume_percent, volume_offset_db, audio_output, opts);
}



// RetroArch is exec'd DIRECTLY from here — there is no generated bash
// launcher any more. Everything that script did is either done below in C++
// or was deleted on purpose; the full inventory (2026-10-03):
//
//   PORTED: mkdir of ~/.config/retroarch, the save/state dirs and
//     /tmp/empty_autoconfig; the core options, override and main config
//     files (byte-for-byte the same content, minus two `echo ... >>` shell
//     lines that had been written INSIDE the main config's heredoc and so
//     landed in retroarch_mdb.cfg as junk); the stale-.opt purge; the
//     environment (`unset DISPLAY WAYLAND_DISPLAY XDG_SESSION_TYPE
//     SDL_VIDEODRIVER`, `export HOME`, `export XDG_RUNTIME_DIR`) — see
//     build_child_environment(); stdout/stderr to the rotated launcher log;
//     one `udevadm settle` (RetroArch's udev joypad driver enumerates by the
//     udev DB's ID_INPUT_JOYSTICK, so the DB must be quiet after the
//     controller's `udevadm trigger`); the KMS readiness watch (now an
//     in-process /proc/<pid>/fd scan) and its /tmp/retroarch_mdb.ready
//     marker; the TERM-trap/wait dance (now stop_game_session()).
//
//   DELETED, with reasons:
//     - Autoconfig-file creation/backup-restore for 0e6d_111d / 0079_0006:
//       the script created the file and then `rm -f`'d it a few lines later,
//       and RetroArch is pointed at an empty autoconfig dir anyway — net
//       effect nothing, plus a "WARNING - Autoconfig file missing!" on every
//       launch.
//     - The controller "wake-up": `hexdump` of a hardcoded /dev/input/event0
//       (not the pad on a Pi) and js0 with `WAKE_PID=$!`, which never
//       captured a PID because the reader ran in a subshell; RetroArch opens
//       the pad itself, which is what resumes a USB HID device. ~1.7 s of
//       sleeps, all of it AFTER the display was handed over (a frozen frame).
//     - The script's two extra `sudo udevadm trigger` passes: the controller
//       already triggers js*/event* right before this (load_playlist_item).
//     - Diagnostics nobody read: `fuser -v /dev/input/event0` (wrong
//       device), `aplay -l`, `whoami`/`groups` (single-quoted, so they
//       logged the literal text `$(whoami)`), the js* permission listing
//       (the C++ accessibility check below still logs to the journal).
//     - Every `>> /tmp/retroarch_launcher.log` line: that tmpfs file grew
//       on every launch and was never rotated. A session header now opens
//       the rotated ~/retroarch_launcher.log instead.
//     - Deleting retroarch_mdb.cfg / retroarch_core_options.cfg on exit:
//       they are overwritten on every launch, so keeping the last one costs
//       nothing and lets a post-mortem see exactly what RetroArch was given.
//     - The find of *.backup.* autoconfig files older than a day: no code
//       has created one since the backup scheme was retired.
bool RetroArchLauncher::launch_drm(const GameLaunchInfo& game_info, int system_volume_percent, float volume_offset_db, int audio_output, const LaunchOptions& opts) {
    std::cout << "=== RetroArch Launcher Called ===" << std::endl;
    std::cout << "ROM: " << game_info.rom_path << std::endl;
    std::cout << "Core: " << game_info.core_name << std::endl;
    std::cout << "Overlay: " << game_info.overlay_path << std::endl;
    std::cout << "Display mode: " << (opts.display_mode == app::DisplayMode::MODERN_TV ? "MODERN_TV" : "CRT_NATIVE") << std::endl;
    std::cout << "Bezel file: " << (opts.bezel_file.empty() ? "(none)" : opts.bezel_file) << std::endl;
    std::cout << "Launching RetroArch in DRM/KMS mode" << std::endl;

    // RetroArch expects the full core name with _libretro suffix for -L
    const std::string core_name = libretro_core_name(game_info.core_name);

    // Resolve the core BEFORE touching anything. The controller already
    // refuses a missing core before it stops video / releases DRM
    // (find_core_dir); this is the backstop for any other caller. It used
    // to log "defaulting to system" and launch anyway, so a missing core
    // surfaced only as a dead RetroArch after the display handoff.
    const auto found_dir = find_core_dir(core_name);
    if (!found_dir) {
        std::cerr << "Core not installed: " << core_name << ".so (searched "
                  << kSystemLibretroDir << " and "
                  << config::retroarch::get_cores_dir() << ")" << std::endl;
        return false;
    }
    const std::string libretro_dir = *found_dir;
    std::cout << "Found core: " << libretro_dir << "/" << core_name << ".so"
              << std::endl;

    // A shutdown that landed during the controller's teardown: do not even
    // start. (Sticky flag — see request_session_stop().)
    if (session_stop_requested()) {
        std::cout << "Kiosk is shutting down; not launching RetroArch" << std::endl;
        return false;
    }

    // One session per log file: move last session's to .1 so the full
    // --verbose output cannot grow ~/retroarch_launcher.log forever.
    const std::string launcher_log = config::retroarch::get_launcher_log();
    if (!rotate_launcher_log(launcher_log)) {
        std::cerr << "Could not rotate launcher log (appending)" << std::endl;
    }
    {
        // Leftovers of the bash launcher: the unrotated tmpfs log, and the
        // last generated script (stale, and misleading to anyone reading it
        // as "what the kiosk runs").
        std::error_code ec;
        fs::remove(kLegacyTmpLauncherLog, ec);
        fs::remove(config::get_home_path() + "/retroarch_launcher.sh", ec);
    }

    // Stop GStreamer and cleanup audio resources first
    stop_gstreamer_and_cleanup();

    std::vector<std::string> cmd = {
        "retroarch",
        "--config", "/tmp/retroarch_mdb.cfg",
        // --appendconfig is applied AFTER the main config, so anything in
        // here always wins. Needed because RetroArch can end up with its
        // own defaults in the base file, and it takes the LAST value for
        // a duplicated key — that silently clobbered our
        // input_exit_emulator="z" with its default "escape", which is why
        // the phone remote's QUIT_GAME chord did nothing in-game
        // (verified on hardware 2026-07-26).
        "--appendconfig", "/tmp/retroarch_mdb_override.cfg",
    };
    const auto input_device_args = core_input_device_args(core_name);
    cmd.insert(cmd.end(), input_device_args.begin(), input_device_args.end());
    cmd.push_back("-L");
    cmd.push_back(core_name);
    cmd.push_back(game_info.rom_path);
    cmd.push_back("--verbose");

    // Select ALSA device based on user's audio output preference
    // audio_output: 0=AUTO, 1=HDMI, 2=HEADPHONE
    std::string alsa_device;
    if (audio_output == 2) {
        // User selected headphone output
        alsa_device = "sysdefault:CARD=Headphones";
        std::cout << "Using headphone ALSA device: " << alsa_device << std::endl;
    } else {
        // AUTO or HDMI: detect HDMI device (existing behavior)
        alsa_device = detect_alsa_device();
    }

    // Directories RetroArch writes into. Saves/states come from
    // config::retroarch::*() and may contain spaces (MAGIC_DATA_DIR under
    // "magic_dingus_box /"), which is why this is no longer a shell line.
    {
        std::error_code ec;
        fs::create_directories(config::get_home_path() + "/.config/retroarch", ec);
        fs::create_directories("/tmp/empty_autoconfig", ec);
        fs::create_directories(config::retroarch::get_saves_dir(), ec);
        if (ec) std::cerr << "Could not create saves dir: " << ec.message() << std::endl;
        ec.clear();
        fs::create_directories(config::retroarch::get_states_dir(), ec);
        if (ec) std::cerr << "Could not create states dir: " << ec.message() << std::endl;
    }

    // Controller type of the first recognized pad: the fallback mapping
    // when no pad is enumerated at all (resolve_port_mappings()).
    ControllerType controller_type = detect_primary_controller();
    std::cout << "Controller detected: " << controller_type_name(controller_type) << std::endl;

    // Core options, performance-tuned per core/title.
    {
        std::ostringstream core_opts;
        write_core_options(core_opts, core_name, game_info.rom_path);
        if (!write_file("/tmp/retroarch_core_options.cfg", core_opts.str())) {
            return false;
        }
    }
    remove_shadowing_opt_files(config::get_home_path(),
                               core_options_key_prefix(core_name));

    // Override file, applied via --appendconfig AFTER the main config so
    // these can never be clobbered by RetroArch's own defaults (see the
    // --appendconfig note where cmd is built).
    {
        std::ostringstream override_cfg;
        retroarch::write_remote_quit_config(override_cfg);
        retroarch::write_menu_disabled_config(override_cfg);
        if (!write_file("/tmp/retroarch_mdb_override.cfg", override_cfg.str())) {
            return false;
        }
    }

    // The FULL config, written to our ISOLATED config location.
    std::ostringstream cfg;
    cfg << "# DRM/KMS RetroArch config for Magic Dingus Box (Isolated)\n";
    cfg << "libretro_system_directory = \"" << config::retroarch::get_system_dir() << "\"\n";
    
    // Save/State directories for game progress persistence
    cfg << "savefile_directory = \"" << config::retroarch::get_saves_dir() << "\"\n";
    cfg << "savestate_directory = \"" << config::retroarch::get_states_dir() << "\"\n";
    cfg << "sort_savefiles_by_content_enable = \"true\"\n";
    cfg << "sort_savestates_by_content_enable = \"true\"\n";
    cfg << "savestate_auto_save = \"true\"\n";
    cfg << "savestate_auto_load = \"true\"\n";
    // Without these RA's auto-save state silently no-ops: with no global retroarch.cfg the default libretro_info_path points to a non-existent dir, core_info_list ends up empty, savestate_support_level reads 0, and command_event_save_auto_state early-returns at the support check.
    cfg << "libretro_info_path = \"/usr/share/libretro/info\"\n";
    cfg << "core_info_savestate_bypass = \"true\"\n";
    
    // Video config (driver, resolution, viewport, sync).
    // Pick the renderer from the core: N64 (GLideN64) needs the GL
    // path, everything else stays on Vulkan/khr_display. opts is
    // const&, so copy it to stamp the renderer.
    {
        LaunchOptions video_opts = opts;
        video_opts.renderer = renderer_for_core(core_name);
        // Size the viewport to the shape the core will actually hand
        // over. Only N64 differs from 4:3, and only for titles whose
        // measured overscan crop is lopsided — see n64_content_aspect.
        // Renderer::GL is precisely the N64 cores (renderer_for_core
        // returns it for mupen64plus/parallel_n64 and nothing else),
        // so it doubles as the family check without exporting one.
        if (video_opts.renderer == Renderer::GL) {
            video_opts.content_aspect =
                n64_content_aspect(core_name, game_info.rom_path);
        }
        write_video_config(cfg, video_opts);
    }
    // RetroArch's threaded ALSA wrapper keeps the HDMI device fed
    // independently of brief emulation or Vulkan present stalls.
    cfg << "audio_driver = \"" << audio_driver_for_gameplay()
                << "\"\n";
    cfg << "audio_resampler = \"sinc\"\n"; // High-quality gameplay resampling

    cfg << "input_joypad_driver = \"udev\"\n";
    cfg << "input_max_users = \"4\"\n";
    cfg << "# Enhanced controller detection and configuration\n";
    cfg << "# CRITICAL: Enable autodetect so RetroArch detects the controller\n";
    cfg << "# But disable autoconfig so it doesn't load autoconfig files\n";
    cfg << "input_autodetect_enable = \"true\"\n";
    cfg << "# CRITICAL: Disable remap binds since autoconfig is disabled\n";
    cfg << "input_remap_binds_enable = \"true\"\n";  // CRITICAL: Enable so core can receive input
    cfg << "input_player1_analog_dpad_mode = \"0\"\n";  // Digital only for NES (matches working test)
    cfg << "# CRITICAL: Force RetroArch to use built-in default button mappings (auto-assignment)\n";
    cfg << "input_player1_bind_defaults = \"false\"\n";
    cfg << "# CRITICAL: This forces RetroArch to automatically assign standard button mappings\n";
    cfg << "# RetroArch will map: A=0, B=1, X=2, Y=3, L=4, R=5, Start=6, Select=7, D-pad=hat0\n";
    cfg << "# CRITICAL: Ensure player 1 controller is enabled and working\n";
    cfg << "input_player1_joypad_index = \"0\"\n";
    cfg << "input_player1_enable = \"true\"\n";
    // Player 2 setup mirrors player 1 — same analog mode + bind_defaults
    // policy, but tied to joypad index 1. Without these explicit lines
    // RetroArch leaves player 2 unbound (no autoconfig is present;
    // we deleted that file to force manual mappings) and a 2nd
    // identical PS-pad shows up in /dev/input/js1 but generates no
    // input events the core can see. Per-core button mappings for
    // player 2 are emitted alongside player 1 below.
    cfg << "input_player2_analog_dpad_mode = \"0\"\n";
    cfg << "input_player2_bind_defaults = \"false\"\n";
    cfg << "input_player2_joypad_index = \"1\"\n";
    cfg << "input_player2_enable = \"true\"\n";
    cfg << "# Default mappings removed to prevent conflict with core-specific overrides\n";
    cfg << "# We rely on core-specific sections to define mappings\n";
    cfg << "# For NES: A=0 (jump), B=1 (run), Start=2, Select=10, D-pad=hat0\n";
    cfg << "input_enable_hotkey = \"true\"\n";
    // Menu chord/key/updater disabled in the override file above
    // (write_menu_disabled_config) -- the kiosk ships no RA menu.
    cfg << "input_auto_game_focus = \"true\"\n";
    cfg << "input_game_focus_enable = \"true\"\n";
    cfg << "input_logging_enable = \"false\"\n";
    cfg << "input_logging_level = \"0\"\n";
    cfg << "input_block_timeout = \"0\"\n";
    cfg << "input_hotkey_block_delay = \"0\"\n";
    cfg << "# CRITICAL: Ensure input is enabled and controller works in-game\n";
    cfg << "input_enabled = \"true\"\n";
    cfg << "input_driver = \"udev\"\n";
    cfg << "input_poll_type_behavior = \"0\"\n";
    cfg << "input_all_users_control_menu = \"true\"\n";
    cfg << "# CRITICAL: Ensure controller input reaches the core\n";
    cfg << "input_descriptor_label_show = \"true\"\n";  // Show descriptors (matches working test)
    cfg << "input_descriptor_hide_unbound = \"false\"\n";
    cfg << "# CRITICAL: Enable autoconfig to load button mappings from autoconfig file\n";
    cfg << "input_autoconfig_enable = \"false\"\n";
    cfg << "input_joypad_driver_autoconfig_dir = \"/tmp/empty_autoconfig\"\n"; // Hide autoconfig files
    cfg << "# CRITICAL: Ensure joypad driver is set (required for controller detection)\n";
    cfg << "input_joypad_driver = \"udev\"\n";
    cfg << "# CRITICAL: Force RetroArch to auto-assign default button mappings if autoconfig fails\n";
    cfg << "# When bind_defaults=true, RetroArch will automatically assign standard button mappings\n";
    cfg << "# This ensures buttons work even if autoconfig doesn't match perfectly\n";
    cfg << "input_joypad_driver_mapping_dir = \"\"\n";
    cfg << "# Don't save config on exit (prevents overwriting our settings)\n";
    cfg << "config_save_on_exit = \"false\"\n";
    cfg << "# CRITICAL: Single press to quit (don't require double press)\n";
    cfg << "quit_press_twice = \"false\"\n";
    {
        // Phone remote QUIT_GAME support (KEY_Z exit bind) —
        // see retroarch::write_remote_quit_config().
        std::ostringstream remote_quit;
        retroarch::write_remote_quit_config(remote_quit);
        retroarch::write_menu_disabled_config(remote_quit);
        cfg << remote_quit.str();
    }
    cfg << "core_options_path = \"/tmp/retroarch_core_options.cfg\"\n";
    cfg << "# Audio settings - use ALSA to match GStreamer (simplified for reliability)\n";
    cfg << "audio_device = \"" << alsa_device << "\"\n";
    cfg << "audio_enable = \"true\"\n";
    cfg << "audio_mute_enable = \"false\"\n";
    // Convert system volume (0-100) to RetroArch dB format
    // RetroArch uses decibels: 0 dB = 100%, negative dB = quieter
    // Formula: dB = 20 * log10(volume_percent / 100)
    // For safety, clamp to reasonable range: -60 dB to 0 dB
    float volume_decimal = system_volume_percent / 100.0f;
    float volume_db = (volume_decimal > 0.001f) ? (20.0f * log10f(volume_decimal)) : -60.0f;
    // Clamp to valid range
    if (volume_db > 0.0f) volume_db = 0.0f;
    if (volume_db < -60.0f) volume_db = -60.0f;
    // Apply user's game volume offset (e.g., -3dB, -6dB, -12dB)
    float final_volume_db = volume_db + volume_offset_db;
    if (final_volume_db < -60.0f) final_volume_db = -60.0f;
    cfg << "audio_volume = \"" << final_volume_db << "\"\n";
    cfg << "audio_mixer_volume = \"1.0\"\n";
    cfg << "audio_mixer_mute_enable = \"false\"\n";
    cfg << "# Simplified audio settings (matches Pi game version)\n";
    cfg << "audio_sync = \"true\"\n";
//             cfg << "audio_resampler = \"sinc\"\n";
    cfg << "audio_out_rate = \"48000\"\n";
    cfg << "audio_latency = \""
                << audio_latency_ms_for_core(core_name) << "\"\n";
    cfg << "# Audio buffer settings - ensure audio callback works\n";
//             cfg << "audio_block_frames = \"512\"\n";
//             cfg << "audio_rate_control = \"true\"\n";
//             cfg << "audio_rate_control_delta = \"0.005000\"\n";
    cfg << "audio_enable_menu = \"false\"\n";
    cfg << "audio_fastforward_mute = \"false\"\n";
    cfg << "audio_dsp_plugin = \"\"\n";
    cfg << "input_keyboard_layout = \"us\"\n";
    cfg << "libretro_directory = \"" << libretro_dir << "\"\n";
    cfg << "core_updater_buildbot_cores_url = \"https://buildbot.libretro.com/nightly/linux/aarch64/latest\"\n";
    cfg << "core_updater_buildbot_assets_url = \"https://buildbot.libretro.com/assets/\"\n";
    cfg << "core_updater_auto_extract_archive = \"true\"\n";
    cfg << "# Ensure core actually runs\n";
    cfg << "rewind_enable = \"false\"\n";
    cfg << "run_ahead_enabled = \"false\"\n";
    cfg << "netplay_enable = \"false\"\n";
    cfg << "# CRITICAL: Ensure content actually loads and runs\n";
    cfg << "content_load_auto_remap = \"false\"\n";
    cfg << "content_load_mode_manual = \"false\"\n";
    cfg << "pause_nonactive = \"false\"\n";
    

    // 2. Resolve each port's mapping independently: captured profile
    // -> builtin -> legacy N64 fallback (resolve_mapping_for_pad's
    // precedence, via resolve_port_mappings() in
    // controller_mapping.cpp). A missing P2 pad mirrors P1 exactly,
    // and no detected pads at all falls back to today's single
    // get_mapping(controller_type, core_name) path unchanged --
    // see resolve_port_mappings() for the exact preserved-behavior
    // contract (also unit-tested on Mac against a synthetic pad
    // list, since this /dev/input scan itself has no Mac build).
    const auto pads = detect_connected_controllers();
    const auto profile_store = load_profile_store();
    const auto port_mappings =
        resolve_port_mappings(pads, controller_type, profile_store, core_name);
    ControllerMapping map = port_mappings.p1;
    ControllerMapping map_p2 = port_mappings.p2;

    cfg << "# === Controller Mapping: " << map.name << " ===\n";

    // 2-5. Apply the full input_player1_* bind block (settings,
    // buttons, d-pad, analog axes, right stick, d-pad axes). See
    // write_player_binds() in controller_mapping.cpp for the exact
    // field order and the unconditional-emission rule (empty
    // in-memory button tokens still write a line, serialized as
    // `= "nul"` rather than omitted).
    write_player_binds(cfg, map, 1);

    // 5b. Player 2's own mapping, resolved above from port 1's
    // VID/PID (or mirrored from player 1 when no second pad is
    // connected -- see resolve_port_mappings()). Historically P2
    // always mirrored P1 outright, because every fielded box ships
    // two identical pads; without SOME P2 emission at all,
    // RetroArch's per-core remap covers only player 1 and the 2nd
    // pad shows up in /dev/input/js1 but produces no in-game effect
    // — symptom: P2 character sits motionless in 2-player Twisted
    // Metal / Tony Hawk / Doom split-screen. Task 7 lets each port
    // resolve independently instead, so two DIFFERENT controller
    // models can each get their own correct mapping in the same
    // two-player game, while the one-pad-detected case still
    // produces an exact mirror (verified above).
    //
    // Both calls MUST go through write_player_binds() — the P2
    // block used to be a hand-duplicated copy of the P1 block, and
    // that copy once drifted out of sync and shipped without the
    // right-stick lines, leaving P2 with no camera control in every
    // two-player N64 game. Routing both players through the same
    // function makes that class of drift structurally impossible.
    //
    // Hotkeys (below) intentionally stay player-1-only so both
    // controllers don't fight over the RA menu toggle.
    write_player_binds(cfg, map_p2, 2);

    // 5c. Apply Hotkeys
    write_hotkey_binds(cfg, map);

    // 8. Apply Extra Config (if any)
    if (!map.extra_config.empty()) {
        cfg << map.extra_config;
    }
    cfg << "# CRITICAL: Ensure input reaches the core (not just RetroArch menu)\n";
    cfg << "input_driver_block_input = \"false\"\n";  // Don't block input
    cfg << "input_driver_block_libretro_input = \"false\"\n";  // Don't block libretro input
    cfg << "# Controller auto-configuration enabled - configure when game launches\n";
    cfg << "\n";
    if (!write_file("/tmp/retroarch_mdb.cfg", cfg.str())) {
        return false;
    }

    // Session header for the rotated launcher log; RetroArch's own
    // --verbose output is appended below it by the child.
    {
        std::ofstream log(launcher_log, std::ios::app);
        log << "=== " << timestamp_now() << " Magic Dingus Box game session ===\n"
            << "ROM: " << game_info.rom_path << "\n"
            << "Core: " << core_name << " (" << libretro_dir << ")\n"
            << "ALSA device: " << alsa_device << "\n"
            << "Mapping P1: " << map.name << "\n"
            << "Mapping P2: " << map_p2.name << "\n";
        for (const auto& pad : pads) {
            char vidpid[16];
            std::snprintf(vidpid, sizeof(vidpid), "%04x:%04x", pad.vid, pad.pid);
            log << "Pad port " << pad.port << ": " << vidpid << " " << pad.name << "\n";
        }
        log << "Command:";
        for (const auto& arg : cmd) log << " '" << arg << "'";
        log << "\n";
    }

    // RetroArch's udev joypad driver builds its pad list from the udev DB
    // (ID_INPUT_JOYSTICK). The controller's `udevadm trigger` just queued a
    // change event for every input device; let udev finish them so the DB
    // is not mid-rewrite when RetroArch enumerates. Bounded at 2 s. Done
    // BEFORE the display handoff, so it costs no frozen-frame time.
    run_quiet({"udevadm", "settle", "--timeout=2"});

    // Verify controller device is accessible before forking (journal only).
    bool controller_accessible = false;
    for (int i = 0; i < 4; ++i) {
        std::string js_path = "/dev/input/js" + std::to_string(i);
        if (access(js_path.c_str(), R_OK) == 0) {
            std::cout << "Controller device accessible: " << js_path << std::endl;
            controller_accessible = true;
            break;
        }
    }
    if (!controller_accessible) {
        std::cerr << "WARNING: No accessible controller devices found before RetroArch launch!" << std::endl;
        std::cerr << "This may cause controller input to not work in RetroArch" << std::endl;
    }

    // Everything the child needs is computed BEFORE the display handoff.
    SpawnSpec spec;
    spec.executable = retroarch_bin_.value();
    spec.argv = cmd;
    spec.envp = build_child_environment(current_environment(),
                                        config::get_home_path(), getuid());
    spec.log_path = launcher_log;

    // A stale marker must never make a new launch look ready to the smoke
    // test.
    {
        std::error_code ec;
        fs::remove(kReadyMarkerPath, ec);
    }

    if (session_stop_requested()) {
        std::cout << "Kiosk is shutting down; not launching RetroArch" << std::endl;
        return false;
    }

    // LAST POSSIBLE MOMENT to hand over the display. The configs are
    // written; everything above needed no display at all, so the kiosk has
    // been able to keep its launch screen animating through all of it. The
    // caller uses this hook to present its final frame and release DRM
    // master, and the fork happens immediately after.
    if (opts.before_fork) {
        opts.before_fork();
    }
    // A SIGTERM during the handoff: DRM is already released, so return
    // "not launched" and let the controller's normal restore re-acquire it.
    if (session_stop_requested()) {
        std::cout << "Kiosk is shutting down; launch aborted after DRM handoff" << std::endl;
        return false;
    }

    std::cout << "Launching RetroArch directly: " << spec.executable << std::endl;
    std::string spawn_error;
    const pid_t pid = spawn_session(spec, &spawn_error);
    if (pid <= 0) {
        std::cerr << "Failed to start RetroArch: " << spawn_error << std::endl;
        return false;
    }

    // Supervise instead of blocking in waitpid(): the main thread keeps
    // feeding the systemd watchdog and notices a shutdown request within one
    // poll, so the watchdog no longer has to be switched off for the game.
    auto watchdog = [&opts](SessionWatchdog ev) {
        if (opts.watchdog) opts.watchdog(ev);
    };
    watchdog(SessionWatchdog::Arm);
    std::cout << "RetroArch started (PID " << pid
              << "), waiting up to 15 seconds for KMS" << std::endl;
    SessionOps ops = real_session_ops(pid, kReadyMarkerPath, [&watchdog] {
        watchdog(SessionWatchdog::Ping);
    });
    auto mark_ready = ops.on_ready;
    ops.on_ready = [mark_ready] {
        std::cout << "RetroArch has taken over the KMS display" << std::endl;
        if (mark_ready) mark_ready();
    };
    const SessionReport report = supervise_session(ops, SupervisePolicy{});
    // The DRM/input restore that follows stays unwatched, as it always was;
    // the controller's session-end hook re-arms the watchdog after it.
    watchdog(SessionWatchdog::Disarm);

    const int status = reap_session(pid);
    std::error_code ec;
    fs::remove(kReadyMarkerPath, ec);

    std::cout << "RetroArch session: " << outcome_name(report.outcome)
              << " (" << stop_name(report.stop) << ")" << std::endl;
    if (status < 0) {
        std::cerr << "Failed to reap RetroArch: " << std::strerror(errno) << std::endl;
    } else if (WIFEXITED(status)) {
        std::cout << "RetroArch exited with status " << WEXITSTATUS(status) << std::endl;
    } else if (WIFSIGNALED(status)) {
        std::cout << "RetroArch killed by signal " << WTERMSIG(status) << std::endl;
    }

    // true = a game actually ran (the kiosk shows no launch error).
    return report.ready;
}

void RetroArchLauncher::release_controllers() {
    std::cout << "Releasing controller devices before RetroArch launch" << std::endl;
    
    // Iterate through joystick devices
    for (int i = 0; i < 4; ++i) {
        std::string js_path = "/dev/input/js" + std::to_string(i);
        
        // Check if device exists and is readable
        if (access(js_path.c_str(), R_OK) == 0) {
            std::cout << "Releasing controller device: " << js_path << std::endl;
            
            // Trigger udev to reset the device
            std::string udev_cmd = "udevadm trigger --action=change --sysname-match=js" + std::to_string(i);
            int result = std::system(udev_cmd.c_str());
            if (result != 0) {
                std::cerr << "Warning: Failed to trigger udev for " << js_path << std::endl;
            }
        }
    }
    
    // Small delay for devices to settle
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

std::string RetroArchLauncher::detect_alsa_device() {
    // Select the HDMI audio device by PCM NAME via the shared contract
    // helper (see retroarch::pick_hdmi_alsa_device) — never by card
    // number, which differs between Pi 4 and Pi 5.
    std::string output_l;
    FILE* pipe_l = popen("aplay -L 2>&1", "r");
    if (pipe_l) {
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe_l) != nullptr) {
            output_l += buffer;
        }
        pclose(pipe_l);
    } else {
        std::cerr << "Warning: Failed to execute aplay -L, using legacy default" << std::endl;
    }
    // Which port has the TV: either HDMI port is a valid place to plug it.
    std::vector<std::string> monitor_cards;
    for (const char* card : {"vc4hdmi0", "vc4hdmi1"}) {
        std::ifstream eld(std::string("/proc/asound/") + card + "/eld#0");
        if (!eld) continue;
        std::stringstream text;
        text << eld.rdbuf();
        if (retroarch::eld_reports_monitor(text.str())) monitor_cards.push_back(card);
    }
    std::string device = retroarch::pick_hdmi_alsa_device(output_l, monitor_cards);
    std::cout << "Detected HDMI ALSA device: " << device << std::endl;
    return device;
}

void RetroArchLauncher::stop_gstreamer_and_cleanup() {
    std::cout << "Stopping GStreamer and cleaning up audio resources..." << std::endl;
    
    // Kill GStreamer processes that are children of our app (avoid killing unrelated processes)
    std::cout << "Killing GStreamer child processes..." << std::endl;
    std::string our_pid = std::to_string(getpid());
    auto run_pkill = [](const char* const args[]) {
        pid_t pid = fork();
        if (pid == 0) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
            execvp(args[0], const_cast<char* const*>(args));
            _exit(127);
        }
        if (pid > 0) { int s; waitpid(pid, &s, 0); }
    };
    const char* kill_gst1[] = {"pkill", "-9", "-P", our_pid.c_str(), "-f", "gst", nullptr};
    const char* kill_gst2[] = {"pkill", "-9", "gst-launch-1.0", nullptr};
    run_pkill(kill_gst1);
    run_pkill(kill_gst2);

    // Wait for processes to exit
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ALSA settle wait. This used to be gated on `popen("lsof | grep snd")`
    // — a scan of EVERY process's fd table (100ms-1s+ on this Pi with the
    // Docker stack up) whose outcome was constant: PulseAudio always holds
    // /dev/snd devices open by design on this system, so "device busy" was
    // always true and the 300ms wait always fired anyway. Keeping the wait
    // unconditional preserves the exact effective timing while dropping the
    // scan cost from every game launch.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::cout << "GStreamer cleanup complete" << std::endl;
}

} // namespace retroarch
