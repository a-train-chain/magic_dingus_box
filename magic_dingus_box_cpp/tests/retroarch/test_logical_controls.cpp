#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

#include "retroarch/logical_controls.h"

using namespace retroarch;

TEST_CASE("logical control keys round-trip", "[logical_controls]") {
    REQUIRE(std::string(logical_control_key(LogicalControl::CROSS)) == "cross");
    REQUIRE(std::string(logical_control_key(LogicalControl::N64_C_UP)) == "n64_c_up");
    REQUIRE(logical_control_from_key("cross") == LogicalControl::CROSS);
    REQUIRE(logical_control_from_key("n64_stick_left") == LogicalControl::N64_STICK_LEFT);
    REQUIRE(!logical_control_from_key("bogus").has_value());
}

TEST_CASE("capture step lists are complete and style-pure", "[logical_controls]") {
    const auto ps = capture_steps(ControllerStyle::PS_STYLE);
    const auto n64 = capture_steps(ControllerStyle::N64_STYLE);
    REQUIRE(ps.size() == 24);
    REQUIRE(n64.size() == 18);
    // Style purity, by key prefix rather than by enum ordering: the "n64_"
    // prefix is the vocabulary's own stable marker (it is what the on-disk
    // profile JSON uses), whereas the enum order is now nothing but the
    // source's reading order.
    for (auto c : ps)
        REQUIRE(std::string(logical_control_key(c)).rfind("n64_", 0) != 0);
    for (auto c : n64)
        REQUIRE(std::string(logical_control_key(c)).rfind("n64_", 0) == 0);
    // D-pad first: the most universal control anchors the flow.
    REQUIRE(ps.front() == LogicalControl::DPAD_UP);
    REQUIRE(n64.front() == LogicalControl::N64_DPAD_UP);
}

TEST_CASE("required_controls is a strict subset of capture_steps",
          "[logical_controls]") {
    // The wizard's save gate. Anything named here that is not actually asked
    // for during capture would be unreachable and would block every save.
    for (auto style : {ControllerStyle::PS_STYLE, ControllerStyle::N64_STYLE}) {
        const auto steps = capture_steps(style);
        const auto required = required_controls(style);
        REQUIRE(required.size() == 6);
        for (auto c : required)
            REQUIRE(std::find(steps.begin(), steps.end(), c) != steps.end());
    }
    // The floor: four d-pad directions plus confirm and Start, in each
    // vocabulary. See the header for why nothing else is mandatory.
    const auto ps = required_controls(ControllerStyle::PS_STYLE);
    REQUIRE(std::find(ps.begin(), ps.end(), LogicalControl::CROSS) != ps.end());
    REQUIRE(std::find(ps.begin(), ps.end(), LogicalControl::START) != ps.end());
    const auto n64 = required_controls(ControllerStyle::N64_STYLE);
    REQUIRE(std::find(n64.begin(), n64.end(), LogicalControl::N64_A) != n64.end());
    REQUIRE(std::find(n64.begin(), n64.end(), LogicalControl::N64_START) != n64.end());
}

TEST_CASE("every control has a human prompt", "[logical_controls]") {
    for (auto style : {ControllerStyle::PS_STYLE, ControllerStyle::N64_STYLE})
        for (auto c : capture_steps(style))
            REQUIRE(!control_prompt(c).empty());
}

TEST_CASE("controller style keys round-trip", "[logical_controls]") {
    REQUIRE(std::string(controller_style_key(ControllerStyle::PS_STYLE)) == "ps_style");
    REQUIRE(std::string(controller_style_key(ControllerStyle::N64_STYLE)) == "n64_style");
    REQUIRE(controller_style_from_key("ps_style") == ControllerStyle::PS_STYLE);
    REQUIRE(controller_style_from_key("n64_style") == ControllerStyle::N64_STYLE);
    REQUIRE(!controller_style_from_key("bogus").has_value());
}
