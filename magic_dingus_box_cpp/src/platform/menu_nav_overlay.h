#pragma once

#include <cstdint>
#include <map>

// Deliberately DEPENDENCY-FREE (std only), and the exact mirror of
// retroarch/capture_device_caps.h: that header exists so both sides of the
// wizard can see CaptureDeviceCaps without dragging a heavy neighbour in, and
// this one does the same for MenuNavOverlay.
//
// MenuNavOverlay is PRODUCED by retroarch::menu_overlay_from_profile() and
// CONSUMED by platform::InputManager, so controller_profile.h has to see it to
// declare that function. It used to get there by including
// platform/input_manager.h, which meant every TU that touched
// controller_profile.h (the launcher, capture_session.h, controller_mapping.h,
// joydev_index.h, four test binaries) also parsed the whole InputManager class
// declaration for a struct it never used. Keeping the shared type in this leaf
// header drops that coupling with no duplication -- input_manager.h includes
// this header, so every existing user of platform::InputAction still sees it
// exactly where it always did.

namespace platform {

enum class InputAction {
    NONE,
    ROTATE,           // Rotate playlist selection (Horizontal/General)
    ROTATE_VERTICAL,  // Rotate vertical (D-pad Up/Down)
    SELECT,           // Select/activate
    NEXT,             // Next track
    PREV,             // Previous track
    SEEK_LEFT,        // Seek backward
    SEEK_RIGHT,       // Seek forward
    PLAY_PAUSE,       // Toggle play/pause
    TOGGLE_LOOP,      // Toggle loop
    QUIT,             // Quit application
    ENTER_SAMPLE_MODE,
    EXIT_SAMPLE_MODE,
    MARKER_ACTION,
    UNDO_MARKER,
    SETTINGS_MENU     // Toggle settings menu
};

// Per-model kiosk-menu mapping learned by the Controller Setup wizard.
//
// ADDITIVE in this precise sense: a code the overlay does NOT list falls
// through to the built-in switch untouched, and an empty overlay (or none at
// all) leaves poll() behaving bit-for-bit as it did before overlays existed.
//
// It does NOT follow that a bad profile is harmless. A code the overlay DOES
// claim deliberately SUPPRESSES the built-in action for that code -- that is
// the whole point (see poll()'s overlay branches, which are mutually exclusive
// with the hardcoded ones so a single event can never fire two actions), but it
// means a mis-captured profile can cost the user a working control. Concretely:
// a profile with nav_x_abs == -1 and seek_abs == 0 turns horizontal stick
// navigation into seek, because ABS_X is then claimed by the seek branch and
// never reaches map_axis_to_action.
//
// The residual risk is bounded by what an overlay is structurally unable to
// claim (see the ceiling comment at poll()'s EV_ABS overlay branches -- the
// d-pad, on every device class, is one of those), and by the recovery surfaces
// that never consult an overlay at all: the d-pad, the rotary encoder, the
// phone remote, and the keyboard. Any one of them can still drive the menu back
// into Controller Setup to re-capture.
struct MenuNavOverlay {
    std::map<uint16_t, InputAction> buttons;  // EV_KEY code -> action
    int nav_x_abs = -1;   // ABS code driving ROTATE (main stick X), -1 = none
    int seek_abs = -1;    // ABS code driving SEEK_LEFT/RIGHT, -1 = none
};

}  // namespace platform
