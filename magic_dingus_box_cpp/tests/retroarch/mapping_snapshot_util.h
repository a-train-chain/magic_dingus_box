#pragma once
#include <sstream>
#include <string>
#include "retroarch/controller_mapping.h"

// Canonical one-string form of every ControllerMapping field. Field order
// is frozen — the golden strings in mapping_snapshot_golden.h depend on it.
inline std::string serialize_mapping(const retroarch::ControllerMapping& m) {
    std::ostringstream o;
    o << "name=" << m.name << "\n"
      << "adm=" << m.analog_dpad_mode << "|drv=" << m.input_driver
      << "|pad=" << m.core_option_pad_type << "\n"
      << "btn=" << m.b_btn << "," << m.y_btn << "," << m.select_btn << ","
      << m.start_btn << "," << m.a_btn << "," << m.x_btn << "," << m.l_btn
      << "," << m.r_btn << "," << m.l2_btn << "," << m.r2_btn << "\n"
      // Stick clicks get their OWN line rather than being appended to btn=
      // above. Appending would have made every golden's face-button line
      // change, so a genuine face-button regression could hide inside a line
      // the reviewer already expected to move; a new line leaves all 33
      // existing btn= lines byte-identical.
      << "l3r3=" << m.l3_btn << "," << m.r3_btn << "\n"
      << "dpad=" << m.up_btn << "," << m.down_btn << "," << m.left_btn << ","
      << m.right_btn << "\n"
      << "ls=" << m.l_x_plus << "," << m.l_x_minus << "," << m.l_y_plus << ","
      << m.l_y_minus << "\n"
      << "rs_axis=" << m.r_x_plus << "," << m.r_x_minus << "," << m.r_y_plus
      << "," << m.r_y_minus << "\n"
      << "rs_btn=" << m.r_x_plus_btn << "," << m.r_x_minus_btn << ","
      << m.r_y_plus_btn << "," << m.r_y_minus_btn << "\n"
      << "dpad_axis=" << m.up_axis << "," << m.down_axis << "," << m.left_axis
      << "," << m.right_axis << "\n"
      << "hotkeys=" << m.enable_hotkey_btn << "," << m.menu_toggle_btn << ","
      << m.exit_emulator_btn << "\n"
      << "extra=" << m.extra_config;
    return o.str();
}
