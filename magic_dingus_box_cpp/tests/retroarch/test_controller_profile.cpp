#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

#include "retroarch/controller_mapping.h"
#include "retroarch/controller_profile.h"

using namespace retroarch;

namespace {

// Raw evdev EV_ABS codes (linux/input-event-codes.h numbering). Hardcoded
// here rather than included, since that header doesn't exist on macOS,
// where this test binary builds.
constexpr uint16_t kAbsX = 0x00;
constexpr uint16_t kAbsY = 0x01;
constexpr uint16_t kAbsZ = 0x02;
constexpr uint16_t kAbsRz = 0x05;
constexpr uint16_t kAbsHat0X = 0x10;
constexpr uint16_t kAbsHat0Y = 0x11;

using K = PhysicalBinding::Kind;

struct Expected {
    LogicalControl control;
    PhysicalBinding::Kind kind;
    uint16_t code;
    int direction;
    std::string token;
};

// Asserts every {kind, code, direction, token} in `expected` against the
// profile's actual binding, plus that the profile has exactly that many
// controls -- so a silently added or dropped binding fails the count check
// even if every listed one still matches.
void check_bindings(const PhysicalProfile& p, const std::vector<Expected>& expected) {
    REQUIRE(p.controls.size() == expected.size());
    for (const auto& e : expected) {
        INFO("control: " << logical_control_key(e.control));
        const PhysicalBinding* b = p.binding(e.control);
        REQUIRE(b != nullptr);
        CHECK(b->kind == e.kind);
        CHECK(b->code == e.code);
        CHECK(b->direction == e.direction);
        CHECK(b->token == e.token);
    }
}

}  // namespace

TEST_CASE("builtin N64 adapter profile pins every binding", "[controller_profile]") {
    const auto& p = builtin_n64_adapter_profile();
    REQUIRE(p.style == ControllerStyle::N64_STYLE);
    REQUIRE(p.vid == 0x0e6d);
    REQUIRE(p.pid == 0x111d);

    // Hardware-confirmed against a live pad (see controller_profile.cpp
    // and .superpowers/sdd/hardware-evidence.md): buttons 304..316
    // contiguous, real ABS_HAT0X/Y hat, ABS_X/Y stick.
    const std::vector<Expected> expected = {
        {LogicalControl::N64_C_LEFT,  K::BUTTON, 304, 0, "0"},
        {LogicalControl::N64_B,       K::BUTTON, 305, 0, "1"},
        {LogicalControl::N64_A,       K::BUTTON, 306, 0, "2"},
        {LogicalControl::N64_C_DOWN,  K::BUTTON, 307, 0, "3"},
        {LogicalControl::N64_L,       K::BUTTON, 308, 0, "4"},
        {LogicalControl::N64_R,       K::BUTTON, 309, 0, "5"},
        {LogicalControl::N64_Z,       K::BUTTON, 310, 0, "6"},
        {LogicalControl::N64_C_RIGHT, K::BUTTON, 312, 0, "8"},
        {LogicalControl::N64_C_UP,    K::BUTTON, 313, 0, "9"},
        {LogicalControl::N64_START,   K::BUTTON, 316, 0, "12"},
        {LogicalControl::N64_DPAD_UP,    K::HAT, kAbsHat0Y, -1, "h0up"},
        {LogicalControl::N64_DPAD_DOWN,  K::HAT, kAbsHat0Y, +1, "h0down"},
        {LogicalControl::N64_DPAD_LEFT,  K::HAT, kAbsHat0X, -1, "h0left"},
        {LogicalControl::N64_DPAD_RIGHT, K::HAT, kAbsHat0X, +1, "h0right"},
        {LogicalControl::N64_STICK_UP,    K::AXIS, kAbsY, -1, "-1"},
        {LogicalControl::N64_STICK_DOWN,  K::AXIS, kAbsY, +1, "+1"},
        {LogicalControl::N64_STICK_LEFT,  K::AXIS, kAbsX, -1, "-0"},
        {LogicalControl::N64_STICK_RIGHT, K::AXIS, kAbsX, +1, "+0"},
    };
    check_bindings(p, expected);

    // Missing control -> empty token, null binding
    REQUIRE(p.token(LogicalControl::CROSS) == "");
    REQUIRE(p.binding(LogicalControl::CROSS) == nullptr);
}

TEST_CASE("builtin DragonRise profile pins every binding", "[controller_profile]") {
    const auto& p = builtin_dragonrise_profile();
    REQUIRE(p.style == ControllerStyle::PS_STYLE);
    REQUIRE(p.vid == 0x0079);
    REQUIRE(p.pid == 0x0006);

    const std::vector<Expected> expected = {
        {LogicalControl::TRIANGLE, K::BUTTON, 288, 0, "0"},
        {LogicalControl::CIRCLE,   K::BUTTON, 289, 0, "1"},
        {LogicalControl::CROSS,    K::BUTTON, 290, 0, "2"},
        {LogicalControl::SQUARE,   K::BUTTON, 291, 0, "3"},
        {LogicalControl::L1,       K::BUTTON, 292, 0, "4"},
        {LogicalControl::R1,       K::BUTTON, 293, 0, "5"},
        {LogicalControl::L2,       K::BUTTON, 294, 0, "6"},
        {LogicalControl::R2,       K::BUTTON, 295, 0, "7"},
        {LogicalControl::SELECT,   K::BUTTON, 296, 0, "8"},
        {LogicalControl::START,    K::BUTTON, 297, 0, "9"},
        // PROVISIONAL / UNVERIFIED (see controller_profile.cpp caveat):
        // assumes a real hat; input_manager.cpp documents this same
        // VID/PID may instead use 8-bit ABS_X/Y extremes with no hat.
        // Settled by Task 12's controller_probe against the physical pad.
        {LogicalControl::DPAD_UP,    K::HAT, kAbsHat0Y, -1, "h0up"},
        {LogicalControl::DPAD_DOWN,  K::HAT, kAbsHat0Y, +1, "h0down"},
        {LogicalControl::DPAD_LEFT,  K::HAT, kAbsHat0X, -1, "h0left"},
        {LogicalControl::DPAD_RIGHT, K::HAT, kAbsHat0X, +1, "h0right"},
        {LogicalControl::LSTICK_UP,    K::AXIS, kAbsY, -1, "-1"},
        {LogicalControl::LSTICK_DOWN,  K::AXIS, kAbsY, +1, "+1"},
        {LogicalControl::LSTICK_LEFT,  K::AXIS, kAbsX, -1, "-0"},
        {LogicalControl::LSTICK_RIGHT, K::AXIS, kAbsX, +1, "+0"},
        // +2/+3 are the correct joydev axis indices for ABS_Z/ABS_RZ (not
        // merely "legacy" -- see controller_profile.cpp's caveat comment
        // and .superpowers/sdd/hardware-evidence.md).
        {LogicalControl::RSTICK_UP,    K::AXIS, kAbsRz, -1, "-3"},
        {LogicalControl::RSTICK_DOWN,  K::AXIS, kAbsRz, +1, "+3"},
        {LogicalControl::RSTICK_LEFT,  K::AXIS, kAbsZ, -1, "-2"},
        {LogicalControl::RSTICK_RIGHT, K::AXIS, kAbsZ, +1, "+2"},
    };
    check_bindings(p, expected);
}

TEST_CASE("vidpid_key formats 4-hex lowercase", "[controller_profile]") {
    REQUIRE(vidpid_key(0x0079, 0x0006) == "0079:0006");
    REQUIRE(vidpid_key(0x0e6d, 0x111d) == "0e6d:111d");
}

TEST_CASE("build_mapping resolves slots through the profile", "[build_mapping]") {
    SemanticMapping sem;
    sem.name = "T"; sem.analog_dpad_mode = "0";
    sem.b = LogicalControl::CROSS;
    sem.r_up = LogicalControl::RSTICK_UP; sem.r_down = LogicalControl::RSTICK_DOWN;
    sem.r_left = LogicalControl::RSTICK_LEFT; sem.r_right = LogicalControl::RSTICK_RIGHT;
    const auto m = build_mapping(sem, builtin_dragonrise_profile());
    REQUIRE(m.b_btn == "2");            // CROSS token
    REQUIRE(m.y_btn == "3");            // slot absent -> struct default kept
    REQUIRE(m.r_x_plus == "+2");        // AXIS kind -> axis form
    REQUIRE(m.r_x_plus_btn.empty());
}

TEST_CASE("build_mapping uses button form for a digital C cluster", "[build_mapping]") {
    SemanticMapping sem;
    sem.r_up = LogicalControl::N64_C_UP; sem.r_down = LogicalControl::N64_C_DOWN;
    sem.r_left = LogicalControl::N64_C_LEFT; sem.r_right = LogicalControl::N64_C_RIGHT;
    const auto m = build_mapping(sem, builtin_n64_adapter_profile());
    REQUIRE(m.r_y_minus_btn == "9");    // C-Up, BUTTON kind -> _btn form
    REQUIRE(m.r_x_plus.empty());
}

TEST_CASE("build_mapping unbinds slots the profile lacks", "[build_mapping]") {
    SemanticMapping sem;
    sem.l2 = LogicalControl::L2;        // DragonRise has it; a stickless capture may not
    PhysicalProfile p = builtin_dragonrise_profile();
    p.controls.erase(LogicalControl::L2);
    REQUIRE(build_mapping(sem, p).l2_btn.empty());   // "" = unbound, not default
}
