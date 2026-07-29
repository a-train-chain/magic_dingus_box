#include "logical_controls.h"

namespace retroarch {

namespace {
struct Entry { LogicalControl c; const char* key; const char* prompt; };
const Entry kEntries[] = {
    {LogicalControl::DPAD_UP, "dpad_up", "Press D-Pad UP"},
    {LogicalControl::DPAD_DOWN, "dpad_down", "Press D-Pad DOWN"},
    {LogicalControl::DPAD_LEFT, "dpad_left", "Press D-Pad LEFT"},
    {LogicalControl::DPAD_RIGHT, "dpad_right", "Press D-Pad RIGHT"},
    {LogicalControl::CROSS, "cross", "Press CROSS (bottom face button)"},
    {LogicalControl::CIRCLE, "circle", "Press CIRCLE (right face button)"},
    {LogicalControl::SQUARE, "square", "Press SQUARE (left face button)"},
    {LogicalControl::TRIANGLE, "triangle", "Press TRIANGLE (top face button)"},
    {LogicalControl::L1, "l1", "Press L1 (left shoulder)"},
    {LogicalControl::R1, "r1", "Press R1 (right shoulder)"},
    {LogicalControl::L2, "l2", "Press L2 (left trigger)"},
    {LogicalControl::R2, "r2", "Press R2 (right trigger)"},
    {LogicalControl::L3, "l3", "Click the LEFT stick (L3)"},
    {LogicalControl::R3, "r3", "Click the RIGHT stick (R3)"},
    {LogicalControl::SELECT, "select", "Press SELECT"},
    {LogicalControl::START, "start", "Press START"},
    {LogicalControl::LSTICK_UP, "lstick_up", "Move the LEFT stick UP"},
    {LogicalControl::LSTICK_DOWN, "lstick_down", "Move the LEFT stick DOWN"},
    {LogicalControl::LSTICK_LEFT, "lstick_left", "Move the LEFT stick LEFT"},
    {LogicalControl::LSTICK_RIGHT, "lstick_right", "Move the LEFT stick RIGHT"},
    {LogicalControl::RSTICK_UP, "rstick_up", "Move the RIGHT stick UP"},
    {LogicalControl::RSTICK_DOWN, "rstick_down", "Move the RIGHT stick DOWN"},
    {LogicalControl::RSTICK_LEFT, "rstick_left", "Move the RIGHT stick LEFT"},
    {LogicalControl::RSTICK_RIGHT, "rstick_right", "Move the RIGHT stick RIGHT"},
    {LogicalControl::N64_A, "n64_a", "Press A (big blue button)"},
    {LogicalControl::N64_B, "n64_b", "Press B (green button)"},
    {LogicalControl::N64_START, "n64_start", "Press START (center)"},
    {LogicalControl::N64_Z, "n64_z", "Press Z (underside trigger)"},
    {LogicalControl::N64_L, "n64_l", "Press L (left shoulder)"},
    {LogicalControl::N64_R, "n64_r", "Press R (right shoulder)"},
    {LogicalControl::N64_C_UP, "n64_c_up", "Press C-UP (yellow)"},
    {LogicalControl::N64_C_DOWN, "n64_c_down", "Press C-DOWN (yellow)"},
    {LogicalControl::N64_C_LEFT, "n64_c_left", "Press C-LEFT (yellow)"},
    {LogicalControl::N64_C_RIGHT, "n64_c_right", "Press C-RIGHT (yellow)"},
    {LogicalControl::N64_DPAD_UP, "n64_dpad_up", "Press D-Pad UP"},
    {LogicalControl::N64_DPAD_DOWN, "n64_dpad_down", "Press D-Pad DOWN"},
    {LogicalControl::N64_DPAD_LEFT, "n64_dpad_left", "Press D-Pad LEFT"},
    {LogicalControl::N64_DPAD_RIGHT, "n64_dpad_right", "Press D-Pad RIGHT"},
    {LogicalControl::N64_STICK_UP, "n64_stick_up", "Move the analog stick UP"},
    {LogicalControl::N64_STICK_DOWN, "n64_stick_down", "Move the analog stick DOWN"},
    {LogicalControl::N64_STICK_LEFT, "n64_stick_left", "Move the analog stick LEFT"},
    {LogicalControl::N64_STICK_RIGHT, "n64_stick_right", "Move the analog stick RIGHT"},
};
}  // namespace

const char* logical_control_key(LogicalControl c) {
    for (const auto& e : kEntries) if (e.c == c) return e.key;
    return "";
}

std::optional<LogicalControl> logical_control_from_key(const std::string& key) {
    for (const auto& e : kEntries) if (key == e.key) return e.c;
    return std::nullopt;
}

std::string control_prompt(LogicalControl c) {
    for (const auto& e : kEntries) if (e.c == c) return e.prompt;
    return "";
}

ControllerStyle style_of(LogicalControl c) {
    return c >= LogicalControl::N64_A ? ControllerStyle::N64_STYLE
                                      : ControllerStyle::PS_STYLE;
}

std::vector<LogicalControl> capture_steps(ControllerStyle style) {
    using L = LogicalControl;
    if (style == ControllerStyle::N64_STYLE) {
        return {L::N64_DPAD_UP, L::N64_DPAD_DOWN, L::N64_DPAD_LEFT,
                L::N64_DPAD_RIGHT, L::N64_A, L::N64_B, L::N64_START, L::N64_Z,
                L::N64_L, L::N64_R, L::N64_C_UP, L::N64_C_DOWN, L::N64_C_LEFT,
                L::N64_C_RIGHT, L::N64_STICK_UP, L::N64_STICK_DOWN,
                L::N64_STICK_LEFT, L::N64_STICK_RIGHT};
    }
    return {L::DPAD_UP, L::DPAD_DOWN, L::DPAD_LEFT, L::DPAD_RIGHT, L::CROSS,
            L::CIRCLE, L::SQUARE, L::TRIANGLE, L::L1, L::R1, L::L2, L::R2,
            L::SELECT, L::START, L::LSTICK_UP, L::LSTICK_DOWN, L::LSTICK_LEFT,
            L::LSTICK_RIGHT, L::RSTICK_UP, L::RSTICK_DOWN, L::RSTICK_LEFT,
            L::RSTICK_RIGHT, L::L3, L::R3};
}

const char* controller_style_key(ControllerStyle s) {
    return s == ControllerStyle::N64_STYLE ? "n64_style" : "ps_style";
}

std::optional<ControllerStyle> controller_style_from_key(const std::string& key) {
    if (key == "n64_style") return ControllerStyle::N64_STYLE;
    if (key == "ps_style") return ControllerStyle::PS_STYLE;
    return std::nullopt;
}

}  // namespace retroarch
