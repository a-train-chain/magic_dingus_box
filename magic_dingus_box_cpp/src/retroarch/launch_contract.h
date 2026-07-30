#pragma once

#include <chrono>
#include <functional>
#include <iosfwd>
#include <string>
#include <sys/types.h>
#include <vector>

#include "app/app_state.h"

namespace retroarch {

// Which RetroArch video driver the core runs under. Most cores (2D,
// PS1, Dreamcast/flycast) use the kiosk's native Vulkan/khr_display
// path. GL-only cores — notably N64 via GLideN64 (mupen64plus_next /
// parallel_n64) — must run video_driver=gl with an EMPTY context so
// RetroArch auto-selects KMS/EGL/GBM; emitting khr_display for a GL
// core black-screens it (Pi 5 emulation research, 2026-07-22).
enum class Renderer {
    Vulkan,
    GL
};

struct LaunchOptions {
    app::DisplayMode display_mode = app::DisplayMode::CRT_NATIVE;
    std::string bezel_file;
    // Detected board, so Pi-5-specific video tuning has one place to
    // land. Today the emitted contract is identical for every model
    // (pinned by test_launch_contract) — the Pi 4 V3D swapchain
    // workarounds are kept everywhere until re-benchmarked on Pi 5.
    platform::PiModel pi_model = platform::PiModel::Unknown;
    // Video driver for this core. Set from the core name at launch
    // (see renderer_for_core). Defaults to Vulkan for the existing
    // 2D/PS1-class lineup.
    Renderer renderer = Renderer::Vulkan;
    // Display aspect of the picture this core will actually hand over, AFTER
    // any per-title overscan crop. The viewport is sized to fill the screen
    // HEIGHT at this ratio, so the picture always touches top and bottom and
    // its geometry is never stretched; anything wider than the bezel's
    // opening tucks under the frame, which is drawn over the video.
    //
    // 4:3 for everything that has no border to crop — Dreamcast, PS1, SNES
    // and the rest all land here. Only the N64 titles with a measured crop
    // differ, because removing a lopsided border leaves a shape that is no
    // longer 4:3, and forcing THAT back into a 4:3 box is what would squeeze
    // the picture (up to 7% on F-Zero X and GoldenEye — visible).
    double content_aspect = 4.0 / 3.0;
    // Invoked once, immediately before RetroArch is forked and AFTER the
    // launcher script is fully written to disk.
    //
    // This exists so the kiosk can keep drawing its launch screen for as long
    // as physically possible. Building the script is ~0.6s of pure file I/O
    // that needs no display, but it used to happen after the kiosk had already
    // handed over DRM master — so the panel sat on a frozen frame through it
    // for nothing. The caller uses this hook to present its final frame and
    // release the display at the last possible moment.
    //
    // Must not throw. Anything it does is unrecoverable from here: the fork
    // follows immediately.
    std::function<void()> before_fork;
};

// Display aspect an N64 title ends up with once its measured overscan crop is
// applied, or 4:3 when it has no entry in the table. Feeds
// LaunchOptions.content_aspect.
//
// Takes the core because ONLY mupen64plus_next implements the overscan
// options — parallel_n64 has none of the five. Widening the viewport for a
// core that will not actually crop stretches the picture horizontally, which
// is precisely the distortion this design exists to avoid, so the backup core
// always gets a plain 4:3.
double n64_content_aspect(const std::string& core_name,
                          const std::string& rom_path);

// Pick the renderer a core needs. GL for the N64 cores (GLideN64),
// Vulkan for everything else the kiosk ships (incl. Dreamcast/flycast).
Renderer renderer_for_core(const std::string& core_name);

void write_video_config(std::ostream& out, const LaunchOptions& options);

// Exit-hotkey bind for the phone remote: its QUIT_GAME chord emits
// KEY_Z + BTN_START on the "MagicDingus Phone Remote" uinput device.
// The virtual pad has no manual joypad binds (autoconfig is disabled),
// but RetroArch's udev keyboard path DOES deliver its KEY_Z — verified
// on hardware 2026-07-22 — so binding exit to "z" is what makes the
// remote able to quit games. (RetroArch's default exit key was Escape;
// kiosk units ship no keyboards, so re-binding costs nothing.)
void write_remote_quit_config(std::ostream& out);

// Command-line device overrides for the core. Debian RetroArch loads
// input_libretro_device_pN from remap files rather than the global config,
// so supported PS1 cores select DualShock through the explicit CLI path.
std::vector<std::string> core_input_device_args(
    const std::string& core_name);

// Pick the ALSA device for HDMI game audio from `aplay -L` output.
// Selects vc4hdmi0 by NAME (the kiosk's display port is HDMI0) instead
// of by card number: on Pi 4 vc4hdmi0 is ALSA card 1 (behind the
// Headphones card) but on Pi 5 it is card 0, so the old hardcoded
// "plughw:1,0" fallback silently routed Pi 5 game audio to the empty
// HDMI1 port. Falls back to vc4hdmi1, then the legacy default so
// behavior on non-Pi dev boxes is unchanged.
std::string pick_hdmi_alsa_device(const std::string& aplay_L_output);
// rom_path selects per-title performance overrides (e.g. the THPS4
// overclock); pass the launch path as-is — matching is filename-based
// and case-insensitive.
void write_core_options(std::ostream& out, const std::string& core_name,
                        const std::string& rom_path);

// The option-key prefix write_core_options() emits for this core, or "" when
// it writes nothing.
//
// This exists so the launcher can delete the per-core options file that would
// otherwise shadow those options. RetroArch's
// config/<Core Name>/<Core Name>.opt takes precedence over
// core_options_path, so a stale .opt silently wins and every carefully
// verified value in write_core_options() becomes a no-op — no error, no log
// line, just the core's defaults. Deriving the prefix from the same place the
// options come from means the two cannot drift apart.
std::string core_options_key_prefix(const std::string& core_name);
const char* audio_driver_for_gameplay();
int audio_latency_ms_for_core(const std::string& core_name);

enum class StartupStatus {
    Ready,
    Exited,
    TimedOut,
    WaitError,
};

struct ReadyWatchOptions {
    std::string ready_file = "/tmp/retroarch_mdb.ready";
    std::string drm_card_pattern = "/dev/dri/card*";
};

std::string build_kms_ready_watch_block(const std::string& command,
                                        const ReadyWatchOptions& options);

StartupStatus wait_for_startup(
    pid_t launcher_pid,
    const std::string& ready_file,
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds poll_interval = std::chrono::milliseconds(50));

bool terminate_process_group(pid_t launcher_pid,
                             std::chrono::milliseconds grace);

}  // namespace retroarch
