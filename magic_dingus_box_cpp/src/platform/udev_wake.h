#pragma once

// "Wake the controllers": `sudo udevadm trigger --action=change` over the
// joystick and event nodes, run before the kiosk opens input devices and
// around every RetroArch handover. Selection logic is pure and tested on
// the Mac (tests/platform/test_udev_wake.cpp).
//
// WHY explicit device paths instead of `--sysname-match=js*`: the match
// also hit the phone remote's uinput pad, which lives under
// /sys/devices/virtual/ — its trigger failed with "Permission denied" on
// every wake (harmless, but it buried real errors in the journal). A
// virtual device has no hardware to wake, so it is skipped; every real pad
// (USB, Bluetooth, GPIO rotary) is triggered exactly as before, in the
// same js-then-event order.

#include <string>
#include <vector>

namespace platform::udev {

// True for a RESOLVED sysfs path under /sys/devices/virtual/ (uinput,
// loopback, ...). Pure string check.
bool is_virtual_device_path(const std::string& resolved_sysfs_path);

// Entries of `class_dir` (normally /sys/class/input) whose name starts
// with `prefix` ("js", "event"), returned as class_dir/<name> paths for
// the ones whose symlink target is NOT a virtual device. Sorted. Empty
// when the directory is missing (the Mac).
std::vector<std::string> real_input_nodes(const std::string& class_dir,
                                          const std::string& prefix);

// argv for one trigger over `paths`, or empty when there is nothing to
// trigger — an argument-less `udevadm trigger` would re-trigger EVERY
// device on the system, so an empty selection must run nothing.
std::vector<std::string> trigger_argv(const std::vector<std::string>& paths);

// Trigger the real js* nodes, then the real event* nodes (each bounded by
// a timeout; failures are logged and otherwise ignored, as before).
void wake_input_devices();

}  // namespace platform::udev
