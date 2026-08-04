#pragma once

#include <cstdint>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <vector>

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
    // ACCEPTED DEBT: nothing reads this. It is kept because it is frozen into
    // the [mapping_snapshot] golden format ("drv=udev" on every one of the 33
    // entries), and deleting it would move all 33 goldens -- destabilizing the
    // safety net for the whole mapping layer to retire one unused string.
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

    // Stick clicks. RetroPad L3/R3, which on a DualShock are the two
    // analog-stick buttons. Default "" like l2/r2 rather than a physical
    // index, so a core whose semantic table never binds them (every console
    // but PS1 -- none of the others HAS a stick click) keeps them unassigned
    // in memory; write_player_binds() serializes that state as "nul".
    std::string l3_btn = "";      // L3 (Optional)
    std::string r3_btn = "";      // R3 (Optional)

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

    // Right analog stick, AXIS form. Empty = not emitted. PS-style/modern
    // pads use this representation when emulating N64 C buttons from a real
    // right stick; native N64-style pads use independent digital slots.
    std::string r_x_plus = "";
    std::string r_x_minus = "";
    std::string r_y_plus = "";
    std::string r_y_minus = "";

    // Right analog stick, DIGITAL BUTTON form. Some PS-style/modern pads
    // may need buttons rather than axes to emulate N64 C buttons. Use these
    // INSTEAD OF the axis fields above; native N64-style pads use the
    // independent digital RetroPad slots instead.
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
    // ControllerMapping's literal struct default, which for the EIGHT
    // index-defaulting slots (b=1, y=3, select=10, start=2, a=0, x=4, l=5,
    // r=6) is a physical button INDEX, not a logical control -- so on a
    // captured pad with more buttons than the built-ins, a legacy branch
    // that never bound this RetroPad function can end up binding whatever
    // stray physical button happens to sit at that index. l2 and r2 are NOT
    // part of that gap: they default to "" and so are simply unbound when
    // unset. The d-pad and stick slot classes below were made explicit
    // (routed through the profile) precisely to close this gap for those
    // classes; the button slots were deliberately left alone, because for
    // these the legacy tables genuinely never bound some of them, and
    // normalizing them would change shipped behavior on fielded boxes
    // (would fail the golden snapshot).
    //
    // PERMANENTLY ACCEPTED. Not a deferred to-do: closing it means moving
    // goldens for the two known pads, which is exactly the destabilization
    // the snapshot exists to prevent. What DID need fixing was the adjacent
    // hazard this gap made reachable -- a captured binding's kind being
    // ignored, so an analog control's token landed in a *_btn field and was
    // mis-parsed as a button index. build_mapping() is now kind-aware; see
    // the "THE FIELD FIXES THE FORM" block in controller_mapping.cpp.
    // When true, build_mapping() clears every RetroPad button slot before
    // applying the semantic assignments below. This lets a table express
    // "intentionally unbound" despite ControllerMapping's legacy non-empty
    // defaults, without changing any other core or controller style.
    bool clear_unassigned_buttons = false;
    std::optional<LogicalControl> b, y, select, start, a, x, l, r, l2, r2;
    // Stick clicks (L3/R3). Like l2/r2 these default to "" in
    // ControllerMapping, so they are outside the index-defaulting gap
    // described above: an unset slot here is simply unbound.
    std::optional<LogicalControl> l3, r3;
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
    // Hotkeys. exit_emulator quits the game directly (hold hotkey_enable,
    // press this); bound from PHYSICAL controls in the style preamble so the
    // exit gesture never depends on a per-core RetroPad slot layout.
    std::optional<LogicalControl> hotkey_enable, menu_toggle, exit_emulator;
};

SemanticMapping get_semantic_mapping(ControllerStyle style,
                                     const std::string& core_name);
ControllerMapping build_mapping(const SemanticMapping& sem,
                                const PhysicalProfile& profile);

// Pick the per-core bindings for a detected controller. Unknown controllers
// fall back to the N64 adapter mapping, preserving existing behavior for
// anyone without the PS pad plugged in.
ControllerMapping get_mapping(ControllerType type, const std::string& core_name);

// Resolve the mapping for one physical pad, by VID/PID, in precedence order:
//   1. A captured profile for this exact VID/PID in `store` (operator-
//      corrected -- this deliberately wins even over a builtin match, so a
//      rewired clone pad sharing a known VID/PID can be fixed by capturing).
//   2. A built-in profile for this VID/PID (see get_mapping/match_vid_pid).
//   3. The legacy N64-adapter fallback (get_mapping's UNKNOWN branch).
//
// Declared here rather than in controller_profile.h because it returns
// ControllerMapping by value and needs the complete type; controller_profile.h
// intentionally stays lightweight (no cyclic dependency on this header).
ControllerMapping resolve_mapping_for_pad(
    uint16_t vid, uint16_t pid,
    const std::map<std::string, PhysicalProfile>& store,
    const std::string& core_name);

// The two per-player mappings the launcher emits, resolved independently
// per port.
struct PortMappings {
    ControllerMapping p1;
    ControllerMapping p2;
};

// Resolve player 1 / player 2 mappings from a per-port pad detection list
// (see detect_connected_controllers() in controller_detector.h), preserving
// every currently-fielded box's behavior exactly:
//
//   - Zero pads: both players get get_mapping(fallback_type, core_name) --
//     TODAY'S path, unchanged. `fallback_type` is whatever
//     detect_primary_controller() returned; this is deliberately NOT
//     resolve_mapping_for_pad(0, 0, ...), which could silently diverge if a
//     captured profile ever existed for VID/PID 0000:0000.
//   - One pad: player 2 is an exact mirror of player 1 (legacy behavior for
//     the shipped identical-pads case -- a missing second pad must not
//     produce a worse or different mapping than today).
//   - Two or more pads: player 1 and player 2 each resolve independently
//     from their own port's VID/PID via resolve_mapping_for_pad (captured
//     profile -> builtin -> legacy N64 fallback). Only ports 0 and 1 are
//     consulted; extra pads beyond player 2 are ignored.
//
// Pulled out of retroarch_launcher.cpp (Pi-only, no Mac build) as a pure
// function so this exact port-resolution logic is unit-testable off-device
// given a synthetic pad list, even though the underlying /dev/input scan
// (detect_connected_controllers()) is not.
PortMappings resolve_port_mappings(
    const std::vector<DetectedPad>& pads, ControllerType fallback_type,
    const std::map<std::string, PhysicalProfile>& store,
    const std::string& core_name);

// Emit the RetroArch input_player<N>_r_* lines for the right analog stick.
//
// Writes the AXIS or BUTTON form for PS-style/modern pads that emulate N64 C
// buttons through a right-stick representation — never both. Native
// N64-style pads use independent digital RetroPad slots, so this emits
// nothing for them. Emitting empty values here would UNBIND the stick rather
// than leave it alone, which is why this is a conditional block and not four
// unconditional lines.
//
// `player` is 1-based; the caller invokes this for both players when a
// mapping supplies a right-stick representation.
void write_right_stick_binds(std::ostream& out, const ControllerMapping& map,
                             int player);

void write_hotkey_binds(std::ostream& out,
                        const ControllerMapping& map);

// Emit the full input_player<N>_* RetroArch bind block for one player:
// analog_dpad_mode, the face/shoulder buttons, the d-pad buttons, the stick
// clicks (l3/r3), the left stick axes, the right stick (via
// write_right_stick_binds), then the d-pad axes.
// Extracted out of retroarch_launcher.cpp (Pi-only, no Mac build) so
// this exact block is unit-testable — that launcher used to hand-duplicate
// this block once per player, and the duplication once let the P2 copy drift
// out of sync and ship without the right-stick lines (P2 had no N64 camera
// control in every two-player game). Routing both players through this one
// function makes that class of drift structurally impossible.
//
// Every line here is UNCONDITIONAL. Empty button fields are serialized with
// RetroArch's explicit "nul" sentinel; a literal empty string is parsed as
// physical button 0. The right stick block is the sole exception: it is
// genuinely conditional (see
// write_right_stick_binds), because emitting empty axis/button lines there
// would UNBIND the stick instead of leaving it alone.
//
// Order matches the legacy launcher block line-for-line (the d-pad _btn
// lines sit between select/start and a/x; the d-pad _axis lines come after
// the right-stick block) purely so a diff against the old block stays
// reviewable — RetroArch's config parser does not care about line order.
// The l3/r3 lines are the one thing the legacy block has no counterpart for:
// RetroPad L3/R3 had no plumbing here until PS1 gained its stick clicks, so
// they were placed after r2 to match a stock retroarch.cfg's own ordering
// rather than transcribed from anywhere.
void write_player_binds(std::ostream& out, const ControllerMapping& map,
                        int player);

}  // namespace retroarch
