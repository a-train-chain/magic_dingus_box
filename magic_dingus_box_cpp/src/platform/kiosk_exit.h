#pragma once

// Kiosk process exit codes that other components depend on.
//
// kExitNoDisplay: display initialization found DRM working but NO
// connected connector (no TV plugged in, or the TV's HDMI is asleep / on
// another input). This is not a broken build, and two consumers need to
// tell the difference:
//
//   - update.sh starts the freshly installed kiosk to verify an OTA. It
//     treats exit 69 as "the new binary loads and runs; there is just no
//     screen" and keeps the update. Before this code existed the kiosk
//     exited 1 here and every OTA on a headless box was rolled back. The
//     value is duplicated there as KIOSK_EXIT_NO_DISPLAY — change both.
//   - systemd: magic-dingus-box-cpp.service is Restart=on-failure with
//     StartLimitIntervalSec=0 and deliberately does NOT list 69 in
//     RestartPreventExitStatus=, so the kiosk keeps retrying every
//     RestartSec and comes up by itself once a TV is connected.
//
// 69 is EX_UNAVAILABLE from <sysexits.h> ("a service is unavailable").

namespace platform {

inline constexpr int kExitNoDisplay = 69;

// Why DrmDisplay::initialize() returned false. None means it has not
// failed (or the failure was never classified).
enum class DisplayInitFailure {
    None,
    NoDrmDevice,         // no mode-setting DRM device could be opened
    NoDrmMaster,         // device found but DRM master unavailable
    NoConnectedDisplay,  // DRM is fine; nothing is plugged in / awake
    NoCrtc,              // connector found but no usable CRTC
    Other,
};

// The process exit code main() returns for a display-init failure. Only a
// positively classified "no connected display" gets the special code; an
// unclassified failure stays a plain 1 so a broken build can never pass
// itself off as a missing TV.
constexpr int exit_code_for_display_init_failure(DisplayInitFailure f) {
    return f == DisplayInitFailure::NoConnectedDisplay ? kExitNoDisplay : 1;
}

}  // namespace platform
