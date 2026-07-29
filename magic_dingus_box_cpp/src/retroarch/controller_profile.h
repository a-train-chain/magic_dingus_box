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
// from the legacy physical tables in controller_mapping.cpp; evdev codes
// from input_manager.cpp's map_button_to_action comments.
const PhysicalProfile& builtin_n64_adapter_profile();
const PhysicalProfile& builtin_dragonrise_profile();

std::string vidpid_key(uint16_t vid, uint16_t pid);  // "0079:0006"

}  // namespace retroarch
