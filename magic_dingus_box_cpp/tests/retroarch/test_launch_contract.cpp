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
    require_line(config, "video_fullscreen_x = \"640\"");
    require_line(config, "video_fullscreen_y = \"480\"");
    require_line(config, "video_custom_viewport_enable = \"false\"");
    require_line(config, "aspect_ratio_index = \"23\"");
    REQUIRE(config.find("input_overlay =") == std::string::npos);
}

TEST_CASE("PS1 core disables frame skipping and preserves native performance options",
          "[retroarch][core-options][ps1]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "pcsx_rearmed_libretro");
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
}

TEST_CASE("non-PS1 core emits no PCSX-ReARMed options",
          "[retroarch][core-options]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "nestopia_libretro");
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
