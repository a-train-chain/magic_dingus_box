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
// Nothing dispatches on the enum ORDER. It used to: style_of() keyed off
// `c >= N64_A`, and this comment said the order was load-bearing because of
// it. That function had no callers outside its own test and has been removed.
// The one remaining consequence of the order is cosmetic — bindings live in
// std::map<LogicalControl, ...>, so it decides the sequence controls are
// listed in on the wizard's TEST screen. Grouping the two families
// contiguously is for readability, not correctness.
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

// The wizard's prompt order for a style. D-pad first, sticks last, so a
// minimal pad front-loads the controls it actually has.
std::vector<LogicalControl> capture_steps(ControllerStyle style);

// The subset of capture_steps() a session MUST have captured before its
// profile may be persisted. A captured profile unconditionally shadows the
// built-in one for its VID/PID (resolve_mapping_for_pad), so a profile that
// binds almost nothing does not degrade the pad -- it disables it, for both
// players, with the file deliberately immune to OTA updates.
//
// The floor is "the pad can still drive a game and still reach a menu":
// all four d-pad directions, plus the confirm and Start buttons. Everything
// else stays genuinely optional, because pads legitimately differ (no
// shoulders, no second stick, no Select).
std::vector<LogicalControl> required_controls(ControllerStyle style);

// On-screen prompt, e.g. "Press CROSS (bottom face button)" /
// "Move the analog stick UP".
std::string control_prompt(LogicalControl c);

const char* controller_style_key(ControllerStyle s);  // "ps_style" / "n64_style"
std::optional<ControllerStyle> controller_style_from_key(const std::string& key);

}  // namespace retroarch
