#include "controller_mapping.h"

#include <ostream>
#include <string>

namespace retroarch {

namespace {

using L = LogicalControl;

// Semantic, per-core RetroPad mapping for the USB N64-style adapter
// (SWITCH CO.,LTD. clone, 0e6d:111d). Expressed in LogicalControl values —
// build_mapping() resolves each slot to a physical token via
// builtin_n64_adapter_profile(); see controller_profile.cpp for that
// adapter's physical button IDs and hardware evidence.
//
// Default N64-style hotkey combo: Z trigger + Start.
// PS1 is the exception: Z is L2 there, so that branch relies on RetroArch's
// global L1 + R1 + Start + Select gamepad combination instead.
SemanticMapping semantic_n64_style(const std::string& core) {
    SemanticMapping s;
    auto stick = [&] {
        s.stick_up = L::N64_STICK_UP; s.stick_down = L::N64_STICK_DOWN;
        s.stick_left = L::N64_STICK_LEFT; s.stick_right = L::N64_STICK_RIGHT;
    };
    auto hotkeys = [&] { s.hotkey_enable = L::N64_Z; s.menu_toggle = L::N64_START; };
    auto dpad = [&] {
        s.up = L::N64_DPAD_UP; s.down = L::N64_DPAD_DOWN;
        s.left = L::N64_DPAD_LEFT; s.right = L::N64_DPAD_RIGHT;
    };

    if (core.find("nestopia") != std::string::npos || core.find("fceumm") != std::string::npos) {
        s.name = "NES (N64 Controller)"; s.analog_dpad_mode = "0";
        s.b = L::N64_B;       // NES B (Run)
        s.a = L::N64_A;       // NES A (Jump)
        s.select = L::N64_C_UP;
        s.start = L::N64_START;
        // Turbo buttons
        s.x = L::N64_C_DOWN;  // Turbo A
        s.y = L::N64_C_LEFT;  // Turbo B
        dpad();
        stick(); s.left_stick = true; s.stick_to_dpad = true;  // stick -> d-pad, so it works for Mario
        hotkeys();
        s.extra_config = "nestopia_audio_vol_sq1 = \"100\"\n"
                         "nestopia_audio_vol_sq2 = \"100\"\n"
                         "nestopia_audio_vol_tri = \"100\"\n"
                         "nestopia_audio_vol_noise = \"100\"\n"
                         "nestopia_audio_vol_dpcm = \"100\"\n";

    } else if (core.find("pcsx") != std::string::npos ||
               core.find("beetle_psx") != std::string::npos ||
               core.find("swanstation") != std::string::npos) {
        // ---- Sony PlayStation on an N64 pad ------------------------
        // Layer-free original PlayStation layout. The physical D-pad and
        // analog stick stay independent; DualShock right-stick/L3/R3
        // functions are intentionally outside this controller's scope.
        s.name = "PS1 (N64 Controller)";
        s.core_option_pad_type = "analog";
        s.analog_dpad_mode = "0";
        s.clear_unassigned_buttons = true;

        s.b = L::N64_A;        // Cross
        s.y = L::N64_B;        // Square
        s.x = L::N64_C_LEFT;   // Triangle
        s.a = L::N64_C_DOWN;   // Circle
        s.l = L::N64_L;        // L1
        s.r = L::N64_R;        // R1
        s.l2 = L::N64_Z;       // L2
        s.r2 = L::N64_C_RIGHT; // R2
        s.select = L::N64_C_UP;
        s.start = L::N64_START;

        dpad();
        stick();
        s.left_stick = true;
        // No stick_to_dpad and no explicit hotkeys. RetroArch's global
        // L1+R1+Start+Select combo opens the menu.

    } else if (core.find("prosystem") != std::string::npos) {
        s.name = "Atari 7800"; s.analog_dpad_mode = "0";
        s.b = L::N64_B; s.a = L::N64_A; s.start = L::N64_START;
        // legacy sets select_btn="10" explicitly — that IS the struct
        // default, and physical button 10 is unused on this pad, so the
        // slot stays nullopt and the default carries it. Same for the
        // other branches below that "set" a field to its default.
        dpad(); stick(); s.left_stick = true; s.stick_to_dpad = true; hotkeys();

    } else if (core.find("genesis_plus_gx") != std::string::npos) {
        s.name = "Sega Genesis"; s.analog_dpad_mode = "0";
        // Genesis 3-button: A, B, C
        s.a = L::N64_A;       // C
        s.b = L::N64_B;       // B
        s.y = L::N64_C_DOWN;  // A
        s.start = L::N64_START;
        dpad(); stick(); s.left_stick = true; s.stick_to_dpad = true; hotkeys();

    } else if (core.find("snes9x") != std::string::npos) {
        s.name = "Super Nintendo"; s.analog_dpad_mode = "0";
        // SNES layout: B, A, Y, X, L, R
        s.b = L::N64_B; s.a = L::N64_A; s.y = L::N64_C_DOWN; s.x = L::N64_C_LEFT;
        s.l = L::N64_L; s.r = L::N64_R; s.start = L::N64_START;
        dpad(); stick(); s.left_stick = true; s.stick_to_dpad = true; hotkeys();

    } else if (core.find("mednafen_pce_fast") != std::string::npos) {
        s.name = "PC Engine / TurboGrafx-16"; s.analog_dpad_mode = "0";
        s.b = L::N64_B;       // II
        s.a = L::N64_A;       // I
        s.start = L::N64_START;
        // Turbo buttons
        s.y = L::N64_C_LEFT;  // Turbo II
        s.x = L::N64_C_DOWN;  // Turbo I
        dpad();
        stick(); s.left_stick = true; s.stick_to_dpad = true; hotkeys();

    } else if (core.find("fbneo") != std::string::npos) {
        s.name = "Arcade (FinalBurn Neo)"; s.analog_dpad_mode = "0";
        // Standard 6-button arcade layout
        // 1 2 3    ->  Y  X  L
        // 4 5 6    ->  B  A  R
        s.y = L::N64_C_LEFT; s.x = L::N64_C_DOWN; s.l = L::N64_L;
        s.b = L::N64_B; s.a = L::N64_A; s.r = L::N64_R;
        s.select = L::N64_C_UP;  // Coin
        s.start = L::N64_START;
        dpad();
        stick(); s.left_stick = true; s.stick_to_dpad = true; hotkeys();

    } else if (core.find("mupen64plus") != std::string::npos || core.find("parallel_n64") != std::string::npos) {
        // ---- Nintendo 64 on a real N64 pad -------------------------
        // The one case where the hardware and the emulated console are
        // the same shape, so this is a straight 1:1 passthrough and the
        // labels on the plastic tell the truth.
        //
        // UNVALIDATED ON HARDWARE — button feel needs a real pad and a
        // real ROM. Considered starting point, not a finished mapping.
        s.name = "Nintendo 64 (N64 pad)"; s.analog_dpad_mode = "0";  // real analog stick
        s.b = L::N64_A; s.a = L::N64_B; s.l = L::N64_L; s.r = L::N64_R;
        s.l2 = L::N64_Z; s.start = L::N64_START;
        stick(); s.left_stick = true;   // NO stick_to_dpad — d-pad must not double
        // The C cluster. On this pad they are four DIGITAL buttons, but
        // mupen64plus_next / parallel_n64 read the C buttons off the
        // RetroPad RIGHT STICK. RetroArch will drive an analog bind from a
        // plain button, so the profile resolves these to the _btn form
        // (build_mapping picks the form off the profile's binding kind) —
        // the adapter has no second stick, and binding to a nonexistent
        // axis would silently do nothing.
        s.r_up = L::N64_C_UP; s.r_down = L::N64_C_DOWN;
        s.r_left = L::N64_C_LEFT; s.r_right = L::N64_C_RIGHT;
        // D-pad on the hat only. Don't also drive it from the stick or
        // analog input would double as D-pad presses in-game.
        dpad(); hotkeys();

    } else if (core.find("flycast") != std::string::npos) {
        // ---- Sega Dreamcast on an N64 pad --------------------------
        // Awkward but workable: the DC's four face buttons land on the
        // N64's two face buttons plus two of the C cluster, and the DC's
        // two ANALOG triggers land on the N64's digital shoulders (so
        // they read as fully-pressed — fine for most titles, imprecise
        // for the racing games). The PS-style pad is the better fit for
        // Dreamcast if one is to hand.
        //
        // UNVALIDATED ON HARDWARE — see the N64 note above.
        s.name = "Dreamcast (N64 pad)"; s.analog_dpad_mode = "0";
        s.b = L::N64_A;       // DC A
        s.a = L::N64_B;       // DC B
        s.y = L::N64_C_LEFT;  // DC X
        s.x = L::N64_C_DOWN;  // DC Y
        s.l2 = L::N64_L;      // DC left trigger
        s.r2 = L::N64_R;      // DC right trigger
        s.start = L::N64_START;
        stick(); s.left_stick = true; dpad(); hotkeys();
    }
    return s;
}

// Semantic, per-core RetroPad mapping for the DragonRise/Microntek PS-style
// USB pad (0079:0006). See controller_profile.cpp for the pad's physical
// button IDs and the open hat-vs-8-bit-axis d-pad question.
//
// Hotkey combo across all cores: Select + Start = toggle RetroArch menu.
SemanticMapping semantic_ps_style(const std::string& core) {
    SemanticMapping s;
    s.analog_dpad_mode = "0";
    s.hotkey_enable = L::SELECT; s.menu_toggle = L::START;
    s.stick_up = L::LSTICK_UP; s.stick_down = L::LSTICK_DOWN;
    s.stick_left = L::LSTICK_LEFT; s.stick_right = L::LSTICK_RIGHT;
    s.left_stick = true; s.stick_to_dpad = true;   // preamble defaults

    if (core.find("nestopia") != std::string::npos || core.find("fceumm") != std::string::npos) {
        s.name = "NES (PS-style)";
        s.b = L::CROSS;     // NES B, run
        s.a = L::CIRCLE;    // NES A, jump
        s.y = L::SQUARE;    // turbo B
        s.x = L::TRIANGLE;  // turbo A
        s.select = L::SELECT; s.start = L::START;
        s.up = L::DPAD_UP; s.down = L::DPAD_DOWN;
        s.left = L::DPAD_LEFT; s.right = L::DPAD_RIGHT;
        s.extra_config = "nestopia_audio_vol_sq1 = \"100\"\n"
                         "nestopia_audio_vol_sq2 = \"100\"\n"
                         "nestopia_audio_vol_tri = \"100\"\n"
                         "nestopia_audio_vol_noise = \"100\"\n"
                         "nestopia_audio_vol_dpcm = \"100\"\n";

    } else if (core.find("snes9x") != std::string::npos) {
        s.name = "Super Nintendo (PS-style)";
        s.b = L::CROSS; s.a = L::CIRCLE; s.y = L::SQUARE; s.x = L::TRIANGLE;
        s.l = L::L1; s.r = L::R1; s.select = L::SELECT; s.start = L::START;
        s.up = L::DPAD_UP; s.down = L::DPAD_DOWN;
        s.left = L::DPAD_LEFT; s.right = L::DPAD_RIGHT;

    } else if (core.find("genesis_plus_gx") != std::string::npos) {
        s.name = "Sega Genesis (PS-style)";
        // 3-button: A B C on face; 6-button adds X Y Z on top row
        s.y = L::SQUARE;    // Genesis A (left)
        s.b = L::CROSS;     // Genesis B (middle)
        s.a = L::CIRCLE;    // Genesis C (right)
        s.x = L::TRIANGLE;  // Genesis X (6-button top)
        s.l = L::L1;        // Genesis Y
        s.r = L::R1;        // Genesis Z
        s.start = L::START;
        s.up = L::DPAD_UP; s.down = L::DPAD_DOWN;
        s.left = L::DPAD_LEFT; s.right = L::DPAD_RIGHT;

    } else if (core.find("pcsx") != std::string::npos || core.find("beetle_psx") != std::string::npos || core.find("swanstation") != std::string::npos) {
        // ---- Sony PlayStation ---------------------------------------
        // The one console whose controller IS this controller, so every
        // slot is a straight 1:1 and the labels on the plastic tell the
        // truth. core_option_pad_type = "analog" makes pcsx_rearmed
        // present a DUALSHOCK to the game -- which means the game polls
        // two sticks and two stick clicks, and all four of those have to
        // be bound here or the emulated pad is lying about what it has.
        s.name = "PS1 (PS-style, 1:1)"; s.core_option_pad_type = "analog";
        s.b = L::CROSS; s.a = L::CIRCLE; s.y = L::SQUARE; s.x = L::TRIANGLE;
        s.l = L::L1; s.r = L::R1; s.l2 = L::L2; s.r2 = L::R2;
        s.select = L::SELECT; s.start = L::START;
        s.up = L::DPAD_UP; s.down = L::DPAD_DOWN;
        s.left = L::DPAD_LEFT; s.right = L::DPAD_RIGHT;
        // Right stick -> RetroPad right stick, 1:1. Unlike the N64 branch
        // below (which spends this stick on the C-button cluster) there is
        // nothing to translate: the DualShock's right stick and the
        // RetroPad's are the same control.
        s.r_up = L::RSTICK_UP; s.r_down = L::RSTICK_DOWN;
        s.r_left = L::RSTICK_LEFT; s.r_right = L::RSTICK_RIGHT;
        // Stick clicks. Only PS1 sets these: it is the only console the box
        // emulates whose controller has them.
        s.l3 = L::L3; s.r3 = L::R3;
        // NOTE ON stick_to_dpad: deliberately LEFT AT the preamble's `true`,
        // NOT cleared the way the N64 branch below clears it. The two are
        // unrelated despite looking adjacent. stick_to_dpad governs the
        // *_axis D-PAD binds and reads sem.stick_* -- the LEFT stick -- so
        // it cannot affect, and is not affected by, the r_* fields just set
        // above (a disjoint set of RetroArch settings, and disjoint axes:
        // left stick 0/1, right stick 2/3). The N64 branch clears it to
        // match what the legacy N64 table emitted, not because it gained a
        // right stick. Clearing it here would instead REMOVE four
        // currently-emitted binds and stop the left stick doubling as the
        // D-pad on every fielded box -- a behavior change PS1 never asked
        // for and no hardware evidence calls for.

    } else if (core.find("prosystem") != std::string::npos) {
        s.name = "Atari 7800 (PS-style)";
        s.b = L::CROSS; s.a = L::CIRCLE; s.select = L::SELECT; s.start = L::START;
        s.up = L::DPAD_UP; s.down = L::DPAD_DOWN;
        s.left = L::DPAD_LEFT; s.right = L::DPAD_RIGHT;

    } else if (core.find("mednafen_pce_fast") != std::string::npos) {
        s.name = "PC Engine (PS-style)";
        s.b = L::CROSS;     // II, secondary
        s.a = L::CIRCLE;    // I, primary (right on a real PCE pad)
        s.y = L::SQUARE;    // turbo II
        s.x = L::TRIANGLE;  // turbo I
        s.select = L::SELECT; s.start = L::START;
        s.up = L::DPAD_UP; s.down = L::DPAD_DOWN;
        s.left = L::DPAD_LEFT; s.right = L::DPAD_RIGHT;

    } else if (core.find("fbneo") != std::string::npos) {
        s.name = "Arcade / FBNeo (PS-style)";
        // Classic Capcom 6-button fighter: top row = punches, bottom row = kicks.
        //     Square Triangle L1     (1 = LP, 2 = MP, 3 = HP)
        //     Cross  Circle   R1     (4 = LK, 5 = MK, 6 = HK)
        // RetroPad assignments (RetroArch's internal "arcade button N" indices):
        //   Y=1, X=2, L=3, B=4, A=5, R=6
        s.y = L::SQUARE; s.x = L::TRIANGLE; s.l = L::L1;
        s.b = L::CROSS; s.a = L::CIRCLE; s.r = L::R1;
        s.select = L::SELECT; s.start = L::START;
        s.up = L::DPAD_UP; s.down = L::DPAD_DOWN;
        s.left = L::DPAD_LEFT; s.right = L::DPAD_RIGHT;

    } else if (core.find("mupen64plus") != std::string::npos || core.find("parallel_n64") != std::string::npos) {
        // ---- Nintendo 64 -------------------------------------------
        // The N64 pad has no modern equivalent: one analog stick, a
        // D-pad, A/B, Z (underside trigger), L/R shoulders, Start, and
        // a four-button C cluster. mupen64plus_next / parallel_n64
        // expect the C buttons on the RetroPad RIGHT STICK, which is
        // why ControllerMapping grew r_*_ fields.
        //
        s.name = "Nintendo 64 (PS-style)";
        s.clear_unassigned_buttons = true;

        // Mupen's alternate map translates these RetroPad slots to the
        // final N64 functions shown at right.
        s.b = L::CROSS;       // bottom -> N64 A
        s.y = L::SQUARE;      // left   -> N64 B
        s.x = L::TRIANGLE;    // top    -> C-Up
        s.r = L::CIRCLE;      // right  -> C-Right
        s.select = L::L1;     // L1     -> N64 L
        s.l2 = L::L2;         // L2     -> N64 Z
        s.r2 = L::R1;         // R1     -> N64 R
        s.a = L::R2;          // R2     -> C-Down
        s.start = L::START;
        // Right stick = C-button cluster; D-pad stays on the hat so
        // analog input doesn't double as D-pad presses in-game.
        s.stick_to_dpad = false;   // legacy clears the *_axis dpad binds
        s.r_up = L::RSTICK_UP; s.r_down = L::RSTICK_DOWN;
        s.r_left = L::RSTICK_LEFT; s.r_right = L::RSTICK_RIGHT;
        s.up = L::DPAD_UP; s.down = L::DPAD_DOWN;
        s.left = L::DPAD_LEFT; s.right = L::DPAD_RIGHT;

    } else if (core.find("flycast") != std::string::npos) {
        // ---- Sega Dreamcast ----------------------------------------
        // Maps cleanly onto a modern pad: A/B/X/Y, one analog stick,
        // two analog triggers, Start. No Select — the DC controller
        // genuinely has no equivalent, so it is left unbound.
        //
        // UNVALIDATED ON HARDWARE — see the N64 note above.
        s.name = "Dreamcast (PS-style)";
        s.b = L::CROSS;     // DC A
        s.a = L::CIRCLE;    // DC B
        s.y = L::SQUARE;    // DC X
        s.x = L::TRIANGLE;  // DC Y
        s.l2 = L::L2;       // DC left trigger (analog)
        s.r2 = L::R2;       // DC right trigger (analog)
        s.start = L::START;
        // NOTE: legacy flycast PS branch KEEPS the preamble's
        // stick-to-dpad binds (it never clears up_axis) — do not clear
        // stick_to_dpad here. Quirk preserved by the snapshot.
        s.up = L::DPAD_UP; s.down = L::DPAD_DOWN;
        s.left = L::DPAD_LEFT; s.right = L::DPAD_RIGHT;
    }
    // else: unknown core. Legacy returns the preamble-only mapping (name
    // "Default", PS hotkeys/sticks applied) — shouldn't happen because
    // every shipped core matches one of the branches above.
    return s;
}

}  // namespace

SemanticMapping get_semantic_mapping(ControllerStyle style, const std::string& core_name) {
    return style == ControllerStyle::N64_STYLE ? semantic_n64_style(core_name)
                                                : semantic_ps_style(core_name);
}

ControllerMapping build_mapping(const SemanticMapping& sem,
                                const PhysicalProfile& profile) {
    ControllerMapping m;  // struct defaults are load-bearing — see header
    m.name = sem.name;
    m.analog_dpad_mode = sem.analog_dpad_mode;
    m.core_option_pad_type = sem.core_option_pad_type;
    m.extra_config = sem.extra_config;

    if (sem.clear_unassigned_buttons) {
        m.b_btn.clear();
        m.y_btn.clear();
        m.select_btn.clear();
        m.start_btn.clear();
        m.a_btn.clear();
        m.x_btn.clear();
        m.l_btn.clear();
        m.r_btn.clear();
        m.l2_btn.clear();
        m.r2_btn.clear();
        m.l3_btn.clear();
        m.r3_btn.clear();
    }

    using Kind = PhysicalBinding::Kind;
    auto kind_is = [&](LogicalControl c, Kind want) {
        const PhysicalBinding* b = profile.binding(c);
        return b && b->kind == want;
    };

    // THE FIELD FIXES THE FORM; THE PROFILE SUPPLIES THE KIND. These two must
    // agree or the emitted config is not merely unbound, it is WRONG:
    //
    //   *_btn  accepts a digital source only. RetroArch's parser reads any
    //          value that does not start with 'h' as a NUMERIC BUTTON INDEX,
    //          so an axis token dropped here ("-0", "+0") silently resolves
    //          to physical button 0 -- and "+0"/"-0" collide on the same
    //          button. BUTTON and HAT are the two digital kinds; both are
    //          fine. Anything else emits "" -- unbound is honest, mis-bound
    //          is not.
    //   *_axis accepts a signed axis token ("+2"/"-3") only. A button or hat
    //          token fails RetroArch's axis parse outright, so it is already
    //          inert; emitting "" says so out loud instead of leaving a value
    //          that reads as configured.
    //
    // The right-stick block below has always followed the profile's kind.
    // These two helpers extend the same contract to every other slot.
    auto put_btn = [&](std::string ControllerMapping::*field,
                       const std::optional<LogicalControl>& slot) {
        if (!slot) return;
        m.*field = kind_is(*slot, Kind::AXIS) ? std::string() : profile.token(*slot);
    };
    auto axis_token = [&](const std::optional<LogicalControl>& slot) {
        return (slot && kind_is(*slot, Kind::AXIS)) ? profile.token(*slot)
                                                    : std::string();
    };

    put_btn(&ControllerMapping::b_btn, sem.b);
    put_btn(&ControllerMapping::y_btn, sem.y);
    put_btn(&ControllerMapping::select_btn, sem.select);
    put_btn(&ControllerMapping::start_btn, sem.start);
    put_btn(&ControllerMapping::a_btn, sem.a);
    put_btn(&ControllerMapping::x_btn, sem.x);
    put_btn(&ControllerMapping::l_btn, sem.l);
    put_btn(&ControllerMapping::r_btn, sem.r);
    put_btn(&ControllerMapping::l2_btn, sem.l2);
    put_btn(&ControllerMapping::r2_btn, sem.r2);
    // Stick clicks are digital buttons on every pad that has them, so they
    // go through the same put_btn contract as the face buttons: an unset
    // slot, a control the profile never captured, or one captured as an axis
    // all leave the field "" -- unbound rather than mis-bound.
    put_btn(&ControllerMapping::l3_btn, sem.l3);
    put_btn(&ControllerMapping::r3_btn, sem.r3);

    // D-pad: the binding's kind decides WHICH FIELD, not just the value. On
    // pads that overload ABS_X/ABS_Y for the d-pad and carry no hat at all
    // (the 8-bit DragonRise/Microntek class this wizard exists to support),
    // a captured d-pad direction is AXIS-kind and belongs in
    // up/down/left/right_axis. Resolved per direction rather than
    // all-or-nothing like the right stick: there, four fields describe one
    // two-axis control in two mutually exclusive forms; here, _btn and _axis
    // are independent RetroArch settings for each direction, so a mixed-kind
    // d-pad still yields four working binds instead of none.
    struct DpadSlot {
        std::string ControllerMapping::*btn;
        std::string ControllerMapping::*axis;
        const std::optional<LogicalControl>* slot;
    };
    const DpadSlot dpad[4] = {
        {&ControllerMapping::up_btn,    &ControllerMapping::up_axis,    &sem.up},
        {&ControllerMapping::down_btn,  &ControllerMapping::down_axis,  &sem.down},
        {&ControllerMapping::left_btn,  &ControllerMapping::left_axis,  &sem.left},
        {&ControllerMapping::right_btn, &ControllerMapping::right_axis, &sem.right},
    };
    std::string dpad_axis[4];
    for (int i = 0; i < 4; ++i) {
        if (!*dpad[i].slot) continue;
        if (kind_is(**dpad[i].slot, Kind::AXIS)) {
            // Clear the _btn field: its struct default is a hat token for a
            // hat this pad does not have.
            m.*dpad[i].btn = "";
            dpad_axis[i] = profile.token(**dpad[i].slot);
        } else {
            m.*dpad[i].btn = profile.token(**dpad[i].slot);
        }
    }

    if (sem.left_stick) {
        m.l_x_plus  = axis_token(sem.stick_right);
        m.l_x_minus = axis_token(sem.stick_left);
        m.l_y_plus  = axis_token(sem.stick_down);
        m.l_y_minus = axis_token(sem.stick_up);
    }
    if (sem.stick_to_dpad) {
        m.right_axis = axis_token(sem.stick_right);
        m.left_axis  = axis_token(sem.stick_left);
        m.down_axis  = axis_token(sem.stick_down);
        m.up_axis    = axis_token(sem.stick_up);
    }
    // Applied AFTER the stick_to_dpad block, deliberately. When a pad shares
    // one axis pair between its d-pad and its left stick, CaptureSession
    // rejects the stick steps as duplicates of the d-pad, so stick_* comes
    // out unbound and stick_to_dpad would otherwise overwrite a real d-pad
    // bind with "". The d-pad also wins outright on a pad that has both: an
    // unbound d-pad is dead, whereas a stick that merely stops doubling as a
    // d-pad still works through l_x_*/l_y_*.
    for (int i = 0; i < 4; ++i) {
        if (!dpad_axis[i].empty()) m.*dpad[i].axis = dpad_axis[i];
    }

    // Right stick / C cluster: axis vs button form follows the PROFILE's
    // binding kind (a real stick binds axes; a digital C cluster binds
    // buttons). Mirrors the legacy write_right_stick_binds contract.
    //
    // Emit ONLY when all four controls are present in the profile AND
    // share the same binding kind. A wizard capture can be partial (e.g.
    // r_up captured but r_right skipped) or mixed-kind (one control lands
    // on an axis, another on a button) -- either way there is no single
    // coherent form to write, so this leaves all eight fields empty
    // rather than emitting an incoherent mix (a button token sitting in
    // an axis field, or three tokens next to one empty one).
    if (sem.r_up && sem.r_down && sem.r_left && sem.r_right) {
        const auto* up = profile.binding(*sem.r_up);
        const auto* down = profile.binding(*sem.r_down);
        const auto* left = profile.binding(*sem.r_left);
        const auto* right = profile.binding(*sem.r_right);
        if (up && down && left && right && up->kind == down->kind &&
            up->kind == left->kind && up->kind == right->kind) {
            if (up->kind == PhysicalBinding::Kind::AXIS) {
                m.r_x_plus  = right->token;
                m.r_x_minus = left->token;
                m.r_y_plus  = down->token;
                m.r_y_minus = up->token;
            } else {
                m.r_x_plus_btn  = right->token;
                m.r_x_minus_btn = left->token;
                m.r_y_plus_btn  = down->token;
                m.r_y_minus_btn = up->token;
            }
        }
    }

    // Hotkeys are emitted as input_enable_hotkey_btn / input_menu_toggle_btn,
    // i.e. the same digital-only form as the face buttons above.
    put_btn(&ControllerMapping::enable_hotkey_btn, sem.hotkey_enable);
    put_btn(&ControllerMapping::menu_toggle_btn, sem.menu_toggle);
    return m;
}

ControllerMapping get_mapping(ControllerType type, const std::string& core_name) {
    switch (type) {
        case ControllerType::PS_STYLE_DRAGONRISE:
            return build_mapping(get_semantic_mapping(ControllerStyle::PS_STYLE, core_name),
                                 builtin_dragonrise_profile());
        case ControllerType::N64_ADAPTER:
        case ControllerType::UNKNOWN:
        default:
            return build_mapping(get_semantic_mapping(ControllerStyle::N64_STYLE, core_name),
                                 builtin_n64_adapter_profile());
    }
}

ControllerMapping resolve_mapping_for_pad(
    uint16_t vid, uint16_t pid,
    const std::map<std::string, PhysicalProfile>& store,
    const std::string& core_name) {
    // 1. Captured profile wins (covers rewired clones of known pads too).
    auto it = store.find(vidpid_key(vid, pid));
    if (it != store.end()) {
        return build_mapping(get_semantic_mapping(it->second.style, core_name),
                             it->second);
    }
    // 2. Builtin by VID/PID; 3. legacy N64 fallback for everything else.
    return get_mapping(match_vid_pid(vid, pid), core_name);
}

PortMappings resolve_port_mappings(
    const std::vector<DetectedPad>& pads, ControllerType fallback_type,
    const std::map<std::string, PhysicalProfile>& store,
    const std::string& core_name) {
    PortMappings out;
    if (pads.empty()) {
        // No pads detected: today's exact path, unchanged. Do NOT route
        // this through resolve_mapping_for_pad(0, 0, ...) -- that would
        // silently pick up a captured profile if one ever existed for
        // VID/PID 0000:0000, diverging from the legacy behavior this branch
        // must preserve.
        out.p1 = get_mapping(fallback_type, core_name);
        out.p2 = out.p1;
        return out;
    }
    out.p1 = resolve_mapping_for_pad(pads[0].vid, pads[0].pid, store, core_name);
    // Only one pad connected: player 2 mirrors player 1 exactly, matching
    // every currently-fielded box (two identical pads, or -- as measured on
    // the one Pi we have hardware evidence for -- a single unrecognized pad
    // riding the legacy N64 fallback via resolve_mapping_for_pad above).
    out.p2 = pads.size() > 1
                 ? resolve_mapping_for_pad(pads[1].vid, pads[1].pid, store, core_name)
                 : out.p1;
    return out;
}

void write_right_stick_binds(std::ostream& out, const ControllerMapping& map,
                             int player) {
    const std::string p = "input_player" + std::to_string(player) + "_r_";
    // Real right stick (PS-style pad): bind to axes.
    if (!map.r_x_plus.empty()) {
        out << p << "x_plus_axis = \"" << map.r_x_plus << "\"\n";
        out << p << "x_minus_axis = \"" << map.r_x_minus << "\"\n";
        out << p << "y_plus_axis = \"" << map.r_y_plus << "\"\n";
        out << p << "y_minus_axis = \"" << map.r_y_minus << "\"\n";
        return;
    }
    // Digital C cluster (N64 adapter): bind the same analog functions to
    // plain buttons. RetroArch accepts either form for an analog bind.
    if (!map.r_x_plus_btn.empty()) {
        out << p << "x_plus_btn = \"" << map.r_x_plus_btn << "\"\n";
        out << p << "x_minus_btn = \"" << map.r_x_minus_btn << "\"\n";
        out << p << "y_plus_btn = \"" << map.r_y_plus_btn << "\"\n";
        out << p << "y_minus_btn = \"" << map.r_y_minus_btn << "\"\n";
    }
}

void write_player_binds(std::ostream& out, const ControllerMapping& map,
                        int player) {
    const std::string p = "input_player" + std::to_string(player) + "_";
    // RetroArch's config parser does not treat an empty button string as
    // unbound: strtoull("", ...) yields 0, silently aliasing physical button
    // zero. "nul" is RetroArch's explicit NO_BTN sentinel.
    auto write_btn = [&](const char* name, const std::string& token) {
        out << p << name << "_btn = \""
            << (token.empty() ? "nul" : token) << "\"\n";
    };
    out << p << "analog_dpad_mode = \"" << map.analog_dpad_mode << "\"\n";
    write_btn("b", map.b_btn);
    write_btn("y", map.y_btn);
    write_btn("select", map.select_btn);
    write_btn("start", map.start_btn);
    write_btn("up", map.up_btn);
    write_btn("down", map.down_btn);
    write_btn("left", map.left_btn);
    write_btn("right", map.right_btn);
    write_btn("a", map.a_btn);
    write_btn("x", map.x_btn);
    write_btn("l", map.l_btn);
    write_btn("r", map.r_btn);
    write_btn("l2", map.l2_btn);
    write_btn("r2", map.r2_btn);
    // L3/R3 sit here, after r2 and before the analog axes, matching both this
    // struct's field order and the order a stock retroarch.cfg lists them in.
    // Unconditional like every other line in this block: "nul" means "this
    // console has no stick click", which RetroArch must be told explicitly.
    write_btn("l3", map.l3_btn);
    write_btn("r3", map.r3_btn);
    out << p << "l_x_plus_axis = \"" << map.l_x_plus << "\"\n";
    out << p << "l_x_minus_axis = \"" << map.l_x_minus << "\"\n";
    out << p << "l_y_plus_axis = \"" << map.l_y_plus << "\"\n";
    out << p << "l_y_minus_axis = \"" << map.l_y_minus << "\"\n";
    write_right_stick_binds(out, map, player);
    out << p << "up_axis = \"" << map.up_axis << "\"\n";
    out << p << "down_axis = \"" << map.down_axis << "\"\n";
    out << p << "left_axis = \"" << map.left_axis << "\"\n";
    out << p << "right_axis = \"" << map.right_axis << "\"\n";
}

}  // namespace retroarch
