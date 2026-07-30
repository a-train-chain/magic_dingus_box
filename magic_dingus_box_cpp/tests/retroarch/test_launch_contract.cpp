#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

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
    // RetroArch's setting names carry NO video_ prefix. These were emitted as
    // video_custom_viewport_* for a long time, which RetroArch does not
    // recognise as settings at all — it silently ignored every one of them.
    // Verified against the shipped binary's own setting-name table.
    // x/y are an offset from CENTRE, not absolute screen coordinates —
    // established on hardware, see write_video_config. The bezel opening is
    // centred to within 3px, so 0/0 is the alignment.
    require_line(config, "custom_viewport_x = \"0\"");
    require_line(config, "custom_viewport_y = \"0\"");
    require_line(config, "custom_viewport_width = \"1440\"");
    require_line(config, "custom_viewport_height = \"1080\"");
    // 23 = ASPECT_RATIO_CUSTOM, the ONLY index that makes RetroArch honour
    // the viewport above. 22 is ASPECT_RATIO_CORE ("Core provided"), which
    // fits the core's own aspect to the full screen and ignores the viewport
    // entirely — measured on hardware as 1440x1080 at x=240 instead of the
    // intended 1415x1059 at (251,10), overflowing the bezel cutout by ~11px
    // on every side. See enum aspect_ratio in RetroArch's gfx/video_defines.h.
    require_line(config, "aspect_ratio_index = \"23\"");
    // video_custom_viewport_enable was never a RetroArch setting either;
    // ASPECT_RATIO_CUSTOM is what turns the viewport on.
    REQUIRE(config.find("video_custom_viewport") == std::string::npos);
    require_line(config, "input_overlay_enable = \"true\"");
    require_line(config, "input_overlay_opacity = \"1.0\"");
    require_line(config, "input_overlay_hide_in_menu = \"true\"");
}

TEST_CASE("every core renders the same 4:3 box inside the bezel cutout",
          "[retroarch][video]") {
    // The user's requirement is that EVERY console draws 4:3, not each core's
    // own idea of its shape. That is what ASPECT_RATIO_CUSTOM plus a 4:3
    // viewport buys: the core's reported aspect stops mattering. Under the
    // old ASPECT_RATIO_CORE the systems genuinely differed — SNES reports
    // 1.306, Dreamcast 1.333 — so each drew a slightly different rectangle.
    //
    // Bezel opening MEASURED from the alpha channel of
    // assets/bezels/mdb_kv19.png (1920x1080): x 251..1665 (width 1415),
    // vertically 1053-1059 depending on threshold. If a future bezel moves
    // that opening these must move with it — a mismatch is invisible in every
    // log and only shows up as the frame overlapping the picture.
    retroarch::LaunchOptions options;
    options.display_mode = app::DisplayMode::MODERN_TV;

    std::ostringstream output;
    retroarch::write_video_config(output, options);
    const std::string config = output.str();

    const int w = 1440, h = 1080;
    require_line(config, "custom_viewport_width = \"" + std::to_string(w) + "\"");
    require_line(config, "custom_viewport_height = \"" + std::to_string(h) + "\"");

    // Exactly 4:3 with no rounding — this is the whole point, so the
    // tolerance is tight enough that only 1440x1080 (and its exact
    // multiples) can pass.
    REQUIRE(w * 3 == h * 4);
    // Full screen height, centred. Anything shorter leaves a sliver of
    // background between the picture and the bezel.
    REQUIRE(h == 1080);
    require_line(config, "custom_viewport_x = \"0\"");
    require_line(config, "custom_viewport_y = \"0\"");
}

TEST_CASE("the viewport always fills the height, whatever the shape",
          "[retroarch][video]") {
    // A cropped N64 title is no longer 4:3. Rather than squeeze it back into
    // a 4:3 box (up to 7% distortion, visible), the viewport keeps the full
    // 1080 height and widens. The excess tucks under the bezel, which is an
    // overlay drawn on top of the video — the same overscan a real CRT had.
    struct Case { double aspect; int expect_w; };
    for (const Case& c : {Case{4.0 / 3.0, 1440},
                          Case{1.399, 1511},     // Banjo-Kazooie, cropped
                          Case{1.426, 1540}}) {  // F-Zero X, cropped
        retroarch::LaunchOptions options;
        options.display_mode = app::DisplayMode::MODERN_TV;
        options.content_aspect = c.aspect;

        std::ostringstream output;
        retroarch::write_video_config(output, options);
        INFO("aspect " << c.aspect);
        require_line(output.str(), "custom_viewport_height = \"1080\"");
        require_line(output.str(),
                     "custom_viewport_width = \"" + std::to_string(c.expect_w) + "\"");
    }
}

TEST_CASE("an absurd aspect cannot produce a viewport wider than the panel",
          "[retroarch][video]") {
    retroarch::LaunchOptions options;
    options.display_mode = app::DisplayMode::MODERN_TV;
    options.content_aspect = 5.0;  // would want 5400px

    std::ostringstream output;
    retroarch::write_video_config(output, options);
    // Past the panel width RetroArch letterboxes to fit and the
    // fill-the-height guarantee is lost, so it clamps instead.
    require_line(output.str(), "custom_viewport_width = \"1920\"");
    require_line(output.str(), "custom_viewport_height = \"1080\"");
}

TEST_CASE("a title with no crop stays exactly 4:3",
          "[retroarch][video][overscan]") {
    // Everything with no table entry — Dreamcast, PS1, SNES, and the five N64
    // titles whose border could not be trusted — must come out precisely 4:3.
    REQUIRE(retroarch::n64_content_aspect("mupen64plus_next_libretro", "data/roms/n64/Perfect Dark (USA).z64")
            == Catch::Approx(4.0 / 3.0));
    REQUIRE(retroarch::n64_content_aspect("mupen64plus_next_libretro", "data/roms/dreamcast/Crazy Taxi (USA).chd")
            == Catch::Approx(4.0 / 3.0));
}

TEST_CASE("a cropped title reports the shape the crop actually leaves",
          "[retroarch][video][overscan]") {
    // Banjo-Kazooie: crop top 12, bottom 15, left 20, right 15 leaves
    // 285x213 of a 320x240 frame whose pixels are square, so 285/213.
    const double got =
        retroarch::n64_content_aspect("mupen64plus_next_libretro", "data/roms/n64/Banjo-Kazooie (USA).z64");
    REQUIRE(got == Catch::Approx(285.0 / 213.0).epsilon(0.001));

    // F-Zero X crops much more vertically than horizontally, so its remainder
    // is decidedly wider than 4:3 — this is the case that would have been
    // squeezed ~7% by forcing it back into a 4:3 box.
    const double fzero =
        retroarch::n64_content_aspect("mupen64plus_next_libretro", "data/roms/n64/F-Zero X (USA).z64");
    REQUIRE(fzero == Catch::Approx(296.0 / 208.0).epsilon(0.001));
    REQUIRE(fzero > 4.0 / 3.0);
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
    // 0 = ASPECT_RATIO_4_3. This branch used to emit 23
    // (ASPECT_RATIO_CUSTOM) while writing no custom_viewport_* values at all,
    // which leaves RetroArch pointed at a zero-sized viewport — it happens to
    // fall back to full screen, so the picture was right by accident rather
    // than by instruction. A 640x480 framebuffer is already 4:3, so naming
    // the ratio outright is the same picture with a defined reason.
    require_line(config, "aspect_ratio_index = \"0\"");
    REQUIRE(config.find("custom_viewport") == std::string::npos);
    REQUIRE(config.find("input_overlay =") == std::string::npos);
}

TEST_CASE("PS1 core disables frame skipping and preserves native performance options",
          "[retroarch][core-options][ps1]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "pcsx_rearmed_libretro",
                                  "data/roms/ps1/Castlevania: Symphony of "
                                  "the Night (USA).chd");
    const std::string config = output.str();

    REQUIRE(config.find("pcsx_rearmed_pad1type") == std::string::npos);
    REQUIRE(config.find("pcsx_rearmed_pad2type") == std::string::npos);
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

TEST_CASE("PS1 cores pass DualShock command-line device overrides",
          "[retroarch][input-device][ps1]") {
    const std::vector<std::string> expected = {
        "--device", "1:517", "--device", "2:517",
    };
    const char* ps1_cores[] = {
        "pcsx_rearmed_libretro",
        "beetle_psx_libretro",
        "swanstation_libretro",
    };
    for (const char* core_name : ps1_cores) {
        CAPTURE(core_name);
        REQUIRE(retroarch::core_input_device_args(core_name) == expected);
    }

    const char* non_ps1_cores[] = {
        "flycast_libretro",
        "mupen64plus_next_libretro",
        "nestopia_libretro",
    };
    for (const char* core_name : non_ps1_cores) {
        CAPTURE(core_name);
        REQUIRE(retroarch::core_input_device_args(core_name).empty());
    }
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

// --- N64 -----------------------------------------------------------------
// Every option key and value below was read out of the shipped
// mupen64plus_next_libretro.so string table, not from memory. An invalid
// value is silently ignored by the core, which would look like "the setting
// did nothing" rather than an error — so these must stay exact.

TEST_CASE("N64 core pins GLideN64 and the ARM64 dynarec",
          "[retroarch][core-options][n64]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "mupen64plus_next_libretro",
                                  "data/roms/n64/Super Mario 64 (USA).z64");
    const std::string config = output.str();

    // GLideN64 (GLES3) is the only viable RDP path on V3D.
    require_line(config, "mupen64plus-rdp-plugin = \"gliden64\"");
    // ParaLLEl-RDP is a Vulkan compute renderer measured ~7x slower than the
    // CPU on V3DV, and Angrylion is a software rasterizer. Either one turns
    // a playable game into a slideshow, so neither may ever be selected.
    REQUIRE(config.find("angrylion") == std::string::npos);
    REQUIRE(config.find("parallel") == std::string::npos);

    require_line(config, "mupen64plus-cpucore = \"dynamic_recompiler\"");
    // Emulation speed is CPU-bound here; the spare A76 cores are free.
    require_line(config, "mupen64plus-ThreadedRenderer = \"True\"");
    require_line(config, "mupen64plus-Framerate = \"Original\"");
}

TEST_CASE("N64 core keeps framebuffer emulation on",
          "[retroarch][core-options][n64]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "mupen64plus_next_libretro",
                                  "data/roms/n64/Conker's Bad Fur Day (USA).z64");
    const std::string config = output.str();

    // Conker, DK64, Perfect Dark, Majora's Mask and GoldenEye all render
    // core effects through the framebuffer. With this off they don't render
    // slightly wrong — they render not at all.
    require_line(config, "mupen64plus-EnableFBEmulation = \"True\"");
    require_line(config, "mupen64plus-EnableLODEmulation = \"True\"");
}

TEST_CASE("N64 core leaves the 8MB Expansion Pak enabled",
          "[retroarch][core-options][n64]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "mupen64plus_next_libretro",
                                  "data/roms/n64/Donkey Kong 64 (USA).z64");
    // Donkey Kong 64 and Majora's Mask REFUSE TO BOOT without the Expansion
    // Pak, and Perfect Dark loses most of its content. Disabling extra
    // memory must never be the default.
    require_line(output.str(), "mupen64plus-ForceDisableExtraMem = \"False\"");
}

TEST_CASE("N64 core gives the C-buttons their own slots",
          "[retroarch][core-options][n64]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "mupen64plus_next_libretro",
                                  "data/roms/n64/Super Mario 64 (USA).z64");
    // With alt-map off, mupen64plus-next OVERLAYS the C-buttons onto the A/B
    // slots -- visible in the core's own input descriptors, which read
    // "A Button (C3)" and "B Button (C2)" instead of plain "A Button" /
    // "B Button". One RetroPad slot then serves two N64 controls and the
    // C-button behaviour wins.
    //
    // Observed on hardware 2026-07-29 with a third-party SHANWAN pad
    // (2563:0526) in Super Mario 64: the button bound to RetroPad B zoomed the
    // camera (acting as C2) instead of jumping, and NO button on the pad
    // produced a jump at all. RetroArch's Port 1 Controls screen reported the
    // binding as correct throughout, because at the RetroPad layer it was --
    // the collision is inside the core, one layer below.
    //
    // The core's own description of this option is "useful for some 3rdparty
    // controllers", which is exactly the case. Every pad and every N64 title
    // depends on it, so it is pinned rather than left to the core default.
    require_line(output.str(), "mupen64plus-alt-map = \"True\"");
}

TEST_CASE("the backup N64 core also gets independent C-buttons",
          "[retroarch][core-options][n64]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "parallel_n64_libretro",
                                  "data/roms/n64/Super Mario 64 (USA).z64");
    // Both N64 cores are reachable from the kiosk, so a pad that works on one
    // must not silently break on the other.
    require_line(output.str(), "mupen64plus-alt-map = \"True\"");
}

TEST_CASE("N64 renders at a Pi-5-safe internal resolution",
          "[retroarch][core-options][n64]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "mupen64plus_next_libretro",
                                  "data/roms/n64/Mario Kart 64 (USA).z64");
    const std::string config = output.str();
    // 2x native (N64 is 320x240). The kiosk scales this into its 4:3
    // viewport anyway, and N64 is the thermally sensitive tier on this
    // board — resolution is the first thing to give back.
    require_line(config, "mupen64plus-43screensize = \"640x480\"");
    require_line(config, "mupen64plus-BilinearMode = \"standard\"");
}

// --- N64 overscan -------------------------------------------------------
// Many N64 games draw less than the full framebuffer and leave a black
// border that a CRT's overscan used to hide. The amount is per title —
// measured by capturing ~10 frames per game with the bezel off and taking
// the MINIMUM border, since cropping past the smallest border any frame
// showed would clip real picture. Units are 320x240 N64 pixels.

TEST_CASE("N64 overscan cropping is always explicitly stated",
          "[retroarch][core-options][n64][overscan]") {
    // A title with no table entry must still emit zeros. Leaving the keys out
    // entirely would let a previous title's saved value stand — the same
    // shadowing trap the per-core .opt deletion exists to close.
    std::ostringstream output;
    retroarch::write_core_options(output, "mupen64plus_next_libretro",
                                  "data/roms/n64/Star Fox 64 (USA).z64");
    const std::string config = output.str();
    require_line(config, "mupen64plus-EnableOverscan = \"Enabled\"");
    require_line(config, "mupen64plus-OverscanTop = \"0\"");
    require_line(config, "mupen64plus-OverscanBottom = \"0\"");
    require_line(config, "mupen64plus-OverscanLeft = \"0\"");
    require_line(config, "mupen64plus-OverscanRight = \"0\"");
}

TEST_CASE("a measured title gets its own crop", "[retroarch][core-options][n64][overscan]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "mupen64plus_next_libretro",
                                  "data/roms/n64/Banjo-Kazooie (USA).z64");
    const std::string config = output.str();
    // Measured with the OSD off: 20 left, 15 right, 12 top, 15 bottom (N64 px).
    require_line(config, "mupen64plus-OverscanTop = \"12\"");
    require_line(config, "mupen64plus-OverscanBottom = \"15\"");
    require_line(config, "mupen64plus-OverscanLeft = \"20\"");
    require_line(config, "mupen64plus-OverscanRight = \"15\"");
}

TEST_CASE("the backup N64 core never gets a crop it cannot perform",
          "[retroarch][core-options][n64][overscan]") {
    // parallel_n64 implements NONE of the five overscan keys — its shipped
    // .so contains zero of them where mupen64plus_next contains all five. It
    // therefore hands over the FULL uncropped frame. Widening the viewport to
    // a cropped shape on top of that would stretch the picture horizontally,
    // which is exactly the distortion this whole design exists to avoid. The
    // crop and the viewport are driven from one table, so they must agree.
    std::ostringstream output;
    retroarch::write_core_options(output, "parallel_n64_libretro",
                                  "data/roms/n64/Banjo-Kazooie (USA).z64");
    const std::string config = output.str();
    // It still gets the shared mupen64plus-* performance contract...
    require_line(config, "mupen64plus-rdp-plugin = \"gliden64\"");
    // ...but no overscan keys at all.
    REQUIRE(config.find("Overscan") == std::string::npos);
    // And a plain 4:3 viewport, not Banjo's cropped 285x213 shape.
    REQUIRE(retroarch::n64_content_aspect("parallel_n64_libretro",
                                          "data/roms/n64/Banjo-Kazooie (USA).z64")
            == Catch::Approx(4.0 / 3.0));
    // The accurate core still crops the same title.
    REQUIRE(retroarch::n64_content_aspect("mupen64plus_next_libretro",
                                          "data/roms/n64/Banjo-Kazooie (USA).z64")
            > 4.0 / 3.0);
}

TEST_CASE("titles whose border could not be trusted are left uncropped",
          "[retroarch][core-options][n64][overscan]") {
    // Each of these measured a vertical border of 19-42 N64 pixels, which is
    // not a framebuffer edge — it is a dark or letterboxed scene the sampler
    // caught. Cropping to it would zoom the game into its own middle, so they
    // are deliberately absent from the table. This test exists so nobody
    // "helpfully" fills them in from the raw sweep numbers later.
    for (const char* rom : {"Donkey Kong 64 (USA).z64", "Jet Force Gemini (USA).z64",
                            "Perfect Dark (USA).z64", "Star Fox 64 (USA).z64",
                            "Wave Race 64 - Kawasaki Jet Ski (USA).z64"}) {
        std::ostringstream output;
        retroarch::write_core_options(output, "mupen64plus_next_libretro",
                                      std::string("data/roms/n64/") + rom);
        INFO(rom);
        require_line(output.str(), "mupen64plus-OverscanTop = \"0\"");
        require_line(output.str(), "mupen64plus-OverscanBottom = \"0\"");
        require_line(output.str(), "mupen64plus-OverscanLeft = \"0\"");
        require_line(output.str(), "mupen64plus-OverscanRight = \"0\"");
        REQUIRE(retroarch::n64_content_aspect("mupen64plus_next_libretro", std::string("data/roms/n64/") + rom)
                == Catch::Approx(4.0 / 3.0));
    }
}

TEST_CASE("no cropped title ends up wider than the panel can show",
          "[retroarch][video][overscan]") {
    // Every entry in the table, run through the viewport maths. A crop that
    // left an extreme shape would clamp at 1920 and silently stop filling the
    // height, which is the one guarantee this whole design rests on.
    for (const char* rom : {"Banjo-Kazooie", "Banjo-Tooie", "Conker's Bad Fur Day",
                            "Diddy Kong Racing", "F-Zero X", "GoldenEye 007",
                            "Kirby 64", "Legend of Zelda, The - Majora's Mask",
                            "Legend of Zelda, The - Ocarina of Time",
                            "Mario Kart 64", "Mario Party 3", "Mario Tennis",
                            "Paper Mario", "Super Mario 64", "Super Smash Bros."}) {
        const double aspect =
            retroarch::n64_content_aspect("mupen64plus_next_libretro", std::string("data/roms/n64/") + rom + ".z64");
        const int width = static_cast<int>(std::lround(1080 * aspect));
        INFO(rom << " -> aspect " << aspect << ", viewport " << width << "x1080");
        REQUIRE(width < 1920);
        // And nothing should be wildly off 4:3 — that would mean a bad crop.
        REQUIRE(aspect > 1.20);
        REQUIRE(aspect < 1.50);
    }
}

TEST_CASE("overscan matching is case-insensitive and path-independent",
          "[retroarch][core-options][n64][overscan]") {
    // The playlist path, an absolute path and a differently-cased filename
    // must all resolve to the same title.
    for (const char* path : {
             "data/roms/n64/Banjo-Kazooie (USA).z64",
             "/opt/magic_dingus_box/magic_dingus_box_cpp/data/roms/n64/BANJO-KAZOOIE (USA).z64",
             "banjo-kazooie.z64"}) {
        std::ostringstream output;
        retroarch::write_core_options(output, "mupen64plus_next_libretro", path);
        INFO("path: " << path);
        require_line(output.str(), "mupen64plus-OverscanBottom = \"15\"");
    }
}

TEST_CASE("no crop exceeds what the frame can spare",
          "[retroarch][core-options][n64][overscan]") {
    // A runaway value here would silently zoom a game into its own centre.
    // The N64 frame is 320x240, so even a generous border cannot approach
    // a third of either axis.
    for (const char* rom : {"Banjo-Kazooie (USA).z64", "Super Mario 64 (USA).z64",
                            "Mario Kart 64 (USA).z64", "Conker's Bad Fur Day (USA).z64",
                            "GoldenEye 007 (USA).z64"}) {
        std::ostringstream output;
        retroarch::write_core_options(output, "mupen64plus_next_libretro",
                                      std::string("data/roms/n64/") + rom);
        const std::string cfg = output.str();
        for (const auto& key : {"mupen64plus-OverscanTop", "mupen64plus-OverscanBottom",
                                "mupen64plus-OverscanLeft", "mupen64plus-OverscanRight"}) {
            const auto pos = cfg.find(std::string(key) + " = \"");
            REQUIRE(pos != std::string::npos);
            const auto start = cfg.find('"', pos) + 1;
            const int value = std::stoi(cfg.substr(start, cfg.find('"', start) - start));
            INFO(rom << " " << key << " = " << value);
            REQUIRE(value >= 0);
            REQUIRE(value <= 40);
        }
    }
}

TEST_CASE("parallel_n64 backup core gets the same N64 contract",
          "[retroarch][core-options][n64]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "parallel_n64_libretro",
                                  "data/roms/n64/GoldenEye 007 (USA).z64");
    const std::string config = output.str();
    require_line(config, "mupen64plus-rdp-plugin = \"gliden64\"");
    require_line(config, "mupen64plus-cpucore = \"dynamic_recompiler\"");
    require_line(config, "mupen64plus-ForceDisableExtraMem = \"False\"");
}

TEST_CASE("N64 and PS1 option sets never bleed into each other",
          "[retroarch][core-options][n64]") {
    std::ostringstream n64_out;
    retroarch::write_core_options(n64_out, "mupen64plus_next_libretro",
                                  "data/roms/n64/Paper Mario (USA).z64");
    REQUIRE(n64_out.str().find("pcsx_rearmed") == std::string::npos);

    std::ostringstream ps1_out;
    retroarch::write_core_options(ps1_out, "pcsx_rearmed_libretro",
                                  "data/roms/ps1/Tekken 3 (USA).chd");
    REQUIRE(ps1_out.str().find("mupen64plus") == std::string::npos);
}

// --- Dreamcast -----------------------------------------------------------
// Same rule as N64: every key and every value below was read out of the
// shipped flycast_libretro.so — not the string table this time but the
// retro_core_option_v2_definition table itself, so the value lists are the
// core's own and not a plausible-looking guess. (That distinction matters
// here: the cable-type value is the bare string "VGA", NOT the "VGA (RGB)"
// that flycast's own UI and every forum post calls it. RetroArch would have
// silently ignored the longer form and left the box on composite.)

TEST_CASE("Dreamcast core pins threaded rendering and native resolution",
          "[retroarch][core-options][dreamcast]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "flycast_libretro",
                                  "data/roms/dreamcast/Crazy Taxi (USA).chd");
    const std::string config = output.str();

    // The SH4 recompiler and the renderer on separate threads. flycast's own
    // help text calls this "Highly recommended"; the Pi 5's spare A76 cores
    // are idle during emulation so it is close to free.
    require_line(config, "reicast_threaded_rendering = \"enabled\"");
    // Dreamcast renders 640x480 natively and the kiosk scales that into its
    // 4:3 viewport regardless. Upscaling is the first thing that would cost
    // frames on V3D, so it stays pinned at native until measured otherwise.
    require_line(config, "reicast_internal_resolution = \"640x480\"");
}

TEST_CASE("Dreamcast core selects the progressive VGA signal",
          "[retroarch][core-options][dreamcast]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "flycast_libretro",
                                  "data/roms/dreamcast/Soulcalibur (USA).chd");
    // The core default is "TV (Composite)", which is right for a real CRT and
    // wrong for the kiosk's HDMI panel — it gives interlaced/240p-style
    // output where VGA gives 480p progressive. Titles with no VGA mode are
    // handled by flycast itself ("Game doesn't support VGA. Using TV
    // Composite instead"), so this is safe to set globally.
    require_line(output.str(), "reicast_cable_type = \"VGA\"");
    // The value is the bare token. Anything longer is not in the core's
    // value list and would be dropped without a word in any log.
    REQUIRE(output.str().find("VGA (RGB)") == std::string::npos);
}

TEST_CASE("Dreamcast core gives every game its own memory card",
          "[retroarch][core-options][dreamcast]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "flycast_libretro",
                                  "data/roms/dreamcast/Sonic Adventure (USA).chd");
    // Default "disabled" shares eight VMU files across the whole library.
    // A real VMU holds 200 blocks and Dreamcast saves are big — Sonic
    // Adventure alone wants a sizeable chunk. With seventeen games sharing
    // them the cards fill up, and the kiosk has no memory-card manager for
    // anyone to go and free space in.
    require_line(output.str(), "reicast_per_content_vmus = \"VMU A1\"");
}

TEST_CASE("Dreamcast core does not phone home",
          "[retroarch][core-options][dreamcast]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "flycast_libretro",
                                  "data/roms/dreamcast/ChuChu Rocket! (USA).chd");
    // reicast_dcnet defaults to "enabled" — it is a third-party cloud relay
    // for Dreamcast online play. A shipped appliance should not be opening
    // connections to a service the owner never agreed to, and none of these
    // titles are played online here.
    require_line(output.str(), "reicast_dcnet = \"disabled\"");
}

TEST_CASE("Dreamcast core prefers real BIOS over the HLE fallback",
          "[retroarch][core-options][dreamcast]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "flycast_libretro",
                                  "data/roms/dreamcast/Grandia II (USA).chd");
    // "disabled" means "use dc_boot.bin if it is there". flycast already
    // falls back to REIOS on its own when the BIOS is absent, so pinning
    // this costs nothing on an un-BIOS'd box and gets the accurate path the
    // moment one is dropped in.
    require_line(output.str(), "reicast_hle_bios = \"disabled\"");
    require_line(output.str(), "reicast_region = \"USA\"");
    require_line(output.str(), "reicast_broadcast = \"NTSC\"");
}

// --- per-core .opt shadowing -------------------------------------------
// RetroArch's config/<Core Name>/<Core Name>.opt takes precedence over
// core_options_path. The launcher deleted exactly one of them — PCSX-
// ReARMed's — so on this box Mupen64Plus-Next.opt and Flycast.opt silently
// won and every value in write_core_options() was a no-op. Verified on
// hardware: with Flycast.opt in place the core ran on "TV (Composite)" and
// DCNet enabled; parked, the same launch came up "VGA" and DCNet disabled.
//
// The prefix has to be derived from the same place the options are, or the
// two drift and the shadowing comes back silently.

TEST_CASE("every core that gets options also reports a key prefix",
          "[retroarch][core-options]") {
    const char* cores[] = {
        "pcsx_rearmed_libretro", "beetle_psx_libretro", "swanstation_libretro",
        "mupen64plus_next_libretro", "parallel_n64_libretro",
        "flycast_libretro",
    };
    for (const char* core : cores) {
        std::ostringstream out;
        retroarch::write_core_options(out, core, "data/roms/x/Game.chd");
        const std::string prefix = retroarch::core_options_key_prefix(core);
        INFO("core: " << core);
        REQUIRE_FALSE(out.str().empty());
        REQUIRE_FALSE(prefix.empty());
        // Every line written must actually start with the advertised prefix,
        // otherwise deleting by prefix leaves part of the set shadowed.
        std::istringstream lines(out.str());
        std::string line;
        while (std::getline(lines, line)) {
            if (line.empty()) continue;
            INFO("line: " << line);
            REQUIRE(line.rfind(prefix, 0) == 0);
        }
    }
}

TEST_CASE("a core with no options reports no prefix to delete by",
          "[retroarch][core-options]") {
    // Deleting on an empty prefix would match every .opt file on the box.
    for (const char* core : {"nestopia_libretro", "snes9x2010_libretro",
                             "genesis_plus_gx_libretro", "fbneo_libretro"}) {
        std::ostringstream out;
        retroarch::write_core_options(out, core, "data/roms/nes/Game.zip");
        INFO("core: " << core);
        REQUIRE(out.str().empty());
        REQUIRE(retroarch::core_options_key_prefix(core).empty());
    }
}

TEST_CASE("N64 sibling cores share one option namespace",
          "[retroarch][core-options][n64]") {
    // Both mupen64plus_next and parallel_n64 use mupen64plus-* keys, so one
    // prefix has to clear both .opt files or switching cores re-shadows.
    REQUIRE(retroarch::core_options_key_prefix("mupen64plus_next_libretro") ==
            retroarch::core_options_key_prefix("parallel_n64_libretro"));
}

TEST_CASE("Dreamcast options do not bleed into the other cores",
          "[retroarch][core-options][dreamcast]") {
    std::ostringstream dc_out;
    retroarch::write_core_options(dc_out, "flycast_libretro",
                                  "data/roms/dreamcast/Bangai-O (USA).chd");
    REQUIRE(dc_out.str().find("mupen64plus") == std::string::npos);
    REQUIRE(dc_out.str().find("pcsx_rearmed") == std::string::npos);

    std::ostringstream n64_out;
    retroarch::write_core_options(n64_out, "mupen64plus_next_libretro",
                                  "data/roms/n64/Star Fox 64 (USA).z64");
    REQUIRE(n64_out.str().find("reicast") == std::string::npos);
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

TEST_CASE("menu-toggle combo opens Quick Menu with L1 R1 Start Select",
          "[retroarch][hotkeys][menu-combo]") {
    std::ostringstream out;
    retroarch::write_menu_toggle_combo_config(out);
    const std::string config = out.str();

    require_line(config, "input_menu_toggle_gamepad_combo = \"3\"");
    REQUIRE(config.find("input_menu_toggle_gamepad_combo = \"1\"") ==
            std::string::npos);
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
    // Name the KMS/EGL/GBM context EXPLICITLY. Leaving this empty made
    // RetroArch walk its context priority list, and on this headless box
    // that means probing Wayland first and logging
    //   [ERROR] [Wayland]: Failed to connect to Wayland server.
    // before recovering with [GL]: Found GL context: "kms". Observed live
    // on the Pi 5 launching Banjo-Kazooie: it worked, but the wasted probe
    // put a spurious ERROR in the launcher log and tripped the emulator
    // smoke test's fatal-video detector. "kms" is the GL/EGL context name
    // ("khr_display" is the Vulkan one — emitting THAT for a GL core is
    // what black-screens it).
    require_line(cfg, "video_context_driver = \"kms\"");
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
