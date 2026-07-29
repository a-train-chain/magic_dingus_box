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
// Hotkey combo across all cores: Z trigger + Start = toggle RetroArch menu.
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
        stick(); s.stick_to_dpad = true;  // stick -> d-pad, so it works for Mario
        hotkeys();
        s.extra_config = "nestopia_audio_vol_sq1 = \"100\"\n"
                         "nestopia_audio_vol_sq2 = \"100\"\n"
                         "nestopia_audio_vol_tri = \"100\"\n"
                         "nestopia_audio_vol_noise = \"100\"\n"
                         "nestopia_audio_vol_dpcm = \"100\"\n";

    } else if (core.find("pcsx") != std::string::npos || core.find("beetle_psx") != std::string::npos || core.find("swanstation") != std::string::npos) {
        s.name = "PS1 (N64 Controller)"; s.core_option_pad_type = "analog"; s.analog_dpad_mode = "0";
        // PS1 face buttons land on the N64's A/B plus two of the C cluster.
        s.b = L::N64_A;       // Cross (primary action)
        s.a = L::N64_B;       // Circle (secondary)
        s.y = L::N64_C_DOWN;  // Square (attack/action)
        s.x = L::N64_C_LEFT;  // Triangle (menu/special)
        s.start = L::N64_START;
        s.select = L::N64_C_UP;
        s.l = L::N64_L; s.r = L::N64_R; s.r2 = L::N64_C_RIGHT;
        stick(); s.left_stick = true; s.stick_to_dpad = true; hotkeys();

    } else if (core.find("prosystem") != std::string::npos) {
        s.name = "Atari 7800"; s.analog_dpad_mode = "0";
        s.b = L::N64_B; s.a = L::N64_A; s.start = L::N64_START;
        // legacy sets select_btn="10" explicitly — that IS the struct
        // default, and physical button 10 is unused on this pad, so the
        // slot stays nullopt and the default carries it. Same for the
        // other branches below that "set" a field to its default.
        dpad(); stick(); s.stick_to_dpad = true; hotkeys();

    } else if (core.find("genesis_plus_gx") != std::string::npos) {
        s.name = "Sega Genesis"; s.analog_dpad_mode = "0";
        // Genesis 3-button: A, B, C
        s.a = L::N64_A;       // C
        s.b = L::N64_B;       // B
        s.y = L::N64_C_DOWN;  // A
        s.start = L::N64_START;
        dpad(); stick(); s.stick_to_dpad = true; hotkeys();

    } else if (core.find("snes9x") != std::string::npos) {
        s.name = "Super Nintendo"; s.analog_dpad_mode = "0";
        // SNES layout: B, A, Y, X, L, R
        s.b = L::N64_B; s.a = L::N64_A; s.y = L::N64_C_DOWN; s.x = L::N64_C_LEFT;
        s.l = L::N64_L; s.r = L::N64_R; s.start = L::N64_START;
        dpad(); stick(); s.stick_to_dpad = true; hotkeys();

    } else if (core.find("mednafen_pce_fast") != std::string::npos) {
        s.name = "PC Engine / TurboGrafx-16"; s.analog_dpad_mode = "0";
        s.b = L::N64_B;       // II
        s.a = L::N64_A;       // I
        s.start = L::N64_START;
        // Turbo buttons
        s.y = L::N64_C_LEFT;  // Turbo II
        s.x = L::N64_C_DOWN;  // Turbo I
        stick(); s.stick_to_dpad = true; hotkeys();

    } else if (core.find("fbneo") != std::string::npos) {
        s.name = "Arcade (FinalBurn Neo)"; s.analog_dpad_mode = "0";
        // Standard 6-button arcade layout
        // 1 2 3    ->  Y  X  L
        // 4 5 6    ->  B  A  R
        s.y = L::N64_C_LEFT; s.x = L::N64_C_DOWN; s.l = L::N64_L;
        s.b = L::N64_B; s.a = L::N64_A; s.r = L::N64_R;
        s.select = L::N64_C_UP;  // Coin
        s.start = L::N64_START;
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
        s.extra_config = "nestopia_audio_vol_sq1 = \"100\"\n"
                         "nestopia_audio_vol_sq2 = \"100\"\n"
                         "nestopia_audio_vol_tri = \"100\"\n"
                         "nestopia_audio_vol_noise = \"100\"\n"
                         "nestopia_audio_vol_dpcm = \"100\"\n";

    } else if (core.find("snes9x") != std::string::npos) {
        s.name = "Super Nintendo (PS-style)";
        s.b = L::CROSS; s.a = L::CIRCLE; s.y = L::SQUARE; s.x = L::TRIANGLE;
        s.l = L::L1; s.r = L::R1; s.select = L::SELECT; s.start = L::START;

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

    } else if (core.find("pcsx") != std::string::npos || core.find("beetle_psx") != std::string::npos || core.find("swanstation") != std::string::npos) {
        s.name = "PS1 (PS-style, 1:1)"; s.core_option_pad_type = "analog";
        s.b = L::CROSS; s.a = L::CIRCLE; s.y = L::SQUARE; s.x = L::TRIANGLE;
        s.l = L::L1; s.r = L::R1; s.l2 = L::L2; s.r2 = L::R2;
        s.select = L::SELECT; s.start = L::START;

    } else if (core.find("prosystem") != std::string::npos) {
        s.name = "Atari 7800 (PS-style)";
        s.b = L::CROSS; s.a = L::CIRCLE; s.select = L::SELECT; s.start = L::START;

    } else if (core.find("mednafen_pce_fast") != std::string::npos) {
        s.name = "PC Engine (PS-style)";
        s.b = L::CROSS;     // II, secondary
        s.a = L::CIRCLE;    // I, primary (right on a real PCE pad)
        s.y = L::SQUARE;    // turbo II
        s.x = L::TRIANGLE;  // turbo I
        s.select = L::SELECT; s.start = L::START;

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

    } else if (core.find("mupen64plus") != std::string::npos || core.find("parallel_n64") != std::string::npos) {
        // ---- Nintendo 64 -------------------------------------------
        // The N64 pad has no modern equivalent: one analog stick, a
        // D-pad, A/B, Z (underside trigger), L/R shoulders, Start, and
        // a four-button C cluster. mupen64plus_next / parallel_n64
        // expect the C buttons on the RetroPad RIGHT STICK, which is
        // why ControllerMapping grew r_*_ fields.
        //
        // UNVALIDATED ON HARDWARE — button feel needs a real pad and a
        // real ROM. Treat these as a considered starting point, not a
        // finished mapping; verify before shipping.
        s.name = "Nintendo 64 (PS-style)";
        s.b = L::CROSS; s.a = L::CIRCLE; s.l = L::L1; s.r = L::R1;
        s.l2 = L::L2; s.start = L::START;
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

    auto put = [&](std::string ControllerMapping::*field,
                   const std::optional<LogicalControl>& slot) {
        if (slot) m.*field = profile.token(*slot);
    };
    put(&ControllerMapping::b_btn, sem.b);
    put(&ControllerMapping::y_btn, sem.y);
    put(&ControllerMapping::select_btn, sem.select);
    put(&ControllerMapping::start_btn, sem.start);
    put(&ControllerMapping::a_btn, sem.a);
    put(&ControllerMapping::x_btn, sem.x);
    put(&ControllerMapping::l_btn, sem.l);
    put(&ControllerMapping::r_btn, sem.r);
    put(&ControllerMapping::l2_btn, sem.l2);
    put(&ControllerMapping::r2_btn, sem.r2);
    put(&ControllerMapping::up_btn, sem.up);
    put(&ControllerMapping::down_btn, sem.down);
    put(&ControllerMapping::left_btn, sem.left);
    put(&ControllerMapping::right_btn, sem.right);

    if (sem.left_stick) {
        m.l_x_plus  = sem.stick_right ? profile.token(*sem.stick_right) : "";
        m.l_x_minus = sem.stick_left  ? profile.token(*sem.stick_left)  : "";
        m.l_y_plus  = sem.stick_down  ? profile.token(*sem.stick_down)  : "";
        m.l_y_minus = sem.stick_up    ? profile.token(*sem.stick_up)    : "";
    }
    if (sem.stick_to_dpad) {
        m.right_axis = sem.stick_right ? profile.token(*sem.stick_right) : "";
        m.left_axis  = sem.stick_left  ? profile.token(*sem.stick_left)  : "";
        m.down_axis  = sem.stick_down  ? profile.token(*sem.stick_down)  : "";
        m.up_axis    = sem.stick_up    ? profile.token(*sem.stick_up)    : "";
    }

    // Right stick / C cluster: axis vs button form follows the PROFILE's
    // binding kind (a real stick binds axes; a digital C cluster binds
    // buttons). Mirrors the legacy write_right_stick_binds contract.
    if (sem.r_up && sem.r_down && sem.r_left && sem.r_right) {
        const auto* up = profile.binding(*sem.r_up);
        if (up && up->kind == PhysicalBinding::Kind::AXIS) {
            m.r_x_plus  = profile.token(*sem.r_right);
            m.r_x_minus = profile.token(*sem.r_left);
            m.r_y_plus  = profile.token(*sem.r_down);
            m.r_y_minus = profile.token(*sem.r_up);
        } else if (up) {
            m.r_x_plus_btn  = profile.token(*sem.r_right);
            m.r_x_minus_btn = profile.token(*sem.r_left);
            m.r_y_plus_btn  = profile.token(*sem.r_down);
            m.r_y_minus_btn = profile.token(*sem.r_up);
        }
        // up == nullptr (skipped in wizard): emit neither form.
    }

    if (sem.hotkey_enable) m.enable_hotkey_btn = profile.token(*sem.hotkey_enable);
    if (sem.menu_toggle)   m.menu_toggle_btn   = profile.token(*sem.menu_toggle);
    if (sem.exit_emulator) m.exit_emulator_btn = profile.token(*sem.exit_emulator);
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

}  // namespace retroarch
