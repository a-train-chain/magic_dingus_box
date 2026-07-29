#include <catch2/catch_test_macros.hpp>
#include "retroarch/joydev_index.h"
#include "retroarch/controller_profile.h"

using namespace retroarch;
using K = PhysicalBinding::Kind;

TEST_CASE("button_index follows joydev two-range ordering", "[joydev_index]") {
    // A pad with codes both above and below BTN_JOYSTICK (0x120=288):
    // 0x120-range first (ascending), then the 0x100 BTN_MISC range wraps.
    const std::vector<uint16_t> keys = {0x100, 0x101, 0x130, 0x131, 0x13b};
    REQUIRE(button_index(keys, 0x130) == 0);
    REQUIRE(button_index(keys, 0x131) == 1);
    REQUIRE(button_index(keys, 0x13b) == 2);
    REQUIRE(button_index(keys, 0x100) == 3);   // BTN_MISC wraps AFTER
    REQUIRE(button_index(keys, 0x101) == 4);
    REQUIRE(button_index(keys, 0x999) == -1);
}

TEST_CASE("DragonRise-shaped pad gets contiguous indices", "[joydev_index]") {
    std::vector<uint16_t> keys;
    for (uint16_t c = 288; c <= 299; ++c) keys.push_back(c);
    REQUIRE(button_index(keys, 288) == 0);   // Triangle
    REQUIRE(button_index(keys, 290) == 2);   // Cross
    REQUIRE(button_index(keys, 297) == 9);   // Start
}

// EVERY vector above is already sorted, so std::sort inside button_index() /
// axis_index() is a no-op in all of them — deleting both sorts would leave
// them green. The capability lists this code consumes are built by walking
// codes in ascending order (controller_probe.cpp, InputManager::device_caps),
// which is exactly why the sort has never been exercised. It is still load
// bearing: nothing in the SIGNATURES promises ordered input, and a future
// caller that assembles a list some other way (a profile replayed from JSON,
// a hand-written test fixture) would silently get wrong indices without it.
TEST_CASE("indices do not depend on the caller's ordering", "[joydev_index]") {
    // Same two pads as above, shuffled.
    const std::vector<uint16_t> keys = {0x13b, 0x101, 0x130, 0x100, 0x131};
    REQUIRE(button_index(keys, 0x130) == 0);
    REQUIRE(button_index(keys, 0x131) == 1);
    REQUIRE(button_index(keys, 0x13b) == 2);
    REQUIRE(button_index(keys, 0x100) == 3);   // BTN_MISC still wraps AFTER
    REQUIRE(button_index(keys, 0x101) == 4);

    const std::vector<uint16_t> abs = {5, 17, 0, 16, 2, 1};
    REQUIRE(axis_index(abs, 0) == 0);
    REQUIRE(axis_index(abs, 1) == 1);
    REQUIRE(axis_index(abs, 2) == 2);
    REQUIRE(axis_index(abs, 5) == 3);
    REQUIRE(axis_index(abs, 16) == -1);

    // ...and the tokens built on top of them agree with the sorted case.
    REQUIRE(bind_token(keys, abs, K::AXIS, 5, +1) == "+3");
    REQUIRE(bind_token(keys, abs, K::BUTTON, 0x13b, 0) == "2");
}

TEST_CASE("axis_index skips hats", "[joydev_index]") {
    // ABS_X(0), ABS_Y(1), ABS_Z(2), ABS_RZ(5), ABS_HAT0X(16), ABS_HAT0Y(17)
    const std::vector<uint16_t> abs = {0, 1, 2, 5, 16, 17};
    REQUIRE(axis_index(abs, 0) == 0);
    REQUIRE(axis_index(abs, 2) == 2);
    REQUIRE(axis_index(abs, 5) == 3);
    REQUIRE(axis_index(abs, 16) == -1);      // hat is not an axis
    REQUIRE(hat_number(16) == 0);
    REQUIRE(hat_number(17) == 0);
    REQUIRE(hat_number(0x14) == 2);          // ABS_HAT2X
    REQUIRE(hat_number(1) == -1);
}

TEST_CASE("bind_token formats all three kinds", "[joydev_index]") {
    std::vector<uint16_t> keys; for (uint16_t c = 288; c <= 299; ++c) keys.push_back(c);
    const std::vector<uint16_t> abs = {0, 1, 2, 5, 16, 17};
    REQUIRE(bind_token(keys, abs, K::BUTTON, 290, 0) == "2");
    REQUIRE(bind_token(keys, abs, K::HAT, 17, -1) == "h0up");
    REQUIRE(bind_token(keys, abs, K::HAT, 16, +1) == "h0right");
    REQUIRE(bind_token(keys, abs, K::AXIS, 5, +1) == "+3");
    REQUIRE(bind_token(keys, abs, K::AXIS, 1, -1) == "-1");
    REQUIRE(bind_token(keys, abs, K::BUTTON, 999, 0) == "");
}

// --- Real captured hardware -------------------------------------------
//
// Captured 2026-07-29 from the live production Pi 5 (magic@magicpi5.local),
// see .superpowers/sdd/hardware-evidence.md. Pad: "SWITCH CO.,LTD.
// Controller (Dinput)", VID/PID 2563:0575 (an N64-style adapter, resolves
// to ControllerType::UNKNOWN in controller_detector.cpp today but falls
// through to the N64 adapter mapping).
//
// Raw /proc/bus/input/devices capability bitmaps for this pad:
//   B: EV=1b
//   B: KEY=1fff000000000000 0 0 0 0   -> bits 304..316 inclusive (13 buttons,
//                                        BTN_SOUTH..BTN_THUMBR), contiguous.
//   B: ABS=30027                       -> bits 0,1,2,5,16,17 -> ABS_X, ABS_Y,
//                                        ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y.
//
// This is not a synthetic case: these are exactly the tokens the shipped,
// hardware-validated builtin_n64_adapter_profile() mapping uses for this
// pad (A=306->"2", Z=310->"6", C-Right=312->"8", C-Up=313->"9",
// Start=316->"12"). All codes fall in the high (>= BTN_JOYSTICK) range, so
// button ordering is simple ascending index == code-304 here; this test
// does not exercise the BTN_MISC-wrap branch (covered above).
TEST_CASE("real N64-adapter capture (2563:0575) reproduces shipped tokens",
          "[joydev_index]") {
    std::vector<uint16_t> keys;
    for (uint16_t c = 304; c <= 316; ++c) keys.push_back(c);
    const std::vector<uint16_t> abs = {0, 1, 2, 5, 16, 17};

    REQUIRE(bind_token(keys, abs, K::BUTTON, 304, 0) == "0");
    REQUIRE(bind_token(keys, abs, K::BUTTON, 306, 0) == "2");
    REQUIRE(bind_token(keys, abs, K::BUTTON, 310, 0) == "6");
    REQUIRE(bind_token(keys, abs, K::BUTTON, 312, 0) == "8");
    REQUIRE(bind_token(keys, abs, K::BUTTON, 313, 0) == "9");
    REQUIRE(bind_token(keys, abs, K::BUTTON, 316, 0) == "12");

    REQUIRE(bind_token(keys, abs, K::AXIS, 0, +1) == "+0");
    REQUIRE(bind_token(keys, abs, K::AXIS, 0, -1) == "-0");
    REQUIRE(bind_token(keys, abs, K::AXIS, 1, +1) == "+1");
    REQUIRE(bind_token(keys, abs, K::AXIS, 1, -1) == "-1");
    REQUIRE(bind_token(keys, abs, K::AXIS, 2, +1) == "+2");
    REQUIRE(bind_token(keys, abs, K::AXIS, 2, -1) == "-2");
    REQUIRE(bind_token(keys, abs, K::AXIS, 5, +1) == "+3");
    REQUIRE(bind_token(keys, abs, K::AXIS, 5, -1) == "-3");

    REQUIRE(bind_token(keys, abs, K::HAT, 17, -1) == "h0up");
    REQUIRE(bind_token(keys, abs, K::HAT, 16, +1) == "h0right");
}
