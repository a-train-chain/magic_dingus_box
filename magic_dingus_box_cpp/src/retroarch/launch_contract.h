#pragma once

#include <chrono>
#include <iosfwd>
#include <string>
#include <sys/types.h>

#include "app/app_state.h"

namespace retroarch {

struct LaunchOptions {
    app::DisplayMode display_mode = app::DisplayMode::CRT_NATIVE;
    std::string bezel_file;
    // Detected board, so Pi-5-specific video tuning has one place to
    // land. Today the emitted contract is identical for every model
    // (pinned by test_launch_contract) — the Pi 4 V3D swapchain
    // workarounds are kept everywhere until re-benchmarked on Pi 5.
    platform::PiModel pi_model = platform::PiModel::Unknown;
};

void write_video_config(std::ostream& out, const LaunchOptions& options);

// Exit-hotkey bind for the phone remote: its QUIT_GAME chord emits
// KEY_Z + BTN_START on the "MagicDingus Phone Remote" uinput device.
// The virtual pad has no manual joypad binds (autoconfig is disabled),
// but RetroArch's udev keyboard path DOES deliver its KEY_Z — verified
// on hardware 2026-07-22 — so binding exit to "z" is what makes the
// remote able to quit games. (RetroArch's default exit key was Escape;
// kiosk units ship no keyboards, so re-binding costs nothing.)
void write_remote_quit_config(std::ostream& out);

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
