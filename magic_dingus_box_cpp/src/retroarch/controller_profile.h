#pragma once
#include <cstdint>
#include <map>
#include <string>
// MenuNavOverlay (+ the InputAction enum it maps to) only. This deliberately
// does NOT include platform/input_manager.h: that header declares the whole
// InputManager class, and every TU that reaches this one -- the launcher,
// capture_session.h, controller_mapping.h, joydev_index.h, four test binaries
// -- would parse all of it just to see one struct. Same leaf-header treatment
// as ../retroarch/capture_device_caps.h, for the same reason.
#include "../platform/menu_nav_overlay.h"
#include "logical_controls.h"

namespace retroarch {

struct PhysicalBinding {
    enum class Kind { BUTTON, HAT, AXIS };
    Kind kind = Kind::BUTTON;
    uint16_t code = 0;   // EV_KEY code for BUTTON; ABS_* code for HAT/AXIS
    int direction = 0;   // -1/+1 for HAT and AXIS; 0 for BUTTON
    std::string token;   // RetroArch udev bind token: "5", "h0up", "+2", "-3"
};

struct PhysicalProfile {
    std::string name;
    ControllerStyle style = ControllerStyle::PS_STYLE;
    uint16_t vid = 0, pid = 0;
    std::string captured_at;  // ISO-8601, informational only
    std::map<LogicalControl, PhysicalBinding> controls;

    std::string token(LogicalControl c) const {
        auto it = controls.find(c);
        return it == controls.end() ? std::string() : it->second.token;
    }
    const PhysicalBinding* binding(LogicalControl c) const {
        auto it = controls.find(c);
        return it == controls.end() ? nullptr : &it->second;
    }
};

// Built-in profiles for the two shipped pads. builtin_n64_adapter_profile()
// is the legacy 0e6d:111d profile; its numeric token ordering is not a
// universal physical-label map for distinct N64-style USB VID/PIDs. A
// wizard-captured profile for a matching VID/PID is authoritative and
// overrides built-ins and fallbacks during resolution.
//
// Evdev code provenance is NOT uniform across kinds:
//  - BUTTON codes are transcribed from input_manager.cpp's
//    map_button_to_action comment block, which documents the actual live
//    ranges observed for each pad (304+index contiguous for the N64
//    adapter; 288+index BTN_TRIGGER range for the DragonRise).
//  - HAT/AXIS codes (ABS_HAT0X/Y, ABS_X/Y/Z/RZ) are NOT transcribed from
//    anywhere -- map_button_to_action documents buttons only, nothing
//    about axes or hats -- so those are this profile author's own
//    assignment. See the per-profile comments in the .cpp: the 2563:0575
//    capture confirms N64-style capabilities, evdev codes, and token
//    numbering, but not face labels for the legacy 0e6d:111d profile; the
//    DragonRise d-pad's are
//    provisional and unverified (input_manager.cpp's axis_is_8bit handling
//    documents that pads of this VID/PID can instead report their d-pad via
//    8-bit ABS_X/Y extremes with no real hat, and no DragonRise pad has been
//    available to settle which this shipped pad is).
const PhysicalProfile& builtin_n64_adapter_profile();
const PhysicalProfile& builtin_dragonrise_profile();

// Built-in profiles for the pads sold alongside the box as the recommended
// controllers, compiled in from the operator's wizard captures (the data
// lives in builtin_controller_profiles.inc). Returns nullptr for any other
// VID/PID.
//
// Why these are in the binary rather than only in config/: without them the
// pads work ONLY for as long as config/controller_profiles.json survives, and
// match_vid_pid() recognises NEITHER of them — so the moment that file goes
// (a customer using Settings -> "Reset Controller Setup", a corrupt write, a
// box built from source rather than the image) both pads silently fall back
// to the legacy N64-style mapping, which is simply wrong for the PS-style
// pad. Compiled in, they cannot be lost, they reach fielded boxes over OTA,
// and "reset to the built-in mapping" restores the RIGHT mapping rather than
// a wrong one.
//
// A wizard-captured profile for the same VID/PID still wins, so a customer
// who rewires or recaptures a pad is never overridden by this.
const PhysicalProfile* builtin_profile_for(uint16_t vid, uint16_t pid);

std::string vidpid_key(uint16_t vid, uint16_t pid);  // "0079:0006"

// Derive the kiosk MENU-navigation mapping for a pad from its physical
// profile. This is the pure half of the overlay feature; InputManager
// applies the result (see platform::MenuNavOverlay).
//
// It deliberately mirrors input_manager.cpp's hardcoded
// map_button_to_action / map_axis_to_action semantics rather than inventing
// new ones -- the overlay's job is to say "on THIS pad, that same kiosk
// action lives at this other code", not to change what the kiosk does.
//
// Anything the profile doesn't bind, or binds with the wrong kind (a face
// button captured as an axis, a stick captured as a button/hat), is simply
// left unclaimed: InputManager then falls through to its built-in switch for
// that code, so a partial or nonsense profile degrades to stock behavior
// rather than to a broken menu.
platform::MenuNavOverlay menu_overlay_from_profile(const PhysicalProfile& p);

// Serialize captured profiles to/from the on-disk JSON schema (see
// config::get_controller_profiles_file()). profiles_to_json writes each
// entry under the CALLER's map key, as-is: std::map already guarantees
// those keys are unique, so writing them straight through can never make
// two distinct entries collide on disk -- unlike deriving the on-disk key
// from each profile's own vid/pid fields, which two entries can easily
// share (e.g. both cloned from a builtin template without overriding
// vid/pid). profiles_from_json instead keys its result map by the
// CANONICAL vidpid_key(vid, pid) (lowercase "vvvv:pppp") parsed from the
// JSON object's key text, ignoring that raw text once parsed -- so an
// uppercase-hex or otherwise non-canonical on-disk key still resolves
// correctly by resolve_mapping_for_pad's lookup, and a record's vid/pid
// always agree with its key immediately after any load (the inconsistency
// a mismatched map key and profile vid/pid can only exist in memory, before
// a save). profiles_from_json NEVER throws and never propagates a parse
// failure -- malformed text, a non-object "profiles" node, a bad map key,
// an unknown style, an unknown control key, or an unknown binding kind all
// degrade to skipping that piece (or an empty store), so a corrupt profiles
// file can never prevent the kiosk from booting.
std::string profiles_to_json(const std::map<std::string, PhysicalProfile>& profiles);
std::map<std::string, PhysicalProfile> profiles_from_json(const std::string& text);

// Load/save the profile store at config::get_controller_profiles_file()
// (overridable via MAGIC_CONTROLLER_PROFILES_FILE for tests). A missing
// file loads as an empty store, not an error. save_profile_store writes to
// a temp file and renames it into place so a crash mid-write can never
// leave a half-written profiles file on disk.
std::map<std::string, PhysicalProfile> load_profile_store();
bool save_profile_store(const std::map<std::string, PhysicalProfile>& profiles);

}  // namespace retroarch
