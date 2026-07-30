#include "launch_contract.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
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

bool is_n64_core(const std::string& core_name) {
    return core_name.find("mupen64plus") != std::string::npos ||
           core_name.find("parallel_n64") != std::string::npos;
}

bool is_dreamcast_core(const std::string& core_name) {
    return core_name.find("flycast") != std::string::npos;
}

// Only mupen64plus_next implements the GLideN64 overscan options. The shipped
// parallel_n64_libretro.so contains none of the five keys (checked against its
// string table); it would ignore them and hand over an uncropped frame.
bool core_supports_overscan(const std::string& core_name) {
    return core_name.find("mupen64plus") != std::string::npos;
}

}  // namespace

void write_remote_quit_config(std::ostream& out) {
    // See launch_contract.h. Keep in sync with QUIT_GAME in
    // magic_dingus_box/web/remote/uinput_writer.py (KEY_Z = 44).
    out << "input_exit_emulator = \"z\"\n";
}

std::vector<std::string> core_input_device_args(
    const std::string& core_name) {
    if (!is_ps1_core(core_name)) {
        return {};
    }
    return {"--device", "1:517", "--device", "2:517"};
}

Renderer renderer_for_core(const std::string& core_name) {
    // The N64 cores render through GLideN64 (OpenGL/GLES) and must run on
    // the GL context. Everything else the kiosk ships — including
    // Dreamcast/flycast, which has a working Vulkan path on V3D — uses the
    // native Vulkan/khr_display contract.
    if (core_name.find("mupen64plus") != std::string::npos ||
        core_name.find("parallel_n64") != std::string::npos) {
        return Renderer::GL;
    }
    return Renderer::Vulkan;
}

std::string pick_hdmi_alsa_device(const std::string& aplay_L_output) {
    // Plain substring checks: `aplay -L` prints one device name per
    // line at column 0, and these exact PCM names cannot appear as a
    // substring of another device name. (The previous implementation
    // used std::regex with a `^` anchor, which only matches the start
    // of the WHOLE string in ECMAScript mode — it never matched, and
    // everything fell through to the card-number fallback.)
    if (aplay_L_output.find("sysdefault:CARD=vc4hdmi0") != std::string::npos) {
        return "sysdefault:CARD=vc4hdmi0";
    }
    if (aplay_L_output.find("sysdefault:CARD=vc4hdmi1") != std::string::npos) {
        return "sysdefault:CARD=vc4hdmi1";
    }
    return "plughw:1,0";
}

void write_video_config(std::ostream& out, const LaunchOptions& options) {
    const bool vulkan = (options.renderer == Renderer::Vulkan);

    // --- Driver + context: renderer-dependent ---
    if (!vulkan) {
        // GL path — N64/GLideN64 (mupen64plus_next / parallel_n64). Use
        // video_driver=gl and name the KMS/EGL/GBM context EXPLICITLY as
        // "kms". Emitting "khr_display" (the Vulkan direct-display context)
        // for a GL core black-screens it — confirmed in the Pi 5 research.
        //
        // This was originally left EMPTY to let RetroArch auto-select.
        // That does work, but only via a failed probe: measured live on the
        // Pi 5 launching Banjo-Kazooie, RetroArch walks its context
        // priority list, tries Wayland first on this headless box, logs
        //   [ERROR] [Wayland]: Failed to connect to Wayland server.
        // and only then settles on [GL]: Found GL context: "kms". The game
        // ran fine, but the spurious ERROR line tripped the emulator smoke
        // test's fatal-video detector and made a healthy N64 launch look
        // like a crash. Naming the context makes the GL path deterministic
        // instead of dependent on fallback ordering.
        // The Vulkan swapchain workarounds below are Vulkan-only, so they
        // are skipped here. Threaded video is safe on GL (the swapchain
        // thrash that forces it off was Vulkan-specific) and helps
        // framepacing on the Pi 5's spare A76 cores.
        out << "video_driver = \"gl\"\n";
        out << "video_context_driver = \"kms\"\n";
        out << "video_threaded = \"true\"\n";
    } else {
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
    // Pi 5 RE-BENCHMARKED 2026-07-25 on V3D 7.1 (cooled, 55-59C, no
    // throttling), pcsx_rearmed, 40s per config:
    //     threaded=OFF swap=2  ->  0 QueuePresent failures
    //     threaded=ON  swap=2  ->  1 QueuePresent failure
    //     threaded=OFF swap=3  ->  0
    //     threaded=ON  swap=3  ->  1
    // Threaded video is the only variable that matters; swapchain depth
    // made no difference at all. The Pi 5 failure is much milder than the
    // Pi 4's (one transient at the first SET_GEOMETRY resolution change,
    // swapchain rebuilds, then ~10 further geometry changes with zero
    // errors — it converges, unlike the Pi 4's runaway loop). But it is
    // still one failure the non-threaded path does not produce, with no
    // measured upside for 2D/PS1-class content, so BOTH settings stay as
    // they are on both boards and the parity test still holds.
    //
    // NOT measured: framerate/framepacing. Threaded video may yet be
    // worth the transient for genuinely heavy cores — re-test with N64
    // (mupen64plus_next) or Dreamcast (flycast) when those are added.
    out << "video_threaded = \"false\"\n";
    }  // end vulkan-only driver/context block
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
    // video_threaded=false. Vulkan-only — meaningless on the GL path.
    if (vulkan) {
        out << "video_max_swapchain_images = \"2\"\n";
    }
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
        // RetroArch's setting names carry NO video_ prefix — they are
        // custom_viewport_{x,y,width,height}. These were emitted as
        // video_custom_viewport_* (and alongside a
        // video_custom_viewport_enable that is not a setting at all), so
        // RetroArch never recognised any of them and the viewport below was
        // dead config. Names verified against the shipped binary's own
        // setting table.
        //
        // x/y are an OFFSET FROM CENTRE, not absolute screen coordinates.
        // Proved on hardware: a 1415x1059 viewport with x=0,y=0 lands dead
        // centre in the bezel opening (symmetric vignette, no side panels in
        // frame), while x=251 — the opening's absolute left edge — pushed the
        // picture 251px right and put the bezel's control panel inside the
        // playfield. The opening is centred to within 3px of the screen
        // anyway (left margin 251, right 254), so centring IS the alignment.
        out << "custom_viewport_x = \"0\"\n";
        out << "custom_viewport_y = \"0\"\n";
        // FILL THE HEIGHT, KEEP THE SHAPE. Height is always the full 1080 so
        // the picture touches top and bottom on every core; width is whatever
        // that core's true aspect demands, so geometry is never stretched.
        //
        // For everything without a border to crop this is 1440x1080 — exact
        // 4:3, no rounding. Only N64 titles carrying a measured overscan crop
        // come out wider, because removing a lopsided border leaves a shape
        // that is no longer 4:3; forcing that back into a 4:3 box is what
        // squeezed the picture by up to 7% on F-Zero X and GoldenEye.
        //
        // Running wider than the bezel's opening (measured x 251..1665) is
        // fine and deliberate. The bezel is an OVERLAY drawn on top of the
        // video, so the excess tucks under the frame rather than spilling
        // onto the artwork — which is exactly what a real CRT did, overscanning
        // past the visible edge of the tube. Sizing the picture to stop at the
        // cutout instead would leave a visible sliver of background between
        // picture and frame.
        const int vp_h = 1080;
        int vp_w = static_cast<int>(std::lround(vp_h * options.content_aspect));
        // Never wider than the panel: past that RetroArch would letterbox to
        // fit and we would lose the fill-the-height guarantee entirely.
        vp_w = std::min(vp_w, 1920);
        out << "custom_viewport_width = \"" << vp_w << "\"\n";
        out << "custom_viewport_height = \"" << vp_h << "\"\n";
        // 23 = ASPECT_RATIO_CUSTOM — the only index that makes RetroArch use
        // the viewport above. This said 22 with a comment claiming that meant
        // "custom viewport"; 22 is ASPECT_RATIO_CORE ("Core provided"), which
        // fits whatever aspect the core reports to the FULL screen and
        // discards the viewport. (See enum aspect_ratio in RetroArch's
        // gfx/video_defines.h: 20=CONFIG, 21=SQUARE, 22=CORE, 23=CUSTOM,
        // 24=FULL.)
        //
        // Measured on hardware before the fix: the picture came out
        // 1440x1080 at x=240 — 4:3 fitted to full screen height — against a
        // bezel opening of 1415x1059 at (251,10), so it overflowed the cutout
        // by ~11px on every side and its edges sat under the frame. Because
        // the size came from the CORE, it also moved whenever a core reported
        // a different aspect, which is why this drifts rather than staying
        // consistently wrong.
        out << "aspect_ratio_index = \"23\"\n";

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
        // 0 = ASPECT_RATIO_4_3. This emitted 23 (ASPECT_RATIO_CUSTOM) while
        // writing no custom_viewport_* values at all, pointing RetroArch at a
        // zero-sized viewport; it falls back to full screen, so the picture
        // was right by accident rather than by instruction. A 640x480
        // framebuffer is already 4:3, so this is the same picture with a
        // defined reason behind it.
        out << "aspect_ratio_index = \"0\"\n";
    }
}

namespace {

std::string lowercase(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

// Per-title N64 overscan crop.
//
// Plenty of N64 games draw less than the full framebuffer and leave a black
// border. On a CRT the tube's own overscan swallowed it; on a flat panel it
// is just a black margin sitting inside the bezel, and it differs per title —
// Banjo-Kazooie has one on three sides, Super Mario 64 only along the bottom,
// Mario Kart 64 none at all. So a single global crop is not an option: it
// would fix Banjo and clip Mario Kart, which has nothing to spare.
//
// GLideN64 can crop this (mupen64plus-EnableOverscan defaults to Enabled),
// but all four per-edge offsets default to 0, so nothing is cropped until
// they are set.
//
// EVERY NUMBER HERE WAS MEASURED, not estimated. Each title was launched with
// the bezel overlay disabled and the offsets zeroed, ~10 frames were captured
// across a minute through RetroArch's network SCREENSHOT command, frames too
// dark to measure were discarded, and the MINIMUM black border across the
// rest was taken — the minimum, because cropping more than the smallest
// border any frame showed would clip real picture in that frame.
//
// Units are 320x240 N64 pixels: the 1440x1080 viewport shows a 320x240 frame,
// so one unit is 4.5 screen pixels. Confirmed on hardware by cropping a known
// 20 units and watching a 67px border go to exactly zero (67/4.5 = 14.9, so
// 20 over-cropped by ~5 units, which is what the picture showed).
//
// Cropping costs no performance: it happens after GLideN64 has rendered, so
// the core still draws the same 640x480 either way. Only the rectangle handed
// to the frontend changes, and that is scaled to the same output regardless.
struct N64Overscan {
    const char* rom_name_substring;  // matched lowercase against rom_path
    int top;
    int bottom;
    int left;
    int right;
};

// Five titles are deliberately ABSENT and therefore uncropped: Donkey Kong
// 64, Jet Force Gemini, Perfect Dark, Star Fox 64 and Wave Race 64. Each
// measured a vertical border of 19-42 units, which is not a framebuffer edge
// — it is a dark or letterboxed scene the sampler happened to catch, and
// cropping to it would zoom the game into its own middle. An uncropped title
// looks exactly as it always has; an over-cropped one looks broken. When the
// evidence is weak, the entry stays out.
constexpr N64Overscan kN64Overscan[] = {
    //                                            top bottom left right
    {"banjo-kazooie",                              12,   15,  20,  15},
    {"banjo-tooie",                                 6,    8,  14,  10},
    {"conker's bad fur day",                       12,   15,  20,  15},
    {"diddy kong racing",                           0,    3,   4,   4},
    {"f-zero x",                                   15,   17,  12,  12},
    {"goldeneye 007",                               0,    3,   4,   4},
    {"kirby 64",                                    9,   11,  10,  10},
    {"legend of zelda, the - majora's mask",        0,    3,   4,   4},
    {"legend of zelda, the - ocarina of time",      0,    3,   4,   4},
    {"mario kart 64",                               0,    3,   4,   4},
    {"mario party 3",                               0,    3,   4,   4},
    {"mario tennis",                               10,    8,  14,  15},
    {"paper mario",                                 0,    3,   4,   4},
    {"super mario 64",                              7,    9,   4,   4},
    {"super smash bros.",                           9,   11,  10,  10},
};

const N64Overscan* find_n64_overscan(const std::string& rom_path) {
    const std::string haystack = lowercase(rom_path);
    for (const auto& entry : kN64Overscan) {
        if (haystack.find(entry.rom_name_substring) != std::string::npos) {
            return &entry;
        }
    }
    return nullptr;
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

// Nintendo 64 (mupen64plus_next / parallel_n64).
//
// EVERY key and value here was read out of the shipped
// mupen64plus_next_libretro.so string table rather than recalled. That
// matters more than usual: RetroArch silently ignores a core option value
// the core doesn't recognize, so a typo produces "the setting had no
// effect" with nothing in any log — indistinguishable from the setting
// being wrong for the hardware.
//
// Deliberately NOT set here:
//   - mupen64plus-rsp-plugin: the core default is already HLE, which is the
//     fast path. Naming it buys nothing and risks an invalid literal.
//   - mupen64plus-EnableCopyColorToRDRAM and friends: GLideN64 ships a
//     per-game ini that sets these correctly per title. Forcing one global
//     value would override that curated database with a worse guess.
void write_n64_core_options(std::ostream& out, const std::string& core_name,
                            const std::string& rom_path) {
    // GLideN64 (GLES3) is the ONLY viable RDP path on this board. The
    // alternatives are not "slower", they are unusable: ParaLLEl-RDP is a
    // Vulkan compute renderer measured ~7x slower than the CPU on V3DV, and
    // Angrylion is a pure software rasterizer.
    out << "mupen64plus-rdp-plugin = \"gliden64\"\n";
    // ARM64 dynamic recompilation. Emulation speed here is CPU-bound, so
    // this is the single largest performance lever.
    out << "mupen64plus-cpucore = \"dynamic_recompiler\"\n";
    // The Pi 5's spare A76 cores are idle during emulation; handing the
    // renderer its own thread is close to free. (Note this is the CORE's
    // internal threading, unrelated to RetroArch's video_threaded, which
    // stays off on the Vulkan path for swapchain reasons.)
    out << "mupen64plus-ThreadedRenderer = \"True\"\n";
    // Native N64 timing rather than uncapped — matches the kiosk's 60Hz
    // vsync contract instead of fighting it.
    out << "mupen64plus-Framerate = \"Original\"\n";
    // Framebuffer effects are not cosmetic on this ROM set: Conker, DK64,
    // Perfect Dark, Majora's Mask and GoldenEye render core effects through
    // the framebuffer and lose them entirely when this is off.
    out << "mupen64plus-EnableFBEmulation = \"True\"\n";
    out << "mupen64plus-EnableLODEmulation = \"True\"\n";
    // 8MB Expansion Pak. Donkey Kong 64 and Majora's Mask REFUSE TO BOOT
    // without it and Perfect Dark loses most of its content, so extra
    // memory must stay available. ("False" = do not force-disable.)
    out << "mupen64plus-ForceDisableExtraMem = \"False\"\n";
    // INDEPENDENT C-BUTTON CONTROLS. The core calls this "alt-map" and
    // describes it as "useful for some 3rdparty controllers"; leaving it off
    // is why a third-party pad could not jump in Super Mario 64.
    //
    // With alt-map off, mupen64plus-next OVERLAYS the four C-buttons onto the
    // A/B slots -- which is what the core's own input descriptors are saying
    // when they read "A Button (C3)" and "B Button (C2)" rather than plain
    // "A Button"/"B Button". A RetroPad slot then does double duty, and the
    // C-button behaviour wins. Observed on hardware 2026-07-29 with a SHANWAN
    // pad (2563:0526): the button bound to RetroPad B zoomed the camera out
    // (acting as C2) instead of jumping, and NO button on the pad produced a
    // jump at all. RetroArch's own Port 1 Controls screen showed the binding
    // as correct the whole time, because at the RetroPad layer it was.
    //
    // Turning it on gives the C-buttons their own slots, so A and B mean A and
    // B. This is a CORE-level fix: it applies to every N64 game and every pad,
    // and it is why the mapping tables looked right while the game did not.
    out << "mupen64plus-alt-map = \"True\"\n";
    // 2x native (the N64 renders at 320x240). The kiosk scales this into
    // its 4:3 viewport regardless, and N64 is the thermally sensitive tier
    // on this board — internal resolution is the first thing to give back
    // if a title runs hot. Raise per-title only after measuring on the Pi.
    out << "mupen64plus-43screensize = \"640x480\"\n";
    out << "mupen64plus-BilinearMode = \"standard\"\n";
    // Controller Pak in slot 1 so games that save to it can.
    out << "mupen64plus-pak1 = \"memory\"\n";

    // Crop the game's own black border, per title. Always emit all four
    // offsets — a title with no entry gets explicit zeros rather than
    // whatever a previous title's run left in the core's saved options.
    //
    // Skipped entirely for parallel_n64, which implements none of these keys.
    // Emitting them there would be inert, but the viewport is sized from the
    // same table (see n64_content_aspect) and the two must agree: a widened
    // viewport with an uncropped frame stretches the picture.
    if (!core_supports_overscan(core_name)) {
        return;
    }
    const N64Overscan* overscan = find_n64_overscan(rom_path);
    out << "mupen64plus-EnableOverscan = \"Enabled\"\n";
    out << "mupen64plus-OverscanTop = \""
        << (overscan ? overscan->top : 0) << "\"\n";
    out << "mupen64plus-OverscanBottom = \""
        << (overscan ? overscan->bottom : 0) << "\"\n";
    out << "mupen64plus-OverscanLeft = \""
        << (overscan ? overscan->left : 0) << "\"\n";
    out << "mupen64plus-OverscanRight = \""
        << (overscan ? overscan->right : 0) << "\"\n";
}

// Sega Dreamcast (flycast).
//
// Same discipline as the N64 block above, and for the same reason: an
// unrecognised value is dropped silently, so a wrong literal is
// indistinguishable from the option having no effect. These were read out of
// the shipped flycast_libretro.so's retro_core_option_v2_definition table —
// the core's own declared value lists, not its UI labels. The two differ, and
// the difference bites: the cable-type VALUE is the bare token "VGA" even
// though flycast's menu (and every guide) writes it "VGA (RGB)".
//
// Deliberately NOT set here:
//   - reicast_alpha_sorting: the "per-triangle (normal)" default is the right
//     accuracy/speed point. Per-pixel is an order-buffer path that would be
//     punishing on V3D; per-strip visibly breaks transparency in 2D titles.
//   - reicast_auto_skip_frame / reicast_enable_dsp: real performance
//     tradeoffs with no measurement behind them yet. Defaults until a slow
//     title says otherwise — guessing here is how you ship a stutter.
//   - reicast_digital_triggers: correct value depends on the attached pad
//     (analog on a PS-style pad, digital on the N64 adapter's shoulders) and
//     this function does not know which is plugged in.
void write_dreamcast_core_options(std::ostream& out) {
    // SH4 recompiler and renderer on separate threads. flycast's own help
    // text says "Highly recommended", and the Pi 5's spare A76 cores sit idle
    // during emulation, so this is close to free.
    out << "reicast_threaded_rendering = \"enabled\"\n";
    // Native. The kiosk scales this into its 1440x1080 4:3 viewport, so a
    // higher internal resolution would genuinely look sharper — there is just
    // no room for it on V3D. MEASURED 2026-07-28, 45s runs, CPU as % of 400%:
    //
    //                 640x480   960x720   1440x1080
    //   Sonic Adv 2     56.6      46.2       41.4
    //   Soulcalibur     46.8      24.0       23.2
    //
    // CPU going DOWN as the picture gets bigger is the tell: the emulation
    // thread is blocking on the GPU, so it is rendering FEWER frames, not
    // cheaper ones. Soulcalibur halves at the very first step up and does not
    // recover. There is plenty of CPU headroom here (peak 60C, 25C under the
    // throttle point) — it just cannot be spent on fill rate.
    out << "reicast_internal_resolution = \"640x480\"\n";
    // The core default is "TV (Composite)" — correct for a real CRT, wrong
    // for the kiosk's HDMI panel, where it produces interlaced/240p-style
    // output instead of 480p progressive. Safe to set globally: flycast
    // detects titles with no VGA mode and falls back by itself ("Game
    // doesn't support VGA. Using TV Composite instead").
    out << "reicast_cable_type = \"VGA\"\n";
    // Per-game memory cards. The default shares eight VMU files across the
    // whole library; a real VMU is 200 blocks, Dreamcast saves are large, and
    // the kiosk ships no memory-card manager for anyone to free space in.
    out << "reicast_per_content_vmus = \"VMU A1\"\n";
    // DCNet is a third-party cloud relay for Dreamcast online play and
    // defaults to ENABLED. A shipped appliance should not open connections to
    // a service its owner never agreed to.
    out << "reicast_dcnet = \"disabled\"\n";
    // "disabled" here means "use the real dc_boot.bin when it is present".
    // flycast already falls back to its HLE BIOS (REIOS) on a box with no
    // BIOS installed, so this costs nothing now and takes the accurate path
    // the moment one is dropped into ~/.config/retroarch/system/dc/.
    out << "reicast_hle_bios = \"disabled\"\n";
    // The shipped library is entirely USA/NTSC.
    out << "reicast_region = \"USA\"\n";
    out << "reicast_broadcast = \"NTSC\"\n";
}

}  // namespace

double n64_content_aspect(const std::string& core_name,
                          const std::string& rom_path) {
    // parallel_n64 does not implement ANY of the five overscan options —
    // verified against the shipped .so, which contains zero of them where
    // mupen64plus_next contains all five. It therefore hands over the full
    // uncropped frame, and widening the viewport to a cropped shape would
    // stretch that horizontally. The backup core stays plain 4:3.
    if (!core_supports_overscan(core_name)) {
        return 4.0 / 3.0;
    }
    const N64Overscan* crop = find_n64_overscan(rom_path);
    if (crop == nullptr) {
        return 4.0 / 3.0;
    }
    // A full 320x240 N64 frame is displayed as 4:3, so its pixels are square.
    // Whatever survives the crop therefore has display aspect
    // (kept_w / kept_h) scaled by that same square-pixel ratio, i.e. simply
    // kept_w/kept_h * (240/320) * (4/3) — which reduces to kept_w/kept_h.
    const double kept_w = 320.0 - crop->left - crop->right;
    const double kept_h = 240.0 - crop->top - crop->bottom;
    if (kept_w <= 0.0 || kept_h <= 0.0) {
        return 4.0 / 3.0;  // a nonsense entry must not produce a nonsense viewport
    }
    return (kept_w / kept_h) * (240.0 / 320.0) * (4.0 / 3.0);
}

std::string core_options_key_prefix(const std::string& core_name) {
    // Keep in lockstep with write_core_options() below. Every line that
    // function emits for a core must begin with the prefix returned here —
    // there is a test that walks the output and checks exactly that.
    if (is_n64_core(core_name)) {
        // Both mupen64plus_next and parallel_n64 use mupen64plus-* keys, so
        // one prefix has to clear both .opt files.
        return "mupen64plus-";
    }
    if (is_dreamcast_core(core_name)) {
        // flycast kept reicast_* from its Reicast ancestry.
        return "reicast_";
    }
    if (is_ps1_core(core_name)) {
        return "pcsx_rearmed_";
    }
    return "";
}

void write_core_options(std::ostream& out, const std::string& core_name,
                        const std::string& rom_path) {
    if (is_n64_core(core_name)) {
        write_n64_core_options(out, core_name, rom_path);
        return;
    }
    if (is_dreamcast_core(core_name)) {
        write_dreamcast_core_options(out);
        return;
    }
    if (!is_ps1_core(core_name)) {
        return;
    }
    const Ps1TitleOverride* title_override = find_ps1_override(rom_path);

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
