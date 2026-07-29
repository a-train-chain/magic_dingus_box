#pragma once

#include <iosfwd>
#include <optional>
#include <string>

#include "controller_detector.h"
#include "controller_profile.h"

namespace retroarch {

// Per-core RetroPad bindings for the controllers the kiosk supports.
//
// Split out of retroarch_launcher.cpp so the mappings can be unit-tested on
// a dev machine — the launcher itself pulls in DRM/KMS and process control
// and cannot build off-Pi. (Same reasoning as the controller_detector split
// in v1.4.0.) The launcher owns turning these fields into
// input_player1_* config lines; this file owns deciding what they are.
//
// Values are PHYSICAL joystick button/axis indices for the detected pad,
// and the field names are the RETROPAD function they drive. So
// `b_btn = "2"` reads as "physical button 2 acts as RetroPad B".
struct ControllerMapping {
    // Metadata
    std::string name = "Default";

    // Settings
    std::string analog_dpad_mode = "1"; // 0=Digital, 1=Left Analog
    std::string input_driver = "udev";

    // Core-specific options (e.g., for config file)
    std::string core_option_pad_type = ""; // e.g. "analog" for PS1
    std::string extra_config = ""; // For any other core-specific settings (audio, etc.)

    // Standard Buttons (Map Physical ID -> RetroPad Function)
    std::string b_btn = "1";      // RetroPad B (Bottom Action)
    std::string y_btn = "3";      // RetroPad Y (Left Action)
    std::string select_btn = "10";
    std::string start_btn = "2";

    std::string a_btn = "0";      // RetroPad A (Right Action)
    std::string x_btn = "4";      // RetroPad X (Top Action)

    std::string l_btn = "5";      // L1
    std::string r_btn = "6";      // R1
    std::string l2_btn = "";      // L2 (Optional)
    std::string r2_btn = "";      // R2 (Optional)

    // D-Pad (Usually Hat)
    std::string up_btn = "h0up";
    std::string down_btn = "h0down";
    std::string left_btn = "h0left";
    std::string right_btn = "h0right";

    // Analog Sticks
    std::string l_x_plus = "+0";
    std::string l_x_minus = "-0";
    std::string l_y_plus = "+1";
    std::string l_y_minus = "-1";

    // Right analog stick, AXIS form. Empty = not emitted (every core the
    // kiosk shipped before N64 was single-stick). Needed for N64, whose
    // four C-buttons live on the RetroPad right stick by convention in
    // mupen64plus_next / parallel_n64 — there is nowhere else to put them
    // on a modern pad without stealing the face buttons.
    std::string r_x_plus = "";
    std::string r_x_minus = "";
    std::string r_y_plus = "";
    std::string r_y_minus = "";

    // Right analog stick, DIGITAL BUTTON form. RetroArch lets an analog
    // bind be driven by a plain button, which is the only way to reach the
    // N64 C cluster from a real N64 controller: on that pad the C buttons
    // are four discrete buttons, not a stick, but the core still expects
    // them on the right stick. Use these INSTEAD OF the axis fields above —
    // emitting both would bind the C buttons to axes the adapter does not
    // physically have.
    std::string r_x_plus_btn = "";
    std::string r_x_minus_btn = "";
    std::string r_y_plus_btn = "";
    std::string r_y_minus_btn = "";

    // D-Pad Axis Mappings (Explicit Analog-to-Dpad)
    std::string up_axis = "";
    std::string down_axis = "";
    std::string left_axis = "";
    std::string right_axis = "";

    // Hotkeys
    std::string enable_hotkey_btn = ""; // The "modifier" button (must be held)
    std::string menu_toggle_btn = "";   // The button to press with modifier
    std::string exit_emulator_btn = ""; // Optional exit button
};

// A per-core mapping expressed in LOGICAL controls instead of physical
// button numbers. get_semantic_mapping() owns the per-core decisions
// (which control drives which RetroPad slot); build_mapping() marries a
// SemanticMapping to a PhysicalProfile to produce the ControllerMapping
// the launcher emits. Slots left nullopt keep ControllerMapping's struct
// defaults — several legacy branches rely on exactly that.
struct SemanticMapping {
    std::string name = "Default";
    std::string analog_dpad_mode = "1";
    std::string core_option_pad_type = "";
    std::string extra_config = "";

    // RetroPad button slots. RESIDUAL: an unset slot here still emits
    // ControllerMapping's literal struct default (e.g. select_btn="10"),
    // which is a physical button INDEX, not a logical control -- so on a
    // captured pad with more buttons than the built-ins, a legacy branch
    // that never bound this RetroPad function can end up binding whatever
    // stray physical button happens to sit at that index. The d-pad and
    // stick slot classes below were made explicit (routed through the
    // profile) precisely to close this gap for those classes; the button
    // slots were deliberately left alone, because for these the legacy
    // tables genuinely never bound some of them, and normalizing them
    // would change shipped behavior on fielded boxes (would fail the
    // golden snapshot). Accepted as a known gap for Task 5+ to revisit.
    std::optional<LogicalControl> b, y, select, start, a, x, l, r, l2, r2;
    // RetroPad digital d-pad slots
    std::optional<LogicalControl> up, down, left, right;
    // Main analog stick controls (LSTICK_* or N64_STICK_*)
    std::optional<LogicalControl> stick_up, stick_down, stick_left, stick_right;
    // When true, override the default left-stick binds (ControllerMapping's
    // l_x_plus/l_x_minus/l_y_plus/l_y_minus struct defaults) with the
    // profile's tokens for stick_*. This is NOT a "does this core use the
    // stick" toggle -- leaving it false does not suppress emission, it
    // just leaves those four fields at the struct defaults, which are
    // physical tokens for one specific pad shape. Must be true whenever
    // stick_* is populated, or a captured pad's real stick tokens get
    // silently ignored in favor of those defaults.
    bool left_stick = false;
    bool stick_to_dpad = false; // emit up/down/left/right_axis from stick_*
    // RetroPad right-stick slots (RSTICK_* or N64_C_*); presence gates emission
    std::optional<LogicalControl> r_up, r_down, r_left, r_right;
    // Hotkeys
    std::optional<LogicalControl> hotkey_enable, menu_toggle;
};

SemanticMapping get_semantic_mapping(ControllerStyle style,
                                     const std::string& core_name);
ControllerMapping build_mapping(const SemanticMapping& sem,
                                const PhysicalProfile& profile);

// Pick the per-core bindings for a detected controller. Unknown controllers
// fall back to the N64 adapter mapping, preserving existing behavior for
// anyone without the PS pad plugged in.
ControllerMapping get_mapping(ControllerType type, const std::string& core_name);

// Emit the RetroArch input_player<N>_r_* lines for the right analog stick.
//
// Writes the AXIS form for pads that have a real right stick and the BUTTON
// form for pads (the N64 adapter) whose C cluster is four digital buttons —
// never both, and nothing at all for the single-stick cores. Emitting empty
// values here would UNBIND the stick rather than leave it alone, which is
// why this is a conditional block and not four unconditional lines.
//
// `player` is 1-based and must be emitted for BOTH players on N64: over a
// third of the box's N64 library is two-player, and a P2 with no C buttons
// has no camera control.
void write_right_stick_binds(std::ostream& out, const ControllerMapping& map,
                             int player);

}  // namespace retroarch
