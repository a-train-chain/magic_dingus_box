#include "controller_mapping.h"

#include <ostream>
#include <string>

namespace retroarch {

namespace {

    // N64 Controller Physical Button IDs (verified via evtest):
    //   0=C-Left, 1=B, 2=A, 3=C-Down, 4=L shoulder, 5=R shoulder,
    //   6=Z trigger, 8=C-Right, 9=C-Up, 10=unused, 12=Start
    //   Axes: 0/1=Analog Stick, Hat0X/Hat0Y=D-pad
    //
    // NOTE: evdev names are misleading on this adapter:
    //   BTN_Z (309) = physical R shoulder (button 5)
    //   BTN_TL (310) = physical Z trigger (button 6)
    //
    // Hotkey: Z trigger (button 6) + Start (button 12) = toggle RetroArch menu
    // This combo is consistent across ALL cores.

    ControllerMapping get_mapping_n64_adapter(const std::string& core_name) {
        ControllerMapping map; // Starts with defaults

        if (core_name.find("nestopia") != std::string::npos || core_name.find("fceumm") != std::string::npos) {
            map.name = "NES (N64 Controller)";
            map.analog_dpad_mode = "0"; // Disable auto-analog, use explicit mapping

            map.b_btn = "1";  // NES B (Run) -> N64 B
            map.a_btn = "2";  // NES A (Jump) -> N64 A

            map.select_btn = "9";  // Select -> C-Up
            map.start_btn = "12";  // Start -> Start

            // Turbo Buttons
            map.x_btn = "3";  // Turbo A -> C-Down
            map.y_btn = "0";  // Turbo B -> C-Left

            // Analog Stick -> D-Pad (so stick works for Mario)
            map.right_axis = "+0";
            map.left_axis = "-0";
            map.down_axis = "+1";
            map.up_axis = "-1";

            // Hotkeys: Z trigger + Start for Menu
            map.enable_hotkey_btn = "6"; // Z trigger (under center grip)
            map.menu_toggle_btn = "12";  // Start

            map.extra_config = "nestopia_audio_vol_sq1 = \"100\"\n"
                               "nestopia_audio_vol_sq2 = \"100\"\n"
                               "nestopia_audio_vol_tri = \"100\"\n"
                               "nestopia_audio_vol_noise = \"100\"\n"
                               "nestopia_audio_vol_dpcm = \"100\"\n";

        } else if (core_name.find("pcsx") != std::string::npos || core_name.find("beetle_psx") != std::string::npos || core_name.find("swanstation") != std::string::npos) {
            map.name = "PS1 (N64 Controller)";
            map.core_option_pad_type = "analog";
            map.analog_dpad_mode = "0";

            // PS1 face buttons on right-hand buttons (A, B, C-cluster):
            map.b_btn = "2";  // Cross (primary action) -> A button
            map.a_btn = "1";  // Circle (secondary) -> B button
            map.y_btn = "3";  // Square (attack/action) -> C-Down
            map.x_btn = "0";  // Triangle (menu/special) -> C-Left

            map.start_btn = "12"; // Start -> Start
            map.select_btn = "9"; // Select -> C-Up

            // Shoulder buttons:
            map.l_btn = "4";  // L1 -> L shoulder
            map.r_btn = "5";  // R1 -> R shoulder
            map.r2_btn = "8"; // R2 -> C-Right

            // Analog Stick
            map.l_x_plus = "+0";
            map.l_x_minus = "-0";
            map.l_y_plus = "+1";
            map.l_y_minus = "-1";

            // Analog Stick -> D-Pad
            map.right_axis = "+0";
            map.left_axis = "-0";
            map.down_axis = "+1";
            map.up_axis = "-1";

            // Hotkeys: Z trigger + Start for Menu
            map.enable_hotkey_btn = "6"; // Z trigger (under center grip)
            map.menu_toggle_btn = "12";  // Start

        } else if (core_name.find("prosystem") != std::string::npos) {
            map.name = "Atari 7800";
            map.analog_dpad_mode = "0"; // Disable auto-analog, use explicit mapping

            map.b_btn = "1"; // Button 1 -> RetroPad B
            map.a_btn = "2"; // Button 2 -> RetroPad A

            map.select_btn = "10";
            map.start_btn = "12";

            map.up_btn = "h0up";
            map.down_btn = "h0down";
            map.left_btn = "h0left";
            map.right_btn = "h0right";

            // Analog Stick -> D-Pad (so stick works for movement)
            map.right_axis = "+0";
            map.left_axis = "-0";
            map.down_axis = "+1";
            map.up_axis = "-1";

            // Hotkeys: Z trigger + Start for Menu
            map.enable_hotkey_btn = "6";
            map.menu_toggle_btn = "12";

        } else if (core_name.find("genesis_plus_gx") != std::string::npos) {
            map.name = "Sega Genesis";
            map.analog_dpad_mode = "0"; // Disable auto-analog, use explicit mapping

            // Genesis 3-button: A, B, C
            map.a_btn = "2"; // C
            map.b_btn = "1"; // B
            map.y_btn = "3"; // A

            map.start_btn = "12";

            map.up_btn = "h0up";
            map.down_btn = "h0down";
            map.left_btn = "h0left";
            map.right_btn = "h0right";

            // Analog Stick -> D-Pad (so stick works for movement)
            map.right_axis = "+0";
            map.left_axis = "-0";
            map.down_axis = "+1";
            map.up_axis = "-1";

            // Hotkeys: Z trigger + Start for Menu
            map.enable_hotkey_btn = "6";
            map.menu_toggle_btn = "12";

        } else if (core_name.find("snes9x") != std::string::npos) {
            map.name = "Super Nintendo";
            map.analog_dpad_mode = "0"; // Disable auto-analog, use explicit mapping

            // SNES Layout: B, A, Y, X, L, R
            map.b_btn = "1";
            map.a_btn = "2";
            map.y_btn = "3";
            map.x_btn = "0";

            // Shoulders on physical shoulder buttons
            map.l_btn = "4"; // L -> L shoulder
            map.r_btn = "5"; // R -> R shoulder

            map.start_btn = "12";
            map.select_btn = "10";

            map.up_btn = "h0up";
            map.down_btn = "h0down";
            map.left_btn = "h0left";
            map.right_btn = "h0right";

            // Analog Stick -> D-Pad (so stick works for movement)
            map.right_axis = "+0";
            map.left_axis = "-0";
            map.down_axis = "+1";
            map.up_axis = "-1";

            // Hotkeys: Z trigger + Start for Menu
            map.enable_hotkey_btn = "6"; // Z trigger (under center grip)
            map.menu_toggle_btn = "12";  // Start

        } else if (core_name.find("mednafen_pce_fast") != std::string::npos) {
            map.name = "PC Engine / TurboGrafx-16";
            map.analog_dpad_mode = "0"; // Disable auto-analog, use explicit mapping

            // PCE: I and II buttons
            map.b_btn = "1";  // II
            map.a_btn = "2";  // I

            map.start_btn = "12"; // Run
            map.select_btn = "10"; // Select

            // Turbo buttons
            map.y_btn = "0"; // Turbo II -> C-Left
            map.x_btn = "3"; // Turbo I -> C-Down

            // Analog Stick -> D-Pad (so stick works for movement)
            map.right_axis = "+0";
            map.left_axis = "-0";
            map.down_axis = "+1";
            map.up_axis = "-1";

            // Hotkeys: Z trigger + Start for Menu
            map.enable_hotkey_btn = "6";
            map.menu_toggle_btn = "12";

        } else if (core_name.find("fbneo") != std::string::npos) {
            map.name = "Arcade (FinalBurn Neo)";
            map.analog_dpad_mode = "0";

            // Standard 6-button arcade layout
            // 1 2 3    ->  Y  X  L
            // 4 5 6    ->  B  A  R
            map.y_btn = "0"; // 1 -> C-Left
            map.x_btn = "3"; // 2 -> C-Down
            map.l_btn = "4"; // 3 -> L shoulder

            map.b_btn = "1"; // 4 -> B
            map.a_btn = "2"; // 5 -> A
            map.r_btn = "5"; // 6 -> R shoulder

            map.select_btn = "9";  // Coin -> C-Up
            map.start_btn = "12";  // Start

            // Analog Stick -> D-Pad
            map.l_x_plus = "+0";
            map.l_x_minus = "-0";
            map.l_y_plus = "+1";
            map.l_y_minus = "-1";

            map.right_axis = "+0";
            map.left_axis = "-0";
            map.down_axis = "+1";
            map.up_axis = "-1";

            // Hotkeys: Z trigger + Start for Menu
            map.enable_hotkey_btn = "6";
            map.menu_toggle_btn = "12";

        } else if (core_name.find("mupen64plus") != std::string::npos ||
                   core_name.find("parallel_n64") != std::string::npos) {
            // ---- Nintendo 64 on a real N64 pad -------------------------
            // The one case where the hardware and the emulated console are
            // the same shape, so this is a straight 1:1 passthrough and the
            // labels on the plastic tell the truth.
            //
            // UNVALIDATED ON HARDWARE — button feel needs a real pad and a
            // real ROM. Considered starting point, not a finished mapping.
            map.name = "Nintendo 64 (N64 pad)";
            map.analog_dpad_mode = "0";  // real analog stick

            map.b_btn  = "2";   // physical A -> RetroPad B -> N64 A
            map.a_btn  = "1";   // physical B -> RetroPad A -> N64 B
            map.l_btn  = "4";   // L shoulder
            map.r_btn  = "5";   // R shoulder
            map.l2_btn = "6";   // Z trigger  -> RetroPad L2 -> N64 Z
            map.start_btn = "12";

            // Analog stick, 1:1.
            map.l_x_plus = "+0"; map.l_x_minus = "-0";
            map.l_y_plus = "+1"; map.l_y_minus = "-1";

            // The C cluster. On this pad they are four DIGITAL buttons, but
            // mupen64plus_next / parallel_n64 read the C buttons off the
            // RetroPad RIGHT STICK. RetroArch will drive an analog bind
            // from a plain button, so use the _btn form and leave the
            // _axis form empty — the adapter has no second stick, and
            // binding to a nonexistent axis would silently do nothing.
            map.r_x_plus_btn  = "8";  // C-Right
            map.r_x_minus_btn = "0";  // C-Left
            map.r_y_plus_btn  = "3";  // C-Down
            map.r_y_minus_btn = "9";  // C-Up

            // D-pad on the hat only. Don't also drive it from the stick or
            // analog input would double as D-pad presses in-game.
            map.up_axis = ""; map.down_axis = "";
            map.left_axis = ""; map.right_axis = "";
            map.up_btn = "h0up"; map.down_btn = "h0down";
            map.left_btn = "h0left"; map.right_btn = "h0right";

            map.enable_hotkey_btn = "6";   // Z trigger
            map.menu_toggle_btn = "12";    // Start

        } else if (core_name.find("flycast") != std::string::npos) {
            // ---- Sega Dreamcast on an N64 pad --------------------------
            // Awkward but workable: the DC's four face buttons land on the
            // N64's two face buttons plus two of the C cluster, and the DC's
            // two ANALOG triggers land on the N64's digital shoulders (so
            // they read as fully-pressed — fine for most titles, imprecise
            // for the racing games). The PS-style pad is the better fit for
            // Dreamcast if one is to hand.
            //
            // UNVALIDATED ON HARDWARE — see the N64 note above.
            map.name = "Dreamcast (N64 pad)";
            map.analog_dpad_mode = "0";

            map.b_btn  = "2";   // physical A -> DC A
            map.a_btn  = "1";   // physical B -> DC B
            map.y_btn  = "0";   // C-Left     -> DC X
            map.x_btn  = "3";   // C-Down     -> DC Y
            map.l2_btn = "4";   // L shoulder -> DC left trigger
            map.r2_btn = "5";   // R shoulder -> DC right trigger
            map.start_btn = "12";

            map.l_x_plus = "+0"; map.l_x_minus = "-0";
            map.l_y_plus = "+1"; map.l_y_minus = "-1";

            map.up_axis = ""; map.down_axis = "";
            map.left_axis = ""; map.right_axis = "";
            map.up_btn = "h0up"; map.down_btn = "h0down";
            map.left_btn = "h0left"; map.right_btn = "h0right";

            map.enable_hotkey_btn = "6";
            map.menu_toggle_btn = "12";
        }
        return map;
    }

    // PS-style USB pad (DragonRise/Microntek 0079:0006):
    //   Face buttons: 0=Triangle, 1=Circle, 2=Cross, 3=Square
    //   Shoulders:    4=L1, 5=R1, 6=L2, 7=R2
    //   Center:       8=Select, 9=Start
    //   D-pad:        hat0
    //   Left stick:   axes 0 (X) / 1 (Y)
    //   Right stick:  axes 2 (Rx) / 5 (Ry)
    //
    // Hotkey combo across all cores: Select (8) + Start (9) = RetroArch menu toggle.
    ControllerMapping get_mapping_ps_style(const std::string& core_name) {
        ControllerMapping map;
        map.analog_dpad_mode = "0";

        // Universal PS-pad hotkey
        map.enable_hotkey_btn = "8"; // Select
        map.menu_toggle_btn   = "9"; // Start

        // Left stick defaults (most cores use it for D-pad emulation)
        map.l_x_plus  = "+0";
        map.l_x_minus = "-0";
        map.l_y_plus  = "+1";
        map.l_y_minus = "-1";

        map.right_axis = "+0";
        map.left_axis  = "-0";
        map.down_axis  = "+1";
        map.up_axis    = "-1";

        if (core_name.find("nestopia") != std::string::npos || core_name.find("fceumm") != std::string::npos) {
            map.name = "NES (PS-style)";
            map.b_btn       = "2"; // Cross -> RetroPad B (NES B, run)
            map.a_btn       = "1"; // Circle -> RetroPad A (NES A, jump)
            map.y_btn       = "3"; // Square -> RetroPad Y (turbo B)
            map.x_btn       = "0"; // Triangle -> RetroPad X (turbo A)
            map.select_btn  = "8";
            map.start_btn   = "9";
            map.extra_config = "nestopia_audio_vol_sq1 = \"100\"\n"
                               "nestopia_audio_vol_sq2 = \"100\"\n"
                               "nestopia_audio_vol_tri = \"100\"\n"
                               "nestopia_audio_vol_noise = \"100\"\n"
                               "nestopia_audio_vol_dpcm = \"100\"\n";

        } else if (core_name.find("snes9x") != std::string::npos) {
            map.name = "Super Nintendo (PS-style)";
            map.b_btn      = "2"; // Cross -> B (bottom)
            map.a_btn      = "1"; // Circle -> A (right)
            map.y_btn      = "3"; // Square -> Y (left)
            map.x_btn      = "0"; // Triangle -> X (top)
            map.l_btn      = "4"; // L1
            map.r_btn      = "5"; // R1
            map.select_btn = "8";
            map.start_btn  = "9";

        } else if (core_name.find("genesis_plus_gx") != std::string::npos) {
            map.name = "Sega Genesis (PS-style)";
            // 3-button: A B C on face; 6-button adds X Y Z on top row
            map.y_btn      = "3"; // Square -> RetroPad Y -> Genesis A (left)
            map.b_btn      = "2"; // Cross -> RetroPad B -> Genesis B (middle)
            map.a_btn      = "1"; // Circle -> RetroPad A -> Genesis C (right)
            map.x_btn      = "0"; // Triangle -> RetroPad X -> Genesis X (6-btn top)
            map.l_btn      = "4"; // L1 -> RetroPad L -> Genesis Y
            map.r_btn      = "5"; // R1 -> RetroPad R -> Genesis Z
            map.start_btn  = "9";

        } else if (core_name.find("pcsx") != std::string::npos ||
                   core_name.find("beetle_psx") != std::string::npos ||
                   core_name.find("swanstation") != std::string::npos) {
            map.name = "PS1 (PS-style, 1:1)";
            map.core_option_pad_type = "analog";
            map.b_btn      = "2"; // Cross -> RetroPad B (== PS1 Cross)
            map.a_btn      = "1"; // Circle -> RetroPad A (== PS1 Circle)
            map.y_btn      = "3"; // Square -> RetroPad Y (== PS1 Square)
            map.x_btn      = "0"; // Triangle -> RetroPad X (== PS1 Triangle)
            map.l_btn      = "4"; // L1
            map.r_btn      = "5"; // R1
            map.l2_btn     = "6"; // L2
            map.r2_btn     = "7"; // R2
            map.select_btn = "8";
            map.start_btn  = "9";

        } else if (core_name.find("prosystem") != std::string::npos) {
            map.name = "Atari 7800 (PS-style)";
            map.b_btn      = "2"; // Cross -> Button 1
            map.a_btn      = "1"; // Circle -> Button 2
            map.select_btn = "8";
            map.start_btn  = "9";

        } else if (core_name.find("mednafen_pce_fast") != std::string::npos) {
            map.name = "PC Engine (PS-style)";
            map.b_btn      = "2"; // Cross -> II (secondary)
            map.a_btn      = "1"; // Circle -> I  (primary, right on real PCE pad)
            map.y_btn      = "3"; // Square -> Turbo II
            map.x_btn      = "0"; // Triangle -> Turbo I
            map.select_btn = "8";
            map.start_btn  = "9";

        } else if (core_name.find("fbneo") != std::string::npos) {
            map.name = "Arcade / FBNeo (PS-style)";
            // Classic Capcom 6-button fighter: top row = punches, bottom row = kicks.
            //     Square Triangle R1     (1 = LP, 2 = MP, 3 = HP)
            //     Cross  Circle   R2     (4 = LK, 5 = MK, 6 = HK)  <- note: R1 is HP, R2 position unused here
            // RetroPad assignments (RetroArch's internal "arcade button N" indices):
            //   Y=1, X=2, L=3, B=4, A=5, R=6
            map.y_btn      = "3"; // Square -> RetroPad Y -> arcade 1 (LP)
            map.x_btn      = "0"; // Triangle -> RetroPad X -> arcade 2 (MP)
            map.l_btn      = "4"; // L1 -> RetroPad L -> arcade 3 (HP)
            map.b_btn      = "2"; // Cross -> RetroPad B -> arcade 4 (LK)
            map.a_btn      = "1"; // Circle -> RetroPad A -> arcade 5 (MK)
            map.r_btn      = "5"; // R1 -> RetroPad R -> arcade 6 (HK)
            map.select_btn = "8";
            map.start_btn  = "9";

        } else if (core_name.find("mupen64plus") != std::string::npos ||
                   core_name.find("parallel_n64") != std::string::npos) {
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
            map.name = "Nintendo 64 (PS-style)";
            map.analog_dpad_mode = "0";   // real analog stick, not dpad-emulation

            map.b_btn  = "2";   // Cross    -> RetroPad B -> N64 A (jump/confirm)
            map.a_btn  = "1";   // Circle   -> RetroPad A -> N64 B (secondary)
            map.l_btn  = "4";   // L1       -> N64 L
            map.r_btn  = "5";   // R1       -> N64 R
            map.l2_btn = "6";   // L2       -> N64 Z trigger (the underside one)
            map.start_btn = "9";

            // Left stick = N64 analog stick (1:1, no dpad emulation).
            map.l_x_plus = "+0"; map.l_x_minus = "-0";
            map.l_y_plus = "+1"; map.l_y_minus = "-1";
            // Right stick = C-button cluster.
            map.r_x_plus = "+2"; map.r_x_minus = "-2";
            map.r_y_plus = "+3"; map.r_y_minus = "-3";
            // D-pad stays on the hat; don't also drive it from the stick,
            // or analog input would double as D-pad presses in-game.
            map.up_axis = ""; map.down_axis = ""; map.left_axis = ""; map.right_axis = "";
            map.up_btn = "h0up"; map.down_btn = "h0down";
            map.left_btn = "h0left"; map.right_btn = "h0right";

        } else if (core_name.find("flycast") != std::string::npos) {
            // ---- Sega Dreamcast ----------------------------------------
            // Maps cleanly onto a modern pad: A/B/X/Y, one analog stick,
            // two analog triggers, Start. No Select — the DC controller
            // genuinely has no equivalent, so it is left unbound.
            //
            // UNVALIDATED ON HARDWARE — see the N64 note above.
            map.name = "Dreamcast (PS-style)";
            map.analog_dpad_mode = "0";

            map.b_btn  = "2";   // Cross    -> DC A
            map.a_btn  = "1";   // Circle   -> DC B
            map.y_btn  = "3";   // Square   -> DC X
            map.x_btn  = "0";   // Triangle -> DC Y
            map.l2_btn = "6";   // L2 -> DC left trigger (analog)
            map.r2_btn = "7";   // R2 -> DC right trigger (analog)
            map.start_btn = "9";

            map.l_x_plus = "+0"; map.l_x_minus = "-0";
            map.l_y_plus = "+1"; map.l_y_minus = "-1";
            map.up_btn = "h0up"; map.down_btn = "h0down";
            map.left_btn = "h0left"; map.right_btn = "h0right";
        }
        // else: leave map with defaults — shouldn't happen because every
        // shipped core matches one of the branches above.

        return map;
    }

}  // namespace

    ControllerMapping get_mapping(ControllerType type, const std::string& core_name) {
        switch (type) {
            case ControllerType::PS_STYLE_DRAGONRISE:
                return get_mapping_ps_style(core_name);
            case ControllerType::N64_ADAPTER:
            case ControllerType::UNKNOWN:
            default:
                return get_mapping_n64_adapter(core_name);
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
