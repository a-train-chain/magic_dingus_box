// Automatic button layouts for modern pads (retroarch/standard_layout.h).
//
// Every popular controller today -- PS4/PS5, Xbox, Switch Pro, 8BitDo in
// any of its modes -- has the DualShock's physical shape: a four-button
// diamond, two shoulders and two triggers, Select/Start, two clickable
// sticks and a d-pad. So they all take the PS-style semantic tables; the
// only per-pad question is WHICH evdev code each physical position reports,
// and that is decided by the kernel driver, not by the pad.
//
// The fixtures below are each pad's real capability list under its Linux
// driver. The expected RetroArch tokens are cross-checked against
// RetroArch's own udev autoconfig profiles for the same pads (DualSense:
// a=1 b=0 x=2 y=3 l2=6 r2=7; Xbox 360/xpad: a=1 b=0 x=3 y=2, l2_axis=+2,
// r2_axis=+5, right stick +3/+4) -- RetroPad "b" is the BOTTOM button,
// "y" the LEFT one, "x" the TOP one.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "retroarch/controller_detector.h"
#include "retroarch/controller_mapping.h"
#include "retroarch/controller_profile.h"
#include "retroarch/standard_layout.h"

using namespace retroarch;
using K = PhysicalBinding::Kind;
using L = LogicalControl;

namespace {

CaptureDeviceCaps caps(const std::string& driver, uint16_t bus, uint16_t vid,
                       uint16_t pid, std::vector<uint16_t> keys,
                       std::vector<uint16_t> abs) {
    CaptureDeviceCaps c;
    c.driver = driver;
    c.bustype = bus;
    c.vid = vid;
    c.pid = pid;
    c.name = "pad";
    c.key_codes = std::move(keys);
    for (uint16_t a : abs) c.axes.push_back({a, -32768, 32767, 0});
    return c;
}

// hid-playstation, Bluetooth.
CaptureDeviceCaps dualsense() {
    return caps("playstation", 0x05, 0x054c, 0x0ce6,
                {0x130, 0x131, 0x133, 0x134, 0x136, 0x137, 0x138, 0x139, 0x13a,
                 0x13b, 0x13c, 0x13d, 0x13e},
                {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x10, 0x11});
}

// xpad, USB (also every 8BitDo pad in X-input mode, which reports 045e:028e).
CaptureDeviceCaps xbox360_xpad() {
    return caps("xpad", 0x03, 0x045e, 0x028e,
                {0x130, 0x131, 0x133, 0x134, 0x136, 0x137, 0x13a, 0x13b, 0x13c,
                 0x13d, 0x13e},
                {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x10, 0x11});
}

// Xbox Series / One (firmware 5.x, Bluetooth LE) through the generic HID
// mapping: buttons 1..15 land on 0x130..0x13e (the unused report slots
// 3/6/9/10 still exist as codes), right stick on Z/RZ, triggers on
// BRAKE (left) / GAS (right).
CaptureDeviceCaps xbox_series_ble() {
    std::vector<uint16_t> keys;
    for (uint16_t c = 0x130; c <= 0x13e; ++c) keys.push_back(c);
    return caps("microsoft", 0x05, 0x045e, 0x0b13, keys,
                {0x00, 0x01, 0x02, 0x05, 0x09, 0x0a, 0x10, 0x11});
}

// hid-nintendo, Bluetooth (8BitDo pads in Switch mode report 057e:2009 too).
CaptureDeviceCaps switch_pro() {
    return caps("nintendo", 0x05, 0x057e, 0x2009,
                {0x130, 0x131, 0x133, 0x134, 0x135, 0x136, 0x137, 0x138, 0x139,
                 0x13a, 0x13b, 0x13c, 0x13d, 0x13e},
                {0x00, 0x01, 0x03, 0x04, 0x10, 0x11});
}

void check_binding(const PhysicalProfile& p, L c, K kind, uint16_t code,
                   int dir, const std::string& token) {
    INFO("control " << logical_control_key(c));
    const PhysicalBinding* b = p.binding(c);
    REQUIRE(b != nullptr);
    CHECK(b->kind == kind);
    CHECK(b->code == code);
    CHECK(b->direction == dir);
    CHECK(b->token == token);
}

}  // namespace

TEST_CASE("standard layout: DualSense maps by position, PlayStation labels",
          "[retroarch][standard_layout]") {
    const auto p = standard_profile(dualsense());
    REQUIRE(p.has_value());
    CHECK(p->style == ControllerStyle::PS_STYLE);
    CHECK(p->vid == 0x054c);
    CHECK(p->pid == 0x0ce6);
    check_binding(*p, L::CROSS, K::BUTTON, 0x130, 0, "0");
    check_binding(*p, L::CIRCLE, K::BUTTON, 0x131, 0, "1");
    check_binding(*p, L::TRIANGLE, K::BUTTON, 0x133, 0, "2");
    check_binding(*p, L::SQUARE, K::BUTTON, 0x134, 0, "3");
    check_binding(*p, L::L1, K::BUTTON, 0x136, 0, "4");
    check_binding(*p, L::R1, K::BUTTON, 0x137, 0, "5");
    check_binding(*p, L::L2, K::BUTTON, 0x138, 0, "6");
    check_binding(*p, L::R2, K::BUTTON, 0x139, 0, "7");
    check_binding(*p, L::SELECT, K::BUTTON, 0x13a, 0, "8");
    check_binding(*p, L::START, K::BUTTON, 0x13b, 0, "9");
    check_binding(*p, L::L3, K::BUTTON, 0x13d, 0, "11");
    check_binding(*p, L::R3, K::BUTTON, 0x13e, 0, "12");
    check_binding(*p, L::DPAD_UP, K::HAT, 0x11, -1, "h0up");
    check_binding(*p, L::DPAD_DOWN, K::HAT, 0x11, +1, "h0down");
    check_binding(*p, L::DPAD_LEFT, K::HAT, 0x10, -1, "h0left");
    check_binding(*p, L::DPAD_RIGHT, K::HAT, 0x10, +1, "h0right");
    check_binding(*p, L::LSTICK_LEFT, K::AXIS, 0x00, -1, "-0");
    check_binding(*p, L::LSTICK_DOWN, K::AXIS, 0x01, +1, "+1");
    check_binding(*p, L::RSTICK_RIGHT, K::AXIS, 0x03, +1, "+3");
    check_binding(*p, L::RSTICK_UP, K::AXIS, 0x04, -1, "-4");
}

TEST_CASE("standard layout: Xbox (xpad) swaps the codes of the left and top buttons",
          "[retroarch][standard_layout]") {
    // xpad reports the Xbox X button (LEFT) as BTN_X == 0x133 and Y (TOP)
    // as BTN_Y == 0x134 -- the opposite of hid-playstation's positional
    // BTN_NORTH(0x133)/BTN_WEST(0x134).
    const auto p = standard_profile(xbox360_xpad());
    REQUIRE(p.has_value());
    check_binding(*p, L::CROSS, K::BUTTON, 0x130, 0, "0");
    check_binding(*p, L::CIRCLE, K::BUTTON, 0x131, 0, "1");
    check_binding(*p, L::SQUARE, K::BUTTON, 0x133, 0, "2");
    check_binding(*p, L::TRIANGLE, K::BUTTON, 0x134, 0, "3");
    check_binding(*p, L::SELECT, K::BUTTON, 0x13a, 0, "6");
    check_binding(*p, L::START, K::BUTTON, 0x13b, 0, "7");
    // Analog triggers, rest at the bottom of their range: pulled = "+".
    check_binding(*p, L::L2, K::AXIS, 0x02, +1, "+2");
    check_binding(*p, L::R2, K::AXIS, 0x05, +1, "+5");
    check_binding(*p, L::RSTICK_RIGHT, K::AXIS, 0x03, +1, "+3");
    check_binding(*p, L::RSTICK_DOWN, K::AXIS, 0x04, +1, "+4");
}

TEST_CASE("standard layout: Xbox over Bluetooth LE uses BRAKE/GAS and Z/RZ",
          "[retroarch][standard_layout]") {
    const auto p = standard_profile(xbox_series_ble());
    REQUIRE(p.has_value());
    check_binding(*p, L::SQUARE, K::BUTTON, 0x133, 0, "3");
    check_binding(*p, L::TRIANGLE, K::BUTTON, 0x134, 0, "4");
    check_binding(*p, L::L1, K::BUTTON, 0x136, 0, "6");
    check_binding(*p, L::SELECT, K::BUTTON, 0x13a, 0, "10");
    check_binding(*p, L::START, K::BUTTON, 0x13b, 0, "11");
    // NOT BTN_TL2/BTN_TR2: those codes exist (unused report slots 9/10) but
    // never fire on this pad.
    check_binding(*p, L::L2, K::AXIS, 0x0a, +1, "+5");
    check_binding(*p, L::R2, K::AXIS, 0x09, +1, "+4");
    check_binding(*p, L::RSTICK_RIGHT, K::AXIS, 0x02, +1, "+2");
    check_binding(*p, L::RSTICK_DOWN, K::AXIS, 0x05, +1, "+3");
}

TEST_CASE("standard layout: Switch Pro maps by position, not by its A/B labels",
          "[retroarch][standard_layout]") {
    // Nintendo's B is the BOTTOM button, so it is the Cross position --
    // which is also what a SNES game expects of it.
    const auto p = standard_profile(switch_pro());
    REQUIRE(p.has_value());
    check_binding(*p, L::CROSS, K::BUTTON, 0x130, 0, "0");
    check_binding(*p, L::CIRCLE, K::BUTTON, 0x131, 0, "1");
    check_binding(*p, L::TRIANGLE, K::BUTTON, 0x133, 0, "2");
    check_binding(*p, L::SQUARE, K::BUTTON, 0x134, 0, "3");
    check_binding(*p, L::L2, K::BUTTON, 0x138, 0, "7");
    check_binding(*p, L::RSTICK_RIGHT, K::AXIS, 0x03, +1, "+2");
}

TEST_CASE("standard layout: d-pad as buttons when the pad has no hat",
          "[retroarch][standard_layout]") {
    // xpad with dpad_to_buttons: BTN_TRIGGER_HAPPY1..4 = left, right, up, down.
    auto c = xbox360_xpad();
    c.axes.erase(c.axes.end() - 2, c.axes.end());  // drop HAT0X/HAT0Y
    for (uint16_t k = 0x2c0; k <= 0x2c3; ++k) c.key_codes.push_back(k);
    const auto p = standard_profile(c);
    REQUIRE(p.has_value());
    check_binding(*p, L::DPAD_LEFT, K::BUTTON, 0x2c0, 0, "11");
    check_binding(*p, L::DPAD_RIGHT, K::BUTTON, 0x2c1, 0, "12");
    check_binding(*p, L::DPAD_UP, K::BUTTON, 0x2c2, 0, "13");
    check_binding(*p, L::DPAD_DOWN, K::BUTTON, 0x2c3, 0, "14");

    // BTN_DPAD_UP/DOWN/LEFT/RIGHT (0x220..0x223) on a positional pad.
    auto d = dualsense();
    d.axes.erase(d.axes.end() - 2, d.axes.end());
    for (uint16_t k = 0x220; k <= 0x223; ++k) d.key_codes.push_back(k);
    const auto q = standard_profile(d);
    REQUIRE(q.has_value());
    check_binding(*q, L::DPAD_UP, K::BUTTON, 0x220, 0, "13");
    check_binding(*q, L::DPAD_RIGHT, K::BUTTON, 0x223, 0, "16");
}

TEST_CASE("standard layout: generic HID pads are left to Controller Setup",
          "[retroarch][standard_layout]") {
    // hid-generic numbers buttons in report order, which says nothing about
    // where they are on the pad -- an automatic layout would be a guess.
    auto c = dualsense();
    c.driver = "hid-generic";
    c.vid = 0x2dc8;
    CHECK_FALSE(standard_profile(c).has_value());

    // The shipped PS-style DragonRise pad (BTN_TRIGGER range).
    auto dr = caps("hid-generic", 0x03, 0x0079, 0x0006,
                   {0x120, 0x121, 0x122, 0x123, 0x124, 0x125, 0x126, 0x127, 0x128,
                    0x129, 0x12a, 0x12b},
                   {0x00, 0x01, 0x02, 0x05, 0x10, 0x11});
    CHECK_FALSE(standard_profile(dr).has_value());

    CHECK_FALSE(standard_profile(caps("", 0x03, 0, 0, {}, {})).has_value());
}

TEST_CASE("standard layout: Microsoft pads count as Xbox under the generic driver",
          "[retroarch][standard_layout]") {
    // hid-microsoft is not bound for every Xbox model; the VID still is.
    auto c = xbox_series_ble();
    c.driver = "hid-generic";
    const auto p = standard_profile(c);
    REQUIRE(p.has_value());
    check_binding(*p, L::SQUARE, K::BUTTON, 0x133, 0, "3");
}

TEST_CASE("standard layout: a known driver still needs the full button set",
          "[retroarch][standard_layout]") {
    // A single Joy-Con, an NSO N64 pad, an old-firmware Xbox One S: right
    // driver, wrong shape. Missing Select (or a d-pad) means "not a modern
    // full-size pad"; the wizard handles those.
    auto no_select = dualsense();
    no_select.key_codes.erase(std::find(no_select.key_codes.begin(),
                                        no_select.key_codes.end(), 0x13a));
    CHECK_FALSE(standard_profile(no_select).has_value());

    auto no_dpad = dualsense();
    no_dpad.axes.erase(no_dpad.axes.end() - 2, no_dpad.axes.end());
    CHECK_FALSE(standard_profile(no_dpad).has_value());
}

TEST_CASE("standard layout: a DetectedPad from sysfs resolves the same way",
          "[retroarch][standard_layout]") {
    DetectedPad pad;
    pad.vid = 0x054c;
    pad.pid = 0x0ce6;
    pad.name = "DualSense Wireless Controller";
    pad.driver = "playstation";
    pad.bustype = 0x05;
    pad.key_codes = dualsense().key_codes;
    pad.abs_codes = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x10, 0x11};
    const auto p = standard_profile(pad);
    REQUIRE(p.has_value());
    CHECK(p->token(L::SQUARE) == "3");
    CHECK(p->name == "DualSense Wireless Controller");
}

TEST_CASE("standard menu overlay: PS-style menu buttons, Square does nothing",
          "[retroarch][standard_layout]") {
    using platform::InputAction;
    const auto o = standard_menu_overlay(*standard_profile(dualsense()));
    CHECK(o.buttons.at(0x130) == InputAction::SELECT);         // Cross
    CHECK(o.buttons.at(0x131) == InputAction::SETTINGS_MENU);  // Circle
    CHECK(o.buttons.at(0x133) == InputAction::PLAY_PAUSE);     // Triangle
    CHECK(o.buttons.at(0x136) == InputAction::PREV);           // L1
    CHECK(o.buttons.at(0x137) == InputAction::NEXT);           // R1
    CHECK(o.buttons.at(0x13b) == InputAction::SELECT);         // Start
    // Square is BTN_WEST, which the built-in table reads as the N64
    // adapter's L (= previous). Claimed as NONE so it is inert.
    CHECK(o.buttons.at(0x134) == InputAction::NONE);
    CHECK(o.buttons.at(0x13a) == InputAction::NONE);           // Select
    CHECK(o.nav_x_abs == 0x00);
    CHECK(o.seek_abs == 0x03);
}

// ---------------------------------------------------------------------------
// Launch-time resolution
// ---------------------------------------------------------------------------

namespace {

DetectedPad detected(const CaptureDeviceCaps& c) {
    DetectedPad pad;
    pad.vid = c.vid;
    pad.pid = c.pid;
    pad.name = c.name;
    pad.driver = c.driver;
    pad.bustype = c.bustype;
    pad.key_codes = c.key_codes;
    for (const auto& a : c.axes) pad.abs_codes.push_back(a.code);
    return pad;
}

}  // namespace

TEST_CASE("resolve: a DualSense with no saved profile gets the PS1 layout 1:1",
          "[retroarch][standard_layout]") {
    const std::map<std::string, PhysicalProfile> store;
    const auto m = resolve_mapping_for_pad(detected(dualsense()), store,
                                           "pcsx_rearmed_libretro");
    CHECK(m.b_btn == "0");       // Cross
    CHECK(m.a_btn == "1");       // Circle
    CHECK(m.x_btn == "2");       // Triangle
    CHECK(m.y_btn == "3");       // Square
    CHECK(m.l2_btn == "6");
    CHECK(m.r2_btn == "7");
    CHECK(m.select_btn == "8");
    CHECK(m.start_btn == "9");
    CHECK(m.enable_hotkey_btn == "8");   // hold Select ...
    CHECK(m.exit_emulator_btn == "9");   // ... press Start = quit
}

TEST_CASE("resolve: a saved Controller Setup profile still wins over the automatic one",
          "[retroarch][standard_layout]") {
    PhysicalProfile mine = *standard_profile(dualsense());
    mine.controls[L::CROSS].token = "1";   // the owner swapped Cross/Circle
    mine.controls[L::CIRCLE].token = "0";
    std::map<std::string, PhysicalProfile> store{{vidpid_key(0x054c, 0x0ce6), mine}};
    const auto m = resolve_mapping_for_pad(detected(dualsense()), store,
                                           "pcsx_rearmed_libretro");
    CHECK(m.b_btn == "1");
    CHECK(m.a_btn == "0");
}

TEST_CASE("resolve: an unrecognized generic pad still takes the legacy path",
          "[retroarch][standard_layout]") {
    const std::map<std::string, PhysicalProfile> store;
    DetectedPad pad;
    pad.vid = 0x2563;
    pad.pid = 0x0575;
    pad.driver = "hid-generic";
    const auto by_pad = resolve_mapping_for_pad(pad, store, "snes9x2010_libretro");
    const auto legacy = resolve_mapping_for_pad(0x2563, 0x0575, store,
                                                "snes9x2010_libretro");
    CHECK(by_pad.b_btn == legacy.b_btn);
    CHECK(by_pad.a_btn == legacy.a_btn);
    CHECK(by_pad.name == legacy.name);
}

TEST_CASE("resolve: Xbox analog triggers drive N64 Z through an _axis bind",
          "[retroarch][standard_layout]") {
    // The PS-style N64 table puts N64 Z on L2. On an Xbox pad L2 is an
    // analog axis, so the RetroPad l2 slot must be emitted as
    // input_playerN_l2_axis -- a *_btn field cannot hold an axis.
    const std::map<std::string, PhysicalProfile> store;
    const auto m = resolve_mapping_for_pad(detected(xbox360_xpad()), store,
                                           "mupen64plus_next_libretro");
    CHECK(m.l2_btn.empty());
    REQUIRE(m.btn_axes.count("l2") == 1);
    CHECK(m.btn_axes.at("l2") == "+2");

    std::ostringstream o;
    write_player_binds(o, m, 1);
    const std::string cfg = o.str();
    CHECK(cfg.find("input_player1_l2_btn = \"nul\"") != std::string::npos);
    CHECK(cfg.find("input_player1_l2_axis = \"+2\"") != std::string::npos);
    // R2 is spent on N64 C-Down via the RetroPad a slot.
    CHECK(cfg.find("input_player1_a_axis = \"+5\"") != std::string::npos);
}

TEST_CASE("resolve: pads with button triggers emit no _axis lines",
          "[retroarch][standard_layout]") {
    const std::map<std::string, PhysicalProfile> store;
    const auto m = resolve_mapping_for_pad(detected(dualsense()), store,
                                           "mupen64plus_next_libretro");
    CHECK(m.btn_axes.empty());
    std::ostringstream o;
    write_player_binds(o, m, 1);
    CHECK(o.str().find("_btn_axis") == std::string::npos);
    CHECK(o.str().find("input_player1_l2_axis") == std::string::npos);
}
