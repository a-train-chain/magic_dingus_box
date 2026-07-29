#pragma once
#include <cstdint>
#include <map>
#include <string>
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

    bool has(LogicalControl c) const { return controls.count(c) != 0; }
    std::string token(LogicalControl c) const {
        auto it = controls.find(c);
        return it == controls.end() ? std::string() : it->second.token;
    }
    const PhysicalBinding* binding(LogicalControl c) const {
        auto it = controls.find(c);
        return it == controls.end() ? nullptr : &it->second;
    }
};

// Built-in profiles for the two shipped pads. Token values transcribed 1:1
// from the legacy physical tables in controller_mapping.cpp.
//
// Evdev code provenance is NOT uniform across kinds:
//  - BUTTON codes are transcribed from input_manager.cpp's
//    map_button_to_action comment block, which documents the actual live
//    ranges observed for each pad (304+index contiguous for the N64
//    adapter; 288+index BTN_TRIGGER range for the DragonRise).
//  - HAT/AXIS codes (ABS_HAT0X/Y, ABS_X/Y/Z/RZ) are NOT transcribed from
//    anywhere -- map_button_to_action documents buttons only, nothing
//    about axes or hats -- so those are this profile author's own
//    assignment. See the per-profile comments in the .cpp: the N64
//    adapter's are hardware-confirmed; the DragonRise d-pad's are
//    provisional and unverified (input_manager.cpp:27-30,154-166 documents
//    that the same VID/PID can instead report its d-pad via 8-bit
//    ABS_X/Y extremes with no real hat, and no DragonRise pad was
//    available to settle which this shipped pad is).
const PhysicalProfile& builtin_n64_adapter_profile();
const PhysicalProfile& builtin_dragonrise_profile();

std::string vidpid_key(uint16_t vid, uint16_t pid);  // "0079:0006"

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
