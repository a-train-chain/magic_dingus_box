#pragma once
#include <optional>
#include <string>
#include <vector>

namespace retroarch {

enum class ControllerStyle { PS_STYLE, N64_STYLE };

// One value per physical control the wizard can ask about. The PS and N64
// vocabularies are deliberately distinct (no punning): an N64 pad's A is not
// "the same control" as a PS pad's Cross even if a core treats them alike.
//
// ORDER IS LOAD-BEARING: style_of() below dispatches on enum ordering
// (c >= LogicalControl::N64_A means N64_STYLE). Do not reorder these
// enumerators or insert new ones before N64_A without also revisiting
// style_of().
enum class LogicalControl {
    // PS-style vocabulary
    DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,
    CROSS, CIRCLE, SQUARE, TRIANGLE,
    L1, R1, L2, R2, L3, R3,
    SELECT, START,
    LSTICK_UP, LSTICK_DOWN, LSTICK_LEFT, LSTICK_RIGHT,
    RSTICK_UP, RSTICK_DOWN, RSTICK_LEFT, RSTICK_RIGHT,
    // N64 vocabulary
    N64_A, N64_B, N64_START, N64_Z, N64_L, N64_R,
    N64_C_UP, N64_C_DOWN, N64_C_LEFT, N64_C_RIGHT,
    N64_DPAD_UP, N64_DPAD_DOWN, N64_DPAD_LEFT, N64_DPAD_RIGHT,
    N64_STICK_UP, N64_STICK_DOWN, N64_STICK_LEFT, N64_STICK_RIGHT,
};

// Stable snake_case key used in controller_profiles.json ("cross", "n64_a").
const char* logical_control_key(LogicalControl c);
std::optional<LogicalControl> logical_control_from_key(const std::string& key);

ControllerStyle style_of(LogicalControl c);

// The wizard's prompt order for a style. D-pad first, sticks last, so a
// minimal pad front-loads the controls it actually has.
std::vector<LogicalControl> capture_steps(ControllerStyle style);

// On-screen prompt, e.g. "Press CROSS (bottom face button)" /
// "Move the analog stick UP".
std::string control_prompt(LogicalControl c);

const char* controller_style_key(ControllerStyle s);  // "ps_style" / "n64_style"
std::optional<ControllerStyle> controller_style_from_key(const std::string& key);

}  // namespace retroarch
