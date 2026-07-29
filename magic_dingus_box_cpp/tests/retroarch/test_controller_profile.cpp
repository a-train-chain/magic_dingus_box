#include <catch2/catch_test_macros.hpp>
#include "retroarch/controller_profile.h"

using namespace retroarch;

TEST_CASE("builtin N64 adapter profile reproduces the legacy tokens",
          "[controller_profile]") {
    const auto& p = builtin_n64_adapter_profile();
    REQUIRE(p.style == ControllerStyle::N64_STYLE);
    REQUIRE(p.vid == 0x0e6d); REQUIRE(p.pid == 0x111d);
    // Tokens transcribed from controller_mapping.cpp's physical table:
    // 0=C-Left, 1=B, 2=A, 3=C-Down, 4=L, 5=R, 6=Z, 8=C-Right, 9=C-Up, 12=Start
    REQUIRE(p.token(LogicalControl::N64_A) == "2");
    REQUIRE(p.token(LogicalControl::N64_B) == "1");
    REQUIRE(p.token(LogicalControl::N64_Z) == "6");
    REQUIRE(p.token(LogicalControl::N64_L) == "4");
    REQUIRE(p.token(LogicalControl::N64_R) == "5");
    REQUIRE(p.token(LogicalControl::N64_START) == "12");
    REQUIRE(p.token(LogicalControl::N64_C_LEFT) == "0");
    REQUIRE(p.token(LogicalControl::N64_C_DOWN) == "3");
    REQUIRE(p.token(LogicalControl::N64_C_RIGHT) == "8");
    REQUIRE(p.token(LogicalControl::N64_C_UP) == "9");
    REQUIRE(p.token(LogicalControl::N64_DPAD_UP) == "h0up");
    REQUIRE(p.token(LogicalControl::N64_STICK_UP) == "-1");
    REQUIRE(p.token(LogicalControl::N64_STICK_RIGHT) == "+0");
    // evdev codes for the kiosk-menu overlay (code = 304 + index on this pad)
    REQUIRE(p.binding(LogicalControl::N64_A)->code == 306);
    REQUIRE(p.binding(LogicalControl::N64_B)->code == 305);
    REQUIRE(p.binding(LogicalControl::N64_START)->code == 316);
    // Missing control -> empty token, null binding
    REQUIRE(p.token(LogicalControl::CROSS) == "");
    REQUIRE(p.binding(LogicalControl::CROSS) == nullptr);
}

TEST_CASE("builtin DragonRise profile reproduces the legacy tokens",
          "[controller_profile]") {
    const auto& p = builtin_dragonrise_profile();
    REQUIRE(p.style == ControllerStyle::PS_STYLE);
    // 0=Triangle 1=Circle 2=Cross 3=Square 4=L1 5=R1 6=L2 7=R2 8=Select 9=Start
    REQUIRE(p.token(LogicalControl::TRIANGLE) == "0");
    REQUIRE(p.token(LogicalControl::CIRCLE) == "1");
    REQUIRE(p.token(LogicalControl::CROSS) == "2");
    REQUIRE(p.token(LogicalControl::SQUARE) == "3");
    REQUIRE(p.token(LogicalControl::L1) == "4");
    REQUIRE(p.token(LogicalControl::R2) == "7");
    REQUIRE(p.token(LogicalControl::SELECT) == "8");
    REQUIRE(p.token(LogicalControl::START) == "9");
    REQUIRE(p.token(LogicalControl::DPAD_LEFT) == "h0left");
    REQUIRE(p.token(LogicalControl::LSTICK_DOWN) == "+1");
    // Legacy right-stick tokens are +2/+3 (see get_mapping_ps_style N64 branch)
    REQUIRE(p.token(LogicalControl::RSTICK_RIGHT) == "+2");
    REQUIRE(p.token(LogicalControl::RSTICK_DOWN) == "+3");
    // evdev codes: code = 288 + index (BTN_TRIGGER range, per input_manager.cpp)
    REQUIRE(p.binding(LogicalControl::CROSS)->code == 290);
    REQUIRE(p.binding(LogicalControl::START)->code == 297);
}

TEST_CASE("vidpid_key formats 4-hex lowercase", "[controller_profile]") {
    REQUIRE(vidpid_key(0x0079, 0x0006) == "0079:0006");
    REQUIRE(vidpid_key(0x0e6d, 0x111d) == "0e6d:111d");
}
