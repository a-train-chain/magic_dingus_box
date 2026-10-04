#pragma once

#include <chrono>
#include <functional>
#include <optional>
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

enum class SessionWatchdog { Arm, Ping, Disarm };

struct LaunchOptions {
    app::DisplayMode display_mode = app::DisplayMode::CRT_NATIVE;
    std::string bezel_file;
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

    // systemd watchdog control for the supervised play phase. The launcher
    // calls Arm right after RetroArch is spawned, Ping on every supervision
    // tick (~50 ms) and Disarm when the session is over, before the kiosk's
    // DRM/input restore runs — that restore and the pre-launch teardown stay
    // unwatched, as before. Empty = no watchdog (tests, dev machines).
    std::function<void(SessionWatchdog)> watchdog;
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

// Where the distro's libretro cores live (Debian aarch64 packaging). The
// launcher searches here FIRST, then the user core dir
// (config::retroarch::get_cores_dir()) -- the same order everywhere.
inline constexpr const char* kSystemLibretroDir =
    "/usr/lib/aarch64-linux-gnu/libretro";

// Canonical RetroArch core name: playlist/auto-resolved names may omit the
// "_libretro" suffix ("flycast" -> "flycast_libretro"); RetroArch's -L and
// the .so filename both need it.
std::string libretro_core_name(const std::string& core_name);

// The directory holding <core>_libretro.so, searching system_dir then
// user_dir; nullopt when neither has it. Called by the kiosk BEFORE it
// tears down video/DRM/input for a launch: a missing core used to be
// discovered only after the display handoff, when RetroArch failed with
// the kiosk already dark.
std::optional<std::string> resolve_core_dir(const std::string& core_name,
                                            const std::string& system_dir,
                                            const std::string& user_dir);

// Rotate the per-session launcher log: move `log_path` to `log_path.1`
// (replacing any previous .1) so each launch starts a fresh file and the
// log is bounded to two sessions. A missing log is not an error. Returns
// false only when an existing log could not be moved.
bool rotate_launcher_log(const std::string& log_path);

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

// Make the RetroArch menu unreachable (owner decision 2026-08-03: no RA
// menu on a kiosk -- see CLAUDE.md "Exit a game"). Clears the gamepad menu
// chord (it was enum 3 = L1+R1+Start+Select, which a player can hit by
// accident and which leads to settings, the core downloader and "Quit"
// without auto-save guarantees), unbinds the keyboard toggle, and hides the
// online/core updater. Written into the --appendconfig override so no
// RetroArch default can clobber it. Nothing on the exit path needs the
// menu: games quit through input_exit_emulator_btn/_axis (the physical
// Z+Start / Select+Start gesture, write_hotkey_binds) and the phone
// remote's KEY_Z (write_remote_quit_config).
void write_menu_disabled_config(std::ostream& out);

// Command-line device overrides for the core. Debian RetroArch loads
// input_libretro_device_pN from remap files rather than the global config,
// so supported PS1 cores select DualShock through the explicit CLI path.
std::vector<std::string> core_input_device_args(
    const std::string& core_name);

// Pick the ALSA device for HDMI game audio from `aplay -L` output.
// Selects by card NAME, never card number: on Pi 4 vc4hdmi0 is ALSA
// card 1 (behind the Headphones card) but on Pi 5 it is card 0, so the
// old hardcoded "plughw:1,0" silently routed Pi 5 game audio to the
// wrong port. `monitor_cards` lists the vc4hdmiN cards whose ELD shows
// a TV that takes audio (see eld_reports_monitor): the TV may be on
// EITHER physical port, so a card with a monitor wins over one without.
// With none known (TV off, dev box) it falls back to name order —
// vc4hdmi0, vc4hdmi1, then the legacy default.
std::string pick_hdmi_alsa_device(const std::string& aplay_L_output,
                                  const std::vector<std::string>& monitor_cards = {});

// True when /proc/asound/<card>/eld#0 text describes an attached sink
// that accepts audio. vc4 reports this as sad_count > 0; HDA-style
// drivers print monitor_present / eld_valid instead.
bool eld_reports_monitor(const std::string& eld_text);

// The ALSA device a game's audio goes to. audio_output: 0=AUTO, 1=HDMI,
// 2=HEADPHONE (app::AudioOutput). `eld_readable_cards` lists the vc4hdmiN
// cards whose ELD file could be read; `monitor_cards` those whose ELD
// reports an audio-capable sink (eld_reports_monitor).
//
// AUTO mirrors PulseAudio's own choice, which the menus already follow:
// when ELD is readable and NO HDMI port has an audio-capable sink (a DVI
// monitor, an HDMI->composite CRT converter, a TV that takes no audio),
// PulseAudio falls back to the analog jack — so a game must too, when the
// board has one (`sysdefault:CARD=Headphones` listed; Pi 4B only — a Pi 5
// has no Headphones card and stays on HDMI). With no ELD readable at all
// the HDMI pick is unchanged: no evidence is not "no audio sink".
std::string pick_game_alsa_device(int audio_output,
                                  const std::string& aplay_L_output,
                                  const std::vector<std::string>& eld_readable_cards,
                                  const std::vector<std::string>& monitor_cards);
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

// Written with RetroArch's pid once it opens the KMS node, removed when the
// session ends. Nothing in the kiosk reads it any more (readiness is
// detected in-process), but scripts/emulator_smoke_test.py does.
inline constexpr const char* kReadyMarkerPath = "/tmp/retroarch_mdb.ready";

// The bash launcher used to append to this tmpfs file on every launch and
// never rotated it. It is no longer written; the launcher deletes any
// leftover copy. Everything now goes to the rotated
// config::retroarch::get_launcher_log().
inline constexpr const char* kLegacyTmpLauncherLog = "/tmp/retroarch_launcher.log";

// True when an existing core .opt file would shadow the options
// write_core_options() emits: some line begins with `prefix` (the
// `grep -l '^<prefix>'` the old launcher script ran). An empty prefix never
// matches — a core we write no options for must not lose its .opt.
bool opt_file_shadows_options(const std::string& opt_contents,
                              const std::string& prefix);

}  // namespace retroarch
