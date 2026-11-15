#pragma once

// Automatic button layouts for modern controllers.
//
// Every popular pad today -- PlayStation 4/5, Xbox, Switch Pro, 8BitDo in
// X-input or Switch mode -- has the DualShock's physical shape: a diamond of
// four face buttons, two shoulders and two triggers, Select/Start, two
// clickable sticks and a d-pad. So they all use the PS-STYLE semantic tables
// (controller_mapping.cpp), matched by POSITION: the bottom face button is
// Cross whatever is printed on it (A on Xbox, B on Nintendo), which is also
// the convention RetroArch's own autoconfig uses.
//
// What differs per pad is which evdev code each position reports, and that
// is fixed by the KERNEL DRIVER, not the pad:
//
//   driver                  face codes (bottom/right/top/left)  triggers
//   playstation, sony       0x130 / 0x131 / 0x133 / 0x134      BTN_TL2/TR2
//   nintendo                0x130 / 0x131 / 0x133 / 0x134      BTN_TL2/TR2
//   xpad, xpadneo           0x130 / 0x131 / 0x134 / 0x133      ABS_Z / ABS_RZ
//   microsoft or VID 045e   0x130 / 0x131 / 0x134 / 0x133      ABS_BRAKE / ABS_GAS
//     (Xbox over Bluetooth LE; right stick on ABS_Z/ABS_RZ)
//
// (xpad reports Xbox X -- the LEFT button -- as BTN_X, which the kernel
// headers alias to BTN_NORTH; hid-playstation and hid-nintendo report true
// positions.) Generic HID pads ("hid-generic" with any other VID) number
// their buttons in report order, which says nothing about position, so they
// get NO automatic layout and are left to Controller Setup.
//
// A known driver is still not enough: the pad must also report the full
// modern button set (four face buttons, L1/R1, Select, Start and a d-pad).
// That keeps single Joy-Cons, NSO N64 pads and old-firmware Xbox One S pads
// on the wizard path.
//
// Precedence everywhere: a Controller Setup capture for the VID/PID wins,
// then a built-in profile, then this, then the legacy N64 fallback.
//
// HARDWARE STATUS: tokens are cross-checked against RetroArch's own udev
// autoconfig for DualSense and Xbox 360 (xpad). The Xbox-over-BLE and
// Switch Pro rows follow the kernel drivers' documented mappings and have
// not been run on a box yet.

#include <optional>

#include "../platform/menu_nav_overlay.h"
#include "capture_device_caps.h"
#include "controller_detector.h"
#include "controller_profile.h"

namespace retroarch {

// The automatic PS-style profile for this pad, or nullopt when its driver
// does not guarantee positional buttons or it lacks the modern button set.
// vid/pid/name come from the caps; captured_at stays empty.
std::optional<PhysicalProfile> standard_profile(const CaptureDeviceCaps& caps);
std::optional<PhysicalProfile> standard_profile(const DetectedPad& pad);

// The kiosk menu buttons for an automatic profile: menu_overlay_from_profile()
// plus explicit NONE claims for the face/shoulder codes the menus do not use
// (Square, Select, L2/R2, L3/R3), so the built-in table -- written for the
// N64 adapter, where BTN_WEST is L = "previous" -- cannot fire on them.
platform::MenuNavOverlay standard_menu_overlay(const PhysicalProfile& profile);

}  // namespace retroarch
