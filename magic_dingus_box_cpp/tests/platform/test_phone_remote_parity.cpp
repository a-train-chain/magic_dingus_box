// Phone remote <-> kiosk button parity.
//
// The phone remote's buttons reach the kiosk as evdev events from a uinput
// virtual gamepad written by magic_dingus_box/web/remote/uinput_writer.py.
// Those codes mean nothing on their own: they work only because the
// kiosk's built-in joystick table (platform/input_mapping.cpp) happens to
// map each one to the action printed on the phone's button. Nothing tied
// the two files together, so a renumbered code on either side would turn a
// phone button into a dead (or wrong) key with no failing test.
//
// This test PARSES the python source (its constants and its _MAP table)
// and feeds every code through the kiosk's real table. A button added on
// the phone without an expectation below fails the test on purpose.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>

#include "platform/input_mapping.h"

namespace fs = std::filesystem;
using platform::InputAction;
namespace mapping = platform::mapping;

namespace {

struct PhoneMap {
    std::map<std::string, int> constants;                     // BTN_SOUTH -> 304
    std::map<std::string, std::string> key_buttons;           // OK -> BTN_SOUTH
    std::map<std::string, std::pair<std::string, int>> axis_buttons;  // UP -> (ABS_HAT0Y, -1)
    std::vector<std::string> button_names;                    // ButtonName members
};

std::string read_writer_source() {
    // tests/platform -> magic_dingus_box_cpp -> repo root
    const fs::path p = fs::path(__FILE__).parent_path().parent_path().parent_path()
                           .parent_path() / "magic_dingus_box" / "web" / "remote" /
                       "uinput_writer.py";
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

PhoneMap parse(const std::string& src) {
    PhoneMap m;
    const std::regex constant(R"(^(BTN_\w+|KEY_\w+|ABS_\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+))");
    const std::regex key_entry(R"(ButtonName\.(\w+)\s*:\s*_KeyEvent\((\w+)\))");
    const std::regex axis_entry(R"(ButtonName\.(\w+)\s*:\s*_AxisEvent\((\w+)\s*,\s*(-?\d+)\))");
    const std::regex enum_member(R"re(^\s+(\w+)\s*=\s*"(\w+)"\s*$)re");

    std::istringstream lines(src);
    std::string line;
    bool in_button_enum = false;
    while (std::getline(lines, line)) {
        std::smatch mm;
        if (std::regex_search(line, mm, constant)) {
            m.constants[mm[1]] = std::stoi(mm[2], nullptr, 0);
        }
        if (line.rfind("class ButtonName", 0) == 0) { in_button_enum = true; continue; }
        if (in_button_enum) {
            if (!line.empty() && line[0] != ' ') in_button_enum = false;
            else if (std::regex_search(line, mm, enum_member)) m.button_names.push_back(mm[1]);
        }
        if (std::regex_search(line, mm, axis_entry)) {
            m.axis_buttons[mm[1]] = {mm[2], std::stoi(mm[3])};
        } else if (std::regex_search(line, mm, key_entry)) {
            m.key_buttons[mm[1]] = mm[2];
        }
    }
    return m;
}

}  // namespace

TEST_CASE("phone remote: every button lands on the action it is labelled with",
          "[phone_remote_parity]") {
    const std::string src = read_writer_source();
    // A missing file is a failure, not a skip — a skip is how a parity
    // check goes silent.
    REQUIRE_FALSE(src.empty());
    const PhoneMap m = parse(src);
    REQUIRE(m.button_names.size() >= 10u);   // the parser actually found the enum

    // The intent of each phone button, from the phone UI's labels.
    const std::map<std::string, InputAction> key_intent = {
        {"OK", InputAction::SELECT},              // gold centre key
        {"BLACK", InputAction::SETTINGS_MENU},    // MENU
        {"YELLOW", InputAction::PREV},
        {"GREEN", InputAction::NEXT},
        {"RED", InputAction::PLAY_PAUSE},
    };
    // D-pad: (hat axis, value). The kiosk's hat branches emit
    // ROTATE (X) / ROTATE_VERTICAL (Y) with delta == value, so -1 on Y is UP.
    const std::map<std::string, std::pair<int, int>> axis_intent = {
        {"UP", {mapping::codes::kAbsHat0Y, -1}},
        {"DOWN", {mapping::codes::kAbsHat0Y, +1}},
        {"LEFT", {mapping::codes::kAbsHat0X, -1}},
        {"RIGHT", {mapping::codes::kAbsHat0X, +1}},
    };

    for (const auto& name : m.button_names) {
        INFO("phone button " << name);
        if (name == "QUIT_GAME") continue;   // the RetroArch chord, below
        const bool is_key = m.key_buttons.count(name) > 0;
        const bool is_axis = m.axis_buttons.count(name) > 0;
        REQUIRE((is_key || is_axis));        // every button has a _MAP entry
        if (is_key) {
            REQUIRE(key_intent.count(name) == 1);   // new button: add its intent
            const std::string& sym = m.key_buttons.at(name);
            REQUIRE(m.constants.count(sym) == 1);
            CHECK(mapping::joystick_button_action(
                      static_cast<uint16_t>(m.constants.at(sym))) == key_intent.at(name));
        } else {
            REQUIRE(axis_intent.count(name) == 1);
            const auto& [sym, value] = m.axis_buttons.at(name);
            REQUIRE(m.constants.count(sym) == 1);
            CHECK(m.constants.at(sym) == axis_intent.at(name).first);
            CHECK(value == axis_intent.at(name).second);
        }
    }
}

TEST_CASE("phone remote: the quit-game chord is inert on the kiosk menu",
          "[phone_remote_parity]") {
    // QUIT_GAME sends KEY_Z + BTN_START for RetroArch's exit hotkey. Both
    // must map to NOTHING in the kiosk, or tapping it at the menu would
    // also fire a menu action.
    const PhoneMap m = parse(read_writer_source());
    REQUIRE(m.constants.count("KEY_Z") == 1);
    REQUIRE(m.constants.count("BTN_START") == 1);
    CHECK(m.constants.at("BTN_START") == mapping::codes::kBtnStart);
    CHECK(mapping::joystick_button_action(
              static_cast<uint16_t>(m.constants.at("KEY_Z"))) == InputAction::NONE);
    CHECK(mapping::joystick_button_action(
              static_cast<uint16_t>(m.constants.at("BTN_START"))) == InputAction::NONE);
}

TEST_CASE("kiosk built-in tables", "[input_mapping]") {
    namespace c = mapping::codes;
    // N64 adapter
    CHECK(mapping::joystick_button_action(c::kBtnC) == InputAction::SELECT);
    CHECK(mapping::joystick_button_action(c::kBtnMode) == InputAction::SELECT);
    CHECK(mapping::joystick_button_action(c::kBtnTl) == InputAction::PLAY_PAUSE);
    CHECK(mapping::joystick_button_action(c::kBtnZ) == InputAction::NEXT);
    CHECK(mapping::joystick_button_action(c::kBtnWest) == InputAction::PREV);
    // PS-style pad (Cross confirms, Circle opens Settings)
    CHECK(mapping::joystick_button_action(c::kBtnThumb2) == InputAction::SELECT);
    CHECK(mapping::joystick_button_action(c::kBtnBase4) == InputAction::SELECT);
    CHECK(mapping::joystick_button_action(c::kBtnThumb) == InputAction::SETTINGS_MENU);
    CHECK(mapping::joystick_button_action(c::kBtnTrigger) == InputAction::PLAY_PAUSE);
    CHECK(mapping::joystick_button_action(c::kBtnTop2) == InputAction::PREV);
    CHECK(mapping::joystick_button_action(c::kBtnPinkie) == InputAction::NEXT);
    CHECK(mapping::joystick_button_action(291) == InputAction::NONE);   // Square: unassigned
    CHECK(mapping::joystick_button_action(0) == InputAction::NONE);
    // Keyboard
    CHECK(mapping::keyboard_key_action(c::kKeyEnter) == InputAction::SELECT);
    CHECK(mapping::keyboard_key_action(c::kKeySpace) == InputAction::SELECT);
    CHECK(mapping::keyboard_key_action(c::kKeyN) == InputAction::NEXT);
    CHECK(mapping::keyboard_key_action(c::kKeyP) == InputAction::PREV);
    CHECK(mapping::keyboard_key_action(c::kKeyPlayPause) == InputAction::PLAY_PAUSE);
    CHECK(mapping::keyboard_key_action(c::kKeyEsc) == InputAction::QUIT);
    CHECK(mapping::keyboard_key_action(c::kKeyQ) == InputAction::QUIT);
    CHECK(mapping::keyboard_key_action(44) == InputAction::NONE);   // KEY_Z
}
