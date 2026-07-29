#pragma once
#include <optional>
#include <string>

#include "retroarch/capture_device_caps.h"

// Control surface for the link-seam doubles in ui_test_doubles.cpp.
// See that file for why the doubles exist.
namespace ui_test {

// What InputManager::device_caps() should return for (vid, pid). Any other
// vid/pid — and this one, after fake_unplug() — reports nullopt, which is
// exactly how a real unplugged pad presents (InputManager drops -ENODEV
// nodes, so it stops appearing in the device list).
void fake_set_caps(retroarch::CaptureDeviceCaps caps);
void fake_unplug();
void fake_reset();

bool fake_raw_capture_enabled();
int  fake_raw_capture_calls();     // number of genuine state changes

std::string fake_last_toast();
void fake_clear_toast();

// Collaborators that no wizard path should ever reach. Both counters must
// stay at 0 across the settings-menu tests; a non-zero value means a wizard
// close/cancel path started doing real I/O.
int fake_pairing_calls();     // PairingScreen::regenerate() + ::close()
int fake_settings_saves();    // SettingsPersistence::save_settings()

}  // namespace ui_test
