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

}  // namespace retroarch
