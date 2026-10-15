#pragma once
// WHICH RETROPAD SLOTS EACH CORE ACTUALLY READS.
//
// This is the data behind the coverage invariant. It is declared per CORE, not
// per console, because the RetroPad->console mapping is a property of the core
// and its options: with mupen64plus-alt-map on, mupen reads RetroPad SELECT as
// the N64's L shoulder, so "which slots matter" cannot be derived from the
// console alone.
//
// Each core lists its NEEDS: one per thing a game can ask for. A need names
// the RetroPad slots that deliver it, and is met when ANY of them is
// reachable. Almost every need has exactly one slot; the N64 C-buttons have
// two (a digital slot and a right-stick direction), because alt-map gives
// each C-button its own digital slot while the core still reads the right
// stick as the C cluster.
//
// An incorrect declaration makes the invariant either toothless (slot omitted
// that the core does read) or a source of false failures (slot listed that it
// does not). When in doubt, prefer omitting a slot and note it, rather than
// asserting a binding that no game can use.
#include <string>
#include <vector>

#include "retroarch/controller_mapping.h"

namespace mdb_cov {

enum class Slot {
    B, A, Y, X, L, R, L2, R2, L3, R3, SELECT, START,
    DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,
    LSTICK,   // the whole left stick: all four directions
    RSTICK,   // the whole right stick: all four directions
    RSTICK_UP, RSTICK_DOWN, RSTICK_LEFT, RSTICK_RIGHT,
};

inline const char* slot_name(Slot s) {
    switch (s) {
        case Slot::B: return "B"; case Slot::A: return "A";
        case Slot::Y: return "Y"; case Slot::X: return "X";
        case Slot::L: return "L"; case Slot::R: return "R";
        case Slot::L2: return "L2"; case Slot::R2: return "R2";
        case Slot::L3: return "L3"; case Slot::R3: return "R3";
        case Slot::SELECT: return "SELECT"; case Slot::START: return "START";
        case Slot::DPAD_UP: return "DPAD_UP"; case Slot::DPAD_DOWN: return "DPAD_DOWN";
        case Slot::DPAD_LEFT: return "DPAD_LEFT"; case Slot::DPAD_RIGHT: return "DPAD_RIGHT";
        case Slot::LSTICK: return "LSTICK"; case Slot::RSTICK: return "RSTICK";
        case Slot::RSTICK_UP: return "RSTICK_UP"; case Slot::RSTICK_DOWN: return "RSTICK_DOWN";
        case Slot::RSTICK_LEFT: return "RSTICK_LEFT"; case Slot::RSTICK_RIGHT: return "RSTICK_RIGHT";
    }
    return "?";
}

// One pad x core, resolved through the same path the launcher uses
// (get_mapping), plus the semantic table it was built from.
struct Resolved {
    retroarch::SemanticMapping sem;
    retroarch::ControllerMapping m;
};

inline Resolved resolve(retroarch::ControllerType pad, const std::string& core) {
    // Mirrors get_mapping's dispatch: only the PS pad is PS-style.
    const auto style = pad == retroarch::ControllerType::PS_STYLE_DRAGONRISE
                           ? retroarch::ControllerStyle::PS_STYLE
                           : retroarch::ControllerStyle::N64_STYLE;
    return {retroarch::get_semantic_mapping(style, core),
            retroarch::get_mapping(pad, core)};
}

// The emitted bind token(s) behind one slot, or "" when nothing reaches it.
//
// A non-empty token alone is NOT proof of a binding: ControllerMapping's
// face-button fields default to physical button indices, and a table that
// never assigns a slot (and does not set clear_unassigned_buttons) still
// emits that default -- the "ghost default" residual documented on
// SemanticMapping. So a button or d-pad slot counts only when the semantic
// table assigned it deliberately AND the pad resolved it to a real token.
// Stick slots need no such guard: build_mapping emits them only from
// deliberate assignments.
inline std::string slot_value(const Resolved& r, Slot s) {
    const auto& sem = r.sem;
    const auto& m = r.m;
    auto deliberate = [](const auto& slot, const std::string& v) {
        return slot ? v : std::string();
    };
    auto either = [](const std::string& axis, const std::string& btn) {
        return axis.empty() ? btn : axis;
    };
    switch (s) {
        case Slot::B: return deliberate(sem.b, m.b_btn);
        case Slot::A: return deliberate(sem.a, m.a_btn);
        case Slot::Y: return deliberate(sem.y, m.y_btn);
        case Slot::X: return deliberate(sem.x, m.x_btn);
        case Slot::L: return deliberate(sem.l, m.l_btn);
        case Slot::R: return deliberate(sem.r, m.r_btn);
        case Slot::L2: return deliberate(sem.l2, m.l2_btn);
        case Slot::R2: return deliberate(sem.r2, m.r2_btn);
        case Slot::L3: return deliberate(sem.l3, m.l3_btn);
        case Slot::R3: return deliberate(sem.r3, m.r3_btn);
        case Slot::SELECT: return deliberate(sem.select, m.select_btn);
        case Slot::START: return deliberate(sem.start, m.start_btn);
        // A d-pad direction lands in _btn (hat or button) or _axis (a pad
        // that reports its d-pad on ABS_X/Y) -- either one is a real bind.
        case Slot::DPAD_UP: return deliberate(sem.up, either(m.up_axis, m.up_btn));
        case Slot::DPAD_DOWN: return deliberate(sem.down, either(m.down_axis, m.down_btn));
        case Slot::DPAD_LEFT: return deliberate(sem.left, either(m.left_axis, m.left_btn));
        case Slot::DPAD_RIGHT: return deliberate(sem.right, either(m.right_axis, m.right_btn));
        case Slot::LSTICK:
            if (!sem.left_stick || m.l_x_plus.empty() || m.l_x_minus.empty() ||
                m.l_y_plus.empty() || m.l_y_minus.empty()) return "";
            return m.l_x_plus;
        case Slot::RSTICK_UP: return either(m.r_y_minus, m.r_y_minus_btn);
        case Slot::RSTICK_DOWN: return either(m.r_y_plus, m.r_y_plus_btn);
        case Slot::RSTICK_LEFT: return either(m.r_x_minus, m.r_x_minus_btn);
        case Slot::RSTICK_RIGHT: return either(m.r_x_plus, m.r_x_plus_btn);
        case Slot::RSTICK:
            for (Slot d : {Slot::RSTICK_UP, Slot::RSTICK_DOWN,
                           Slot::RSTICK_LEFT, Slot::RSTICK_RIGHT}) {
                if (slot_value(r, d).empty()) return "";
            }
            return slot_value(r, Slot::RSTICK_RIGHT);
    }
    return "";
}

// The RetroPad slots that deliver one thing a game can ask for; met when any
// one of them is reachable.
using Need = std::vector<Slot>;

struct CoreSlots {
    const char* core;              // substring the mapping dispatch matches on
    std::vector<Need> needs;       // what this core reads, one entry per need
};

// Every core the box ships, with what that core reads.
//
// Deliberate omissions:
//  - Genesis MODE: toggles 3/6-button compatibility, no game needs it pressed.
//  - PC Engine / NES turbo (X, Y): a kiosk convenience, not console hardware,
//    so not required for coverage even though both tables bind them.
inline const std::vector<CoreSlots>& kCoreSlots() {
    static const std::vector<CoreSlots> v = [] {
        auto with_dpad = [](std::vector<Need> needs) {
            for (Slot d : {Slot::DPAD_UP, Slot::DPAD_DOWN, Slot::DPAD_LEFT, Slot::DPAD_RIGHT})
                needs.push_back({d});
            return needs;
        };
        // N64 under mupen64plus-alt-map = "True" (launch_contract.cpp), the
        // RetroPad->N64 table in docs/superpowers/specs/
        // 2026-07-30-native-n64-controller-core-mapping-design.md: each C-button
        // has its own digital slot, and the right stick still drives the C
        // cluster (2026-07-30-universal-ps-style-n64-controller-mapping-design.md
        // -- the PS pad's C-Left lives only there). Both cores share the option
        // contract, so they share the table.
        const std::vector<Need> n64 = with_dpad({
            {Slot::B},                        // A
            {Slot::Y},                        // B
            {Slot::L2},                       // Z
            {Slot::R2},                       // R shoulder
            {Slot::SELECT},                   // L shoulder
            {Slot::START},
            {Slot::X, Slot::RSTICK_UP},       // C-Up
            {Slot::A, Slot::RSTICK_DOWN},     // C-Down
            {Slot::L, Slot::RSTICK_LEFT},     // C-Left
            {Slot::R, Slot::RSTICK_RIGHT},    // C-Right
            {Slot::LSTICK},                   // control stick
        });
        return std::vector<CoreSlots>{
            {"nestopia", with_dpad({{Slot::B}, {Slot::A}, {Slot::SELECT}, {Slot::START}})},
            {"snes9x", with_dpad({{Slot::B}, {Slot::A}, {Slot::Y}, {Slot::X},
                                  {Slot::L}, {Slot::R}, {Slot::SELECT}, {Slot::START}})},
            // Genesis 6-button: RetroPad Y/B/A = A/B/C, L/X/R = X/Y/Z.
            {"genesis_plus_gx", with_dpad({{Slot::B}, {Slot::A}, {Slot::Y}, {Slot::X},
                                           {Slot::L}, {Slot::R}, {Slot::START}})},
            {"pcsx_rearmed", with_dpad({{Slot::B}, {Slot::A}, {Slot::Y}, {Slot::X},
                                        {Slot::L}, {Slot::R}, {Slot::L2}, {Slot::R2},
                                        {Slot::L3}, {Slot::R3}, {Slot::SELECT}, {Slot::START},
                                        {Slot::LSTICK}, {Slot::RSTICK}})},
            {"mednafen_pce_fast", with_dpad({{Slot::B}, {Slot::A}, {Slot::SELECT}, {Slot::START}})},
            {"prosystem", with_dpad({{Slot::B}, {Slot::A}, {Slot::SELECT}, {Slot::START}})},
            // FBNeo 6-button + coin on SELECT.
            {"fbneo", with_dpad({{Slot::B}, {Slot::A}, {Slot::Y}, {Slot::X},
                                 {Slot::L}, {Slot::R}, {Slot::SELECT}, {Slot::START}})},
            {"mupen64plus", n64},
            {"parallel_n64", n64},
            // Dreamcast: 4 face buttons, Start, two ANALOG triggers on L2/R2,
            // one analog stick. No second stick, no stick clicks.
            {"flycast", with_dpad({{Slot::B}, {Slot::A}, {Slot::Y}, {Slot::X},
                                   {Slot::L2}, {Slot::R2}, {Slot::START}, {Slot::LSTICK}})},
        };
    }();
    return v;
}

// A slot a pad does not deliberately reach, with the reason. Enforced in BOTH
// directions: if one of these turns out to be bound, the test fails too, so
// the list cannot decay into a record of things that used to be broken.
//
// Three kinds of entry, named at the start of each reason:
//   - a hardware limit: the pad has nothing left to give;
//   - OPEN: a fact about the hardware nobody has checked yet;
//   - KNOWN GAP: a fixable table omission, recorded so CI stays green until
//     it is fixed. Fixing one fails the second test case until its entry is
//     deleted here.
struct Exception {
    retroarch::ControllerType pad;
    const char* core;
    Slot slot;
    const char* reason;
};

inline const std::vector<Exception>& kExceptions() {
    static const std::vector<Exception> v = {
        {retroarch::ControllerType::N64_ADAPTER, "pcsx_rearmed", Slot::RSTICK,
         "The N64 pad has exactly ten buttons and PS1 needs ten (4 face, L1/R1/"
         "L2/R2, Select, Start). Nothing is left for a second stick. Freeing the "
         "C-cluster for a digital right stick would cost R2, and far more PS1 "
         "games use R2 than use the right stick."},
        {retroarch::ControllerType::N64_ADAPTER, "pcsx_rearmed", Slot::L3,
         "The N64 pad has no stick clicks and no spare button to stand in for "
         "one; see the RSTICK reason."},
        {retroarch::ControllerType::N64_ADAPTER, "pcsx_rearmed", Slot::R3,
         "The N64 pad has no stick clicks and no spare button to stand in for "
         "one; see the RSTICK reason."},
        // OPEN QUESTION (2026-10-14), not a settled limitation. The PS1 table
        // asks for L3/R3, but builtin_dragonrise_profile() lists no stick
        // clicks, so they are unbound unless the pad was captured with the
        // Controller Setup wizard. If the physical pad has stick clicks, add
        // them to the built-in profile (regenerating the mapping snapshot
        // golden) and delete these two entries -- the second test case
        // will insist.
        {retroarch::ControllerType::PS_STYLE_DRAGONRISE, "pcsx_rearmed", Slot::L3,
         "OPEN: the built-in DragonRise profile has no L3; unverified whether "
         "the physical pad has stick clicks."},
        {retroarch::ControllerType::PS_STYLE_DRAGONRISE, "pcsx_rearmed", Slot::R3,
         "OPEN: the built-in DragonRise profile has no R3; unverified whether "
         "the physical pad has stick clicks."},
    };
    return v;
}

}  // namespace mdb_cov
