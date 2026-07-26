#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "retroarch/launch_contract.h"

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

void require_line(const std::string& config, const std::string& line) {
    REQUIRE(config.find(line + "\n") != std::string::npos);
}

std::string temp_path(const char* leaf) {
    return (fs::temp_directory_path() /
            (std::string("mdb-ra-") + std::to_string(getpid()) + "-" + leaf))
        .string();
}

pid_t spawn_group(const std::string& command) {
    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        setpgid(0, 0);
        execl("/bin/bash", "bash", "-c", command.c_str(), nullptr);
        _exit(127);
    }
    setpgid(pid, pid);
    return pid;
}

}  // namespace

TEST_CASE("Modern TV keeps its native-core 4:3 bezel contract",
          "[retroarch][video]") {
    retroarch::LaunchOptions options;
    options.display_mode = app::DisplayMode::MODERN_TV;
    options.bezel_file = "mdb_kv19.png";

    std::ostringstream output;
    retroarch::write_video_config(output, options);
    const std::string config = output.str();

    require_line(config, "video_driver = \"vulkan\"");
    require_line(config, "video_context_driver = \"khr_display\"");
    require_line(config, "video_threaded = \"false\"");
    require_line(config, "video_max_swapchain_images = \"2\"");
    require_line(config, "video_vsync = \"true\"");
    require_line(config, "video_frame_delay = \"4\"");
    require_line(config, "video_frame_delay_auto = \"true\"");
    require_line(config, "video_shader_enable = \"false\"");
    require_line(config, "video_smooth = \"false\"");
    require_line(config, "video_fullscreen_x = \"1920\"");
    require_line(config, "video_fullscreen_y = \"1080\"");
    require_line(config, "video_custom_viewport_enable = \"true\"");
    require_line(config, "video_custom_viewport_x = \"251\"");
    require_line(config, "video_custom_viewport_y = \"10\"");
    require_line(config, "video_custom_viewport_width = \"1415\"");
    require_line(config, "video_custom_viewport_height = \"1059\"");
    require_line(config, "aspect_ratio_index = \"22\"");
    require_line(config, "input_overlay_enable = \"true\"");
    require_line(config, "input_overlay_opacity = \"1.0\"");
    require_line(config, "input_overlay_hide_in_menu = \"true\"");
}

TEST_CASE("CRT Native remains 640x480 without a custom viewport",
          "[retroarch][video]") {
    retroarch::LaunchOptions options;
    options.display_mode = app::DisplayMode::CRT_NATIVE;
    options.bezel_file = "mdb_kv19.png";

    std::ostringstream output;
    retroarch::write_video_config(output, options);
    const std::string config = output.str();

    require_line(config, "video_context_driver = \"khr_display\"");
    require_line(config, "video_frame_delay_auto = \"true\"");
    require_line(config, "video_fullscreen_x = \"640\"");
    require_line(config, "video_fullscreen_y = \"480\"");
    require_line(config, "video_custom_viewport_enable = \"false\"");
    require_line(config, "aspect_ratio_index = \"23\"");
    REQUIRE(config.find("input_overlay =") == std::string::npos);
}

TEST_CASE("PS1 core disables frame skipping and preserves native performance options",
          "[retroarch][core-options][ps1]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "pcsx_rearmed_libretro",
                                  "data/roms/ps1/Castlevania: Symphony of "
                                  "the Night (USA).chd");
    const std::string config = output.str();

    require_line(config, "pcsx_rearmed_pad1type = \"analog\"");
    require_line(config, "pcsx_rearmed_spu_thread = \"enabled\"");
    require_line(config, "pcsx_rearmed_nocdaudio = \"disabled\"");
    require_line(config, "pcsx_rearmed_noxadecoding = \"disabled\"");
    require_line(config, "pcsx_rearmed_frameskip_type = \"disabled\"");
    require_line(config, "pcsx_rearmed_gpu_slow_llists = \"disabled\"");
    require_line(config, "pcsx_rearmed_drc = \"enabled\"");
    require_line(config, "pcsx_rearmed_icache_emulation = \"enabled\"");
    require_line(config, "pcsx_rearmed_psxclock = \"57\"");
    require_line(config, "pcsx_rearmed_spu_interpolation = \"off\"");
    require_line(config, "pcsx_rearmed_spu_reverb = \"disabled\"");
    require_line(config,
                 "pcsx_rearmed_neon_enhancement_enable = \"disabled\"");
    require_line(config, "pcsx_rearmed_dithering = \"enabled\"");
    REQUIRE(config.find("pcsx_rearmed_frameskip_threshold") ==
            std::string::npos);
    REQUIRE(config.find("pcsx_rearmed_frameskip_interval") ==
            std::string::npos);
    REQUIRE(config.find("auto_threshold") == std::string::npos);
    // This core build has no THREAD_RENDERING support — emitting the
    // option would be an inert no-op, so we must not.
    REQUIRE(config.find("gpu_thread_rendering") == std::string::npos);
    // Default titles run at the native PSX clock with stall emulation.
    REQUIRE(config.find("pcsx_rearmed_nostalls") == std::string::npos);
}

TEST_CASE("THPS4 gets the heavy-title PS1 overclock override",
          "[retroarch][core-options][ps1]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "pcsx_rearmed_libretro",
                                  "data/roms/ps1/Tony Hawk's Pro Skater 4 "
                                  "(USA).chd");
    const std::string config = output.str();

    require_line(config, "pcsx_rearmed_psxclock = \"65\"");
    require_line(config, "pcsx_rearmed_nostalls = \"enabled\"");
    REQUIRE(config.find("pcsx_rearmed_psxclock = \"57\"") ==
            std::string::npos);
    // The rest of the PS1 contract is untouched by the override.
    require_line(config, "pcsx_rearmed_drc = \"enabled\"");
    require_line(config, "pcsx_rearmed_frameskip_type = \"disabled\"");
    require_line(config, "pcsx_rearmed_dithering = \"enabled\"");
}

TEST_CASE("THPS4 override matches case-insensitively",
          "[retroarch][core-options][ps1]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "pcsx_rearmed_libretro",
                                  "/roms/TONY HAWK'S PRO SKATER 4.chd");
    require_line(output.str(), "pcsx_rearmed_psxclock = \"65\"");
}

TEST_CASE("other Tony Hawk titles keep the native clock",
          "[retroarch][core-options][ps1]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "pcsx_rearmed_libretro",
                                  "data/roms/ps1/Tony Hawk's Pro Skater 2 "
                                  "(USA).chd");
    const std::string config = output.str();
    require_line(config, "pcsx_rearmed_psxclock = \"57\"");
    REQUIRE(config.find("pcsx_rearmed_nostalls") == std::string::npos);
}

TEST_CASE("non-PS1 core emits no PCSX-ReARMed options",
          "[retroarch][core-options]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "nestopia_libretro",
                                  "data/roms/nes/Bubble Bobble (USA).zip");
    REQUIRE(output.str().empty());
}

TEST_CASE("PS1 cores use underrun-safe audio latency",
          "[retroarch][audio]") {
    REQUIRE(retroarch::audio_latency_ms_for_core("pcsx_rearmed_libretro") ==
            48);
    REQUIRE(retroarch::audio_latency_ms_for_core("beetle_psx_libretro") ==
            48);
    REQUIRE(retroarch::audio_latency_ms_for_core("swanstation_libretro") ==
            48);
}

TEST_CASE("non-PS1 cores keep low audio latency", "[retroarch][audio]") {
    REQUIRE(retroarch::audio_latency_ms_for_core("nestopia_libretro") == 48);
    REQUIRE(retroarch::audio_latency_ms_for_core("snes9x2010_libretro") ==
            48);
}

TEST_CASE("gameplay uses RetroArch threaded ALSA", "[retroarch][audio]") {
    REQUIRE(std::string(retroarch::audio_driver_for_gameplay()) ==
            "alsathread");
}

TEST_CASE("KMS marker makes startup ready without waiting for game exit",
          "[retroarch][startup]") {
    const std::string marker = temp_path("ready");
    fs::remove(marker);
    const pid_t pid = spawn_group("sleep 0.10; printf '1234\\n' > '" + marker +
                                  "'; sleep 5");

    REQUIRE(retroarch::wait_for_startup(pid, marker, 2s, 20ms) ==
            retroarch::StartupStatus::Ready);
    REQUIRE(retroarch::terminate_process_group(pid, 500ms));
    fs::remove(marker);
}

TEST_CASE("child exit before KMS marker is a startup failure",
          "[retroarch][startup]") {
    const std::string marker = temp_path("early-ready");
    fs::remove(marker);
    const pid_t pid = spawn_group("exit 7");

    REQUIRE(retroarch::wait_for_startup(pid, marker, 2s, 20ms) ==
            retroarch::StartupStatus::Exited);
    fs::remove(marker);
}

TEST_CASE("startup timeout terminates the entire launch group",
          "[retroarch][startup]") {
    const std::string marker = temp_path("timeout-ready");
    fs::remove(marker);
    const pid_t pid = spawn_group("sleep 10");

    REQUIRE(retroarch::wait_for_startup(pid, marker, 200ms, 20ms) ==
            retroarch::StartupStatus::TimedOut);
    REQUIRE(retroarch::terminate_process_group(pid, 200ms));
    int status = 0;
    REQUIRE(waitpid(pid, &status, WNOHANG) == -1);
    fs::remove(marker);
}

TEST_CASE("generated watcher removes compositor hints and publishes real PID",
          "[retroarch][startup]") {
    retroarch::ReadyWatchOptions options;
    options.ready_file = "/tmp/mdb-ready";
    options.drm_card_pattern = "/dev/dri/card*";

    const std::string block = retroarch::build_kms_ready_watch_block(
        "/usr/bin/retroarch --verbose", options);

    REQUIRE(block.find(
                "unset DISPLAY WAYLAND_DISPLAY XDG_SESSION_TYPE SDL_VIDEODRIVER") !=
            std::string::npos);
    REQUIRE(block.find("/proc/$RETROARCH_PID/fd/*") != std::string::npos);
    REQUIRE(block.find("printf '%s\\n' \"$RETROARCH_PID\"") !=
            std::string::npos);
    REQUIRE(block.find("/dev/dri/card*") != std::string::npos);
}

TEST_CASE("generated KMS watcher is valid Bash", "[retroarch][startup]") {
    const std::string script_path = temp_path("watcher.sh");
    retroarch::ReadyWatchOptions options;
    options.ready_file = temp_path("watcher-ready");

    {
        std::ofstream script(script_path);
        REQUIRE(script.is_open());
        script << "#!/bin/bash\n";
        script << retroarch::build_kms_ready_watch_block(
            "/usr/bin/retroarch --verbose", options);
        script << "exit \"$RETROARCH_EXIT\"\n";
    }

    REQUIRE(std::system(("/bin/bash -n '" + script_path + "'").c_str()) == 0);
    fs::remove(script_path);
}

TEST_CASE("video contract is identical across Pi models until Pi 5 is benchmarked",
          "[retroarch][video][platform]") {
    // LaunchOptions carries the detected board so Pi-5-specific tuning has
    // a single place to land. The two Pi-4-empirical workarounds
    // (video_threaded=false, max_swapchain_images=2) were RE-BENCHMARKED
    // on Pi 5 / V3D 7.1 on 2026-07-25 and both were kept: threaded video
    // still produces a swapchain failure the non-threaded path doesn't
    // (1 vs 0), and swapchain depth made no measurable difference. See
    // write_video_config() for the full data. This test pins the parity
    // so any future divergence stays a conscious, measured decision.
    retroarch::LaunchOptions pi4_options;
    pi4_options.display_mode = app::DisplayMode::MODERN_TV;
    pi4_options.bezel_file = "mdb_kv19.png";
    pi4_options.pi_model = platform::PiModel::Pi4;

    retroarch::LaunchOptions pi5_options = pi4_options;
    pi5_options.pi_model = platform::PiModel::Pi5;

    std::ostringstream pi4_out, pi5_out;
    retroarch::write_video_config(pi4_out, pi4_options);
    retroarch::write_video_config(pi5_out, pi5_options);

    REQUIRE(pi4_out.str() == pi5_out.str());
    require_line(pi5_out.str(), "video_threaded = \"false\"");
    require_line(pi5_out.str(), "video_max_swapchain_images = \"2\"");
}

TEST_CASE("remote-quit bind lets the phone remote's KEY_Z chord exit the core",
          "[retroarch][input]") {
    // The phone remote's QUIT_GAME emits KEY_Z + BTN_START on the virtual
    // "MagicDingus Phone Remote" gamepad. RetroArch's udev keyboard path
    // reads that device's KEY_Z (verified live on the Pi 5 bench,
    // 2026-07-22), so binding the exit hotkey to "z" makes the remote able
    // to quit games — previously the chord was silently ignored because
    // the virtual pad has no manual joypad binds and autoconfig is off.
    std::ostringstream out;
    retroarch::write_remote_quit_config(out);
    const std::string config = out.str();
    require_line(config, "input_exit_emulator = \"z\"");
}

TEST_CASE("HDMI ALSA device picks vc4hdmi0 by NAME on both Pi 4 and Pi 5",
          "[retroarch][audio]") {
    // Pi 5 layout: vc4hdmi0 is ALSA card 0 (captured from the bench Pi,
    // 2026-07-22). The old detect_alsa_device() fell through to a
    // hardcoded "plughw:1,0" — correct on Pi 4 only by card-ordering
    // luck (its card 1 was vc4hdmi0, behind the Headphones card 0);
    // on Pi 5 that lands on vc4hdmi1, the EMPTY HDMI port → silent games.
    const char* pi5_aplay_L =
        "null\n    Discard all samples\n"
        "sysdefault\n    Default Audio Device\n"
        "hw:CARD=vc4hdmi0,DEV=0\n    vc4-hdmi-0, MAI PCM i2s-hifi-0\n"
        "plughw:CARD=vc4hdmi0,DEV=0\n    vc4-hdmi-0, MAI PCM i2s-hifi-0\n"
        "sysdefault:CARD=vc4hdmi0\n    vc4-hdmi-0, MAI PCM i2s-hifi-0\n"
        "hw:CARD=vc4hdmi1,DEV=0\n    vc4-hdmi-1, MAI PCM i2s-hifi-0\n"
        "sysdefault:CARD=vc4hdmi1\n    vc4-hdmi-1, MAI PCM i2s-hifi-0\n";
    REQUIRE(retroarch::pick_hdmi_alsa_device(pi5_aplay_L) == "sysdefault:CARD=vc4hdmi0");

    // Pi 4 layout: Headphones card first, then the HDMI cards.
    const char* pi4_aplay_L =
        "null\n    Discard all samples\n"
        "sysdefault:CARD=Headphones\n    bcm2835 Headphones, bcm2835 Headphones\n"
        "sysdefault:CARD=vc4hdmi0\n    vc4-hdmi-0, MAI PCM i2s-hifi-0\n"
        "sysdefault:CARD=vc4hdmi1\n    vc4-hdmi-1, MAI PCM i2s-hifi-0\n";
    REQUIRE(retroarch::pick_hdmi_alsa_device(pi4_aplay_L) == "sysdefault:CARD=vc4hdmi0");
}

TEST_CASE("HDMI ALSA device falls back to vc4hdmi1 then legacy default",
          "[retroarch][audio]") {
    REQUIRE(retroarch::pick_hdmi_alsa_device(
                "sysdefault:CARD=vc4hdmi1\n    vc4-hdmi-1\n") ==
            "sysdefault:CARD=vc4hdmi1");
    // No vc4 cards at all (dev box, USB-only audio): keep the legacy
    // fallback so behavior off-Pi is unchanged.
    REQUIRE(retroarch::pick_hdmi_alsa_device("null\n    Discard\n") == "plughw:1,0");
}

TEST_CASE("GL renderer path emits video_driver=gl without Vulkan-only settings",
          "[retroarch][video][gl]") {
    // N64 (GLideN64) and other GL-only cores can't use the kiosk's default
    // Vulkan/khr_display contract — emitting khr_display for a GL core
    // black-screens it (verified in the Pi 5 emulation research). When
    // LaunchOptions.renderer == GL, the video config must switch to
    // video_driver=gl, drop khr_display (leave context empty for KMS/EGL/
    // GBM auto-select), and NOT emit the Pi-4 Vulkan swapchain workarounds
    // (video_threaded=false / video_max_swapchain_images=2), which are
    // meaningless and can hurt on the GL path.
    retroarch::LaunchOptions gl_opts;
    gl_opts.display_mode = app::DisplayMode::MODERN_TV;
    gl_opts.renderer = retroarch::Renderer::GL;

    std::ostringstream out;
    retroarch::write_video_config(out, gl_opts);
    const std::string cfg = out.str();

    require_line(cfg, "video_driver = \"gl\"");
    REQUIRE(cfg.find("khr_display") == std::string::npos);
    REQUIRE(cfg.find("video_max_swapchain_images") == std::string::npos);
    REQUIRE(cfg.find("video_threaded = \"false\"") == std::string::npos);
    // Mode-specific viewport must still be emitted (unchanged by renderer).
    require_line(cfg, "video_fullscreen_x = \"1920\"");
}

TEST_CASE("default renderer stays Vulkan/khr_display with the swapchain workarounds",
          "[retroarch][video][vulkan]") {
    // Existing behavior must be untouched for the 2D/PS1-class cores.
    retroarch::LaunchOptions vk_opts;  // renderer defaults to Vulkan
    vk_opts.display_mode = app::DisplayMode::MODERN_TV;

    std::ostringstream out;
    retroarch::write_video_config(out, vk_opts);
    const std::string cfg = out.str();

    require_line(cfg, "video_driver = \"vulkan\"");
    require_line(cfg, "video_context_driver = \"khr_display\"");
    require_line(cfg, "video_threaded = \"false\"");
    require_line(cfg, "video_max_swapchain_images = \"2\"");
}

TEST_CASE("renderer_for_core routes N64 cores to GL, others to Vulkan",
          "[retroarch][video][gl]") {
    using retroarch::Renderer;
    REQUIRE(retroarch::renderer_for_core("mupen64plus_next_libretro") == Renderer::GL);
    REQUIRE(retroarch::renderer_for_core("parallel_n64_libretro") == Renderer::GL);
    // Everything the kiosk already ships stays on Vulkan, incl. Dreamcast.
    REQUIRE(retroarch::renderer_for_core("flycast_libretro") == Renderer::Vulkan);
    REQUIRE(retroarch::renderer_for_core("pcsx_rearmed_libretro") == Renderer::Vulkan);
    REQUIRE(retroarch::renderer_for_core("snes9x2010_libretro") == Renderer::Vulkan);
    REQUIRE(retroarch::renderer_for_core("fbneo_libretro") == Renderer::Vulkan);
}
