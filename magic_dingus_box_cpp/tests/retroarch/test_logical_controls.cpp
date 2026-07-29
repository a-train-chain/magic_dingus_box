#include <catch2/catch_test_macros.hpp>
#include "retroarch/logical_controls.h"

using namespace retroarch;

TEST_CASE("logical control keys round-trip", "[logical_controls]") {
    REQUIRE(std::string(logical_control_key(LogicalControl::CROSS)) == "cross");
    REQUIRE(std::string(logical_control_key(LogicalControl::N64_C_UP)) == "n64_c_up");
    REQUIRE(logical_control_from_key("cross") == LogicalControl::CROSS);
    REQUIRE(logical_control_from_key("n64_stick_left") == LogicalControl::N64_STICK_LEFT);
    REQUIRE(!logical_control_from_key("bogus").has_value());
}

TEST_CASE("style_of splits the vocabularies", "[logical_controls]") {
    REQUIRE(style_of(LogicalControl::TRIANGLE) == ControllerStyle::PS_STYLE);
    REQUIRE(style_of(LogicalControl::N64_Z) == ControllerStyle::N64_STYLE);
}

TEST_CASE("capture step lists are complete and style-pure", "[logical_controls]") {
    const auto ps = capture_steps(ControllerStyle::PS_STYLE);
    const auto n64 = capture_steps(ControllerStyle::N64_STYLE);
    REQUIRE(ps.size() == 24);
    REQUIRE(n64.size() == 18);
    for (auto c : ps) REQUIRE(style_of(c) == ControllerStyle::PS_STYLE);
    for (auto c : n64) REQUIRE(style_of(c) == ControllerStyle::N64_STYLE);
    // D-pad first: the most universal control anchors the flow.
    REQUIRE(ps.front() == LogicalControl::DPAD_UP);
    REQUIRE(n64.front() == LogicalControl::N64_DPAD_UP);
}

TEST_CASE("every control has a human prompt", "[logical_controls]") {
    for (auto style : {ControllerStyle::PS_STYLE, ControllerStyle::N64_STYLE})
        for (auto c : capture_steps(style))
            REQUIRE(!control_prompt(c).empty());
}
