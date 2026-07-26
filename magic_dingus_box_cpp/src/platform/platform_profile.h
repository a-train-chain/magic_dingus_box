#pragma once

// Platform profile — runtime Raspberry Pi model detection and the
// per-model hardware facts the kiosk needs to branch on (analog audio
// availability, GPIO header chip labels, PulseAudio sink resolution).
//
// MDB originally targeted the Pi 4B only and hardcoded Pi 4 SoC bus
// addresses (audio sinks) and device paths (/dev/gpiochip0). The Pi 5
// moved GPIO to the RP1 southbridge, dropped the 3.5mm analog jack,
// and renamed every PulseAudio platform sink — so the binary now
// detects the board once at startup and resolves hardware dynamically.
//
// Everything in this header is pure logic (no /dev, no forking) except
// detect_platform(), which only reads a file — all unit-testable off-Pi.

#include <optional>
#include <string>
#include <vector>

namespace platform {

enum class PiModel {
    Pi4,
    Pi5,
    Unknown
};

struct PlatformProfile {
    PiModel model = PiModel::Unknown;

    // True when the board has a 3.5mm analog jack (Pi 4B yes, Pi 5 no).
    // Gates the Settings-menu "Headphone" audio output option.
    bool has_analog_audio = false;

    // Acceptable GPIO header chip labels in preference order, matched
    // against gpiod chip info labels instead of hardcoding /dev/gpiochip0
    // (the Pi 5's RP1 chip has shifted between gpiochip4 and gpiochip0
    // across kernel releases).
    std::vector<std::string> gpiochip_labels;

    // How many EV_REL events the rotary encoder emits per physical
    // detent click. InputManager accumulates this many before advancing
    // the UI one step, so a mismatch makes the menu move every OTHER
    // click (too high) or skip items (too low).
    //   Pi 5: 1  — MEASURED on hardware 2026-07-25 (10 clicks -> exactly
    //              10 events, all value=+1).
    //   Pi 4: 2  — the long-shipped default ("accumulate 2 units per
    //              detent"). NOT re-measured on Pi 4 hardware; kept as-is
    //              so fielded boxes cannot regress. If you ever bench a
    //              Pi 4, run the same 10-click count and correct this.
    int rotary_events_per_detent = 2;
};

// Parse the contents of /proc/device-tree/model (may carry a trailing
// NUL from the device-tree blob).
PiModel parse_pi_model(const std::string& device_tree_model);

// Hardware facts for a given model.
PlatformProfile profile_for(PiModel model);

// Read the device-tree model file and build the matching profile.
// Missing/unreadable file yields the Unknown profile (dev machines).
PlatformProfile detect_platform(
    const std::string& model_path = "/proc/device-tree/model");

// --- PulseAudio sink resolution -------------------------------------
// All take the raw output of `pactl list short sinks`, whose lines are
// "<id>\t<name>\t<driver>\t<sample spec>\t<state>".

// First sink whose name identifies an HDMI output.
std::optional<std::string> find_hdmi_sink(const std::string& pactl_short_sinks);

// First sink whose name identifies an analog output (Pi 4 3.5mm
// "mailbox" sink, USB DAC / I2S HAT "analog" sinks). Never HDMI.
std::optional<std::string> find_analog_sink(const std::string& pactl_short_sinks);

enum class SinkChoice {
    Hdmi,
    Analog
};

// Resolve the sink for the requested output, falling back to the other
// kind when the requested one is absent (a stale "headphone" setting on
// a Pi 5 must degrade to HDMI, not silence). Empty when no sink at all.
std::optional<std::string> resolve_sink(const std::string& pactl_short_sinks,
                                        SinkChoice want);

// --- Storage mount detection -----------------------------------------

// True if `mount_point` appears as a mounted filesystem in the given
// /proc/mounts content. Matches the mount-point field exactly so
// "/mnt/ssd" is never satisfied by "/mnt/ssd2".
bool is_path_mounted(const std::string& proc_mounts_content,
                     const std::string& mount_point);

// Convenience: read /proc/mounts and test `mount_point`. The Media
// Browser's movie library lives on an external drive mounted at
// STORAGE_ROOT (/mnt/ssd); when it is absent Radarr cannot start, so the
// kiosk shows "Movies (drive not connected)" instead of silently hiding
// the feature.
bool is_storage_mounted(const std::string& mount_point = "/mnt/ssd",
                        const std::string& proc_mounts_path = "/proc/mounts");

// --- GPIO header chip selection --------------------------------------

// Given the labels of /dev/gpiochip0..N (index == chip number) and the
// profile's acceptable labels in preference order, return the index of
// the header chip, or -1 if none matches.
int pick_gpiochip(const std::vector<std::string>& chip_labels,
                  const std::vector<std::string>& wanted_labels);

} // namespace platform
