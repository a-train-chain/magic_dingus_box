#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "platform/input_manager.h"
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

// Regression test for Fix 1: semantic_ps_style's nestopia branch never sets
// up/down/left/right, so before the fix build_mapping left those
// ControllerMapping fields at their struct defaults ("h0up"/"h0down"/
// "h0left"/"h0right") no matter what the profile said. That was invisible
// with the built-in DragonRise profile, whose d-pad really is a hat bound
// to those exact tokens, but would silently break a captured pad whose
// d-pad is an axis pair instead of a hat.
TEST_CASE("build_mapping resolves the d-pad through the profile even when "
          "the core never binds it explicitly",
          "[build_mapping]") {
    PhysicalProfile profile = builtin_dragonrise_profile();
    profile.controls[LogicalControl::DPAD_UP] =
        {K::AXIS, kAbsY, -1, "-1"};
    profile.controls[LogicalControl::DPAD_DOWN] =
        {K::AXIS, kAbsY, +1, "+1"};
    profile.controls[LogicalControl::DPAD_LEFT] =
        {K::AXIS, kAbsX, -1, "-0"};
    profile.controls[LogicalControl::DPAD_RIGHT] =
        {K::AXIS, kAbsX, +1, "+0"};

    const auto sem = get_semantic_mapping(ControllerStyle::PS_STYLE, "nestopia_libretro");
    const auto m = build_mapping(sem, profile);
    REQUIRE(m.up_btn == "-1");
    REQUIRE(m.up_btn != "h0up");
    REQUIRE(m.down_btn == "+1");
    REQUIRE(m.left_btn == "-0");
    REQUIRE(m.right_btn == "+0");
}

// Regression test for Fix 1's stick half: semantic_n64_style's nestopia
// branch called stick() but never set left_stick, so before the fix
// build_mapping left l_x_plus/l_x_minus/l_y_plus/l_y_minus at their struct
// defaults ("+0"/"-0"/"+1"/"-1") regardless of the profile's stick tokens.
TEST_CASE("build_mapping resolves the left stick through the profile even "
          "when the core leaves left_stick unset",
          "[build_mapping]") {
    PhysicalProfile profile = builtin_n64_adapter_profile();
    profile.controls[LogicalControl::N64_STICK_RIGHT] =
        {K::AXIS, kAbsX, +1, "+9"};
    profile.controls[LogicalControl::N64_STICK_LEFT] =
        {K::AXIS, kAbsX, -1, "-9"};
    profile.controls[LogicalControl::N64_STICK_DOWN] =
        {K::AXIS, kAbsY, +1, "+8"};
    profile.controls[LogicalControl::N64_STICK_UP] =
        {K::AXIS, kAbsY, -1, "-8"};

    const auto sem = get_semantic_mapping(ControllerStyle::N64_STYLE, "nestopia_libretro");
    const auto m = build_mapping(sem, profile);
    REQUIRE(m.l_x_plus == "+9");
    REQUIRE(m.l_x_plus != "+0");
    REQUIRE(m.l_x_minus == "-9");
    REQUIRE(m.l_y_plus == "+8");
    REQUIRE(m.l_y_minus == "-8");
}

// Regression test for Fix 2: a wizard capture that got r_up/r_down/r_left
// but skipped r_right used to still emit r_x_minus/r_y_plus/r_y_minus from
// whatever profile.token() returned for the three present controls, next
// to an empty r_x_plus -- an incoherent, partially-bound right stick.
TEST_CASE("build_mapping leaves right stick fully unbound on a partial capture",
          "[build_mapping]") {
    SemanticMapping sem;
    sem.r_up = LogicalControl::RSTICK_UP; sem.r_down = LogicalControl::RSTICK_DOWN;
    sem.r_left = LogicalControl::RSTICK_LEFT; sem.r_right = LogicalControl::RSTICK_RIGHT;
    PhysicalProfile p = builtin_dragonrise_profile();
    p.controls.erase(LogicalControl::RSTICK_RIGHT);  // wizard capture skipped this one

    const auto m = build_mapping(sem, p);
    REQUIRE(m.r_x_plus.empty());
    REQUIRE(m.r_x_minus.empty());
    REQUIRE(m.r_y_plus.empty());
    REQUIRE(m.r_y_minus.empty());
    REQUIRE(m.r_x_plus_btn.empty());
    REQUIRE(m.r_x_minus_btn.empty());
    REQUIRE(m.r_y_plus_btn.empty());
    REQUIRE(m.r_y_minus_btn.empty());
}

// Regression test for Fix 2's mixed-kind case: one of the four right-stick
// controls captured as a BUTTON while the other three are AXIS used to
// still take the axis branch (keyed off r_up alone) and write a button
// token into an axis field.
TEST_CASE("build_mapping leaves right stick fully unbound on a mixed-kind capture",
          "[build_mapping]") {
    SemanticMapping sem;
    sem.r_up = LogicalControl::RSTICK_UP; sem.r_down = LogicalControl::RSTICK_DOWN;
    sem.r_left = LogicalControl::RSTICK_LEFT; sem.r_right = LogicalControl::RSTICK_RIGHT;
    PhysicalProfile p = builtin_dragonrise_profile();
    // r_up/r_down/r_left are AXIS (from the built-in profile); make
    // r_right a BUTTON, as if that corner of the capture landed on a
    // digital control instead of an axis.
    p.controls[LogicalControl::RSTICK_RIGHT] = {K::BUTTON, 0, 0, "7"};

    const auto m = build_mapping(sem, p);
    REQUIRE(m.r_x_plus.empty());
    REQUIRE(m.r_x_minus.empty());
    REQUIRE(m.r_y_plus.empty());
    REQUIRE(m.r_y_minus.empty());
    REQUIRE(m.r_x_plus_btn.empty());
    REQUIRE(m.r_x_minus_btn.empty());
    REQUIRE(m.r_y_plus_btn.empty());
    REQUIRE(m.r_y_minus_btn.empty());
}

TEST_CASE("profiles round-trip through JSON", "[controller_profile][json]") {
    std::map<std::string, PhysicalProfile> in;
    PhysicalProfile p = builtin_dragonrise_profile();
    p.vid = 0x0810; p.pid = 0xe501; p.name = "Twin USB"; p.captured_at = "2026-07-28T21:00:00Z";
    in[vidpid_key(p.vid, p.pid)] = p;
    const auto out = profiles_from_json(profiles_to_json(in));
    REQUIRE(out.size() == 1);
    const auto& q = out.at("0810:e501");
    REQUIRE(q.name == "Twin USB");
    REQUIRE(q.style == ControllerStyle::PS_STYLE);
    REQUIRE(q.vid == 0x0810); REQUIRE(q.pid == 0xe501);
    REQUIRE(q.token(LogicalControl::CROSS) == "2");
    REQUIRE(q.binding(LogicalControl::DPAD_UP)->kind == PhysicalBinding::Kind::HAT);
    REQUIRE(q.binding(LogicalControl::DPAD_UP)->direction == -1);
}

// Regression test for Fix 1: profiles_from_json used to key its result map
// by the RAW JSON object key text rather than the canonical
// vidpid_key(vid, pid) it had just parsed that key into. vidpid_key() always
// emits lowercase hex, and resolve_mapping_for_pad's lookup
// (controller_mapping.cpp) always builds its lookup key the same way -- but
// the key parser above accepts upper- and lower-case hex equally. Before the
// fix, an uppercase-hex key parsed successfully but was silently unreachable
// by lookup, so resolution fell all the way through to the built-in/legacy
// mapping with no error.
TEST_CASE("a profile captured under an uppercase-hex key is still found by "
          "resolve_mapping_for_pad, and beats the built-in profile",
          "[controller_profile][json]") {
    // 0x0e6d:0x111d is the built-in N64 adapter's vid/pid, and both hex
    // fields contain letters (e, d), so upper- vs. lower-case actually
    // differs in the on-disk key text.
    const char* j = R"({"version":1,"profiles":{
      "0E6D:111D":{"name":"Rewired N64 pad","style":"n64_style","captured_at":"",
        "controls":{"n64_a":{"kind":"button","code":999,"token":"77"}}}}})";
    const auto store = profiles_from_json(j);
    REQUIRE(store.size() == 1);
    // Canonicalized to vidpid_key()'s lowercase form, not the raw file key.
    REQUIRE(store.count("0e6d:111d") == 1);
    REQUIRE(store.count("0E6D:111D") == 0);

    const auto m = resolve_mapping_for_pad(0x0e6d, 0x111d, store, "snes9x2010_libretro");
    // The built-in N64 adapter profile's N64_A token is "2" (see
    // builtin_n64_adapter_profile()); seeing "77" instead proves the
    // captured profile above was found by lookup and won, not the built-in.
    REQUIRE(m.a_btn == "77");
}

// Fix pass 2: profiles_to_json must write each entry under the CALLER's map
// key, not a key derived from the profile's own vid/pid fields. std::map
// already guarantees the caller's keys are unique, so writing them straight
// through can never collide two distinct entries on disk -- unlike deriving
// the on-disk key from p.vid/p.pid, which two entries can easily share (see
// the collision regression test below).
TEST_CASE("profiles_to_json writes each entry under the caller's map key, "
          "not a key derived from the profile's own vid/pid",
          "[controller_profile][json]") {
    std::map<std::string, PhysicalProfile> in;
    PhysicalProfile p = builtin_dragonrise_profile();  // p.vid/p.pid baked at 0079:0006
    in["0810:e501"] = p;  // caller's key intentionally differs from p.vid/p.pid
    const auto out = profiles_from_json(profiles_to_json(in));
    REQUIRE(out.size() == 1);
    // Found under the caller's key, not a key derived from p.vid/p.pid.
    REQUIRE(out.count("0810:e501") == 1);
    REQUIRE(out.count("0079:0006") == 0);
}

// Regression test: profiles_to_json used to derive the on-disk key from each
// profile's own vid/pid fields instead of writing the caller's map key.
// std::map guarantees the caller's keys are unique, but two entries can
// easily carry IDENTICAL vid/pid fields -- e.g. both cloned from the same
// builtin template without overriding vid/pid, as here -- so deriving the
// on-disk key from vid/pid let the second entry silently overwrite the
// first, with no error and no log. This test fails against that code (both
// entries collapse to the same "0079:0006" key, so out.size() == 1) and
// passes once each entry is written under its own caller-supplied key.
TEST_CASE("profiles_to_json does not collide two entries with identical "
          "vid/pid fields stored under different caller keys",
          "[controller_profile][json]") {
    std::map<std::string, PhysicalProfile> in;
    PhysicalProfile a = builtin_dragonrise_profile();  // vid/pid left at 0079:0006
    PhysicalProfile b = builtin_dragonrise_profile();  // same vid/pid, different map key
    a.name = "Player 1 pad";
    b.name = "Player 2 pad";
    in["0810:e501"] = a;
    in["1234:5678"] = b;
    const auto out = profiles_from_json(profiles_to_json(in));
    REQUIRE(out.size() == 2);
    REQUIRE(out.count("0810:e501") == 1);
    REQUIRE(out.count("1234:5678") == 1);
    REQUIRE(out.at("0810:e501").name == "Player 1 pad");
    REQUIRE(out.at("1234:5678").name == "Player 2 pad");
}

TEST_CASE("malformed JSON degrades to an empty store", "[controller_profile][json]") {
    REQUIRE(profiles_from_json("").empty());
    REQUIRE(profiles_from_json("not json at all").empty());
    REQUIRE(profiles_from_json("{\"version\":1,\"profiles\":{\"x\":42}}").empty());
}

TEST_CASE("unknown keys are ignored, known ones survive", "[controller_profile][json]") {
    const char* j = R"({"version":1,"future_field":true,"profiles":{
      "0810:e501":{"name":"T","style":"ps_style","captured_at":"",
        "controls":{"cross":{"kind":"button","code":289,"token":"1"},
                    "warp_drive":{"kind":"button","code":300,"token":"9"}}}}})";
    const auto out = profiles_from_json(j);
    REQUIRE(out.size() == 1);
    REQUIRE(out.at("0810:e501").token(LogicalControl::CROSS) == "1");
    REQUIRE(out.at("0810:e501").controls.size() == 1);  // warp_drive dropped
}

// Extra regression coverage beyond the brief: jsoncpp's asString()/asUInt()/
// asInt() THROW when a field's actual JSON type doesn't convert (verified
// directly against the linked jsoncpp build) -- e.g. root itself not being
// an object, or a "code"/"direction" field written as a string/array. A
// hand-corrupted or partially-written controller_profiles.json can easily
// produce either shape, so both must degrade like every other malformed
// input rather than crashing the kiosk on boot.
TEST_CASE("profiles_from_json never throws on well-formed-but-wrong-shaped JSON",
          "[controller_profile][json]") {
    REQUIRE(profiles_from_json("42").empty());       // valid JSON, non-object root
    REQUIRE(profiles_from_json("[1,2,3]").empty());   // valid JSON, array root
    const char* j = R"({"version":1,"profiles":{"0810:e501":{
        "name":"T","style":"ps_style","captured_at":"",
        "controls":{"cross":{"kind":"button","code":"nope","direction":[1],"token":42}}}}})";
    std::map<std::string, PhysicalProfile> out;
    REQUIRE_NOTHROW(out = profiles_from_json(j));
    REQUIRE(out.size() == 1);
    // Non-convertible fields degrade to their defaults rather than throwing.
    const auto* b = out.at("0810:e501").binding(LogicalControl::CROSS);
    REQUIRE(b != nullptr);
    REQUIRE(b->code == 0);
    REQUIRE(b->direction == 0);
}

// Fix 2: controller_profile.h and the code comments enumerate six
// degrade-gracefully categories (malformed text, non-object "profiles" node,
// bad map key, unknown style, unknown control key, unknown binding kind),
// but only "malformed text" and "unknown control key" were actually
// exercised by a test that reaches the guard it's meant to cover. The four
// tests below close that gap; each is shaped to genuinely reach its guard
// rather than short-circuiting earlier (the existing {"x":42} malformed-JSON
// case, for example, never reaches key parsing at all, since it fails the
// earlier !jp.isObject() check).

TEST_CASE("a well-formed profile object under a non-hex map key is dropped, "
          "not thrown", "[controller_profile][json]") {
    // "zzzz:zzzz" has the right SHAPE (9 chars, colon at index 4) to clear
    // the isObject()/length/colon checks, so this genuinely drives an
    // object-shaped profile entry into parse_hex4_field's std::stoul call --
    // the specific throw risk called out in the brief -- rather than being
    // rejected earlier for some other reason.
    const char* j = R"({"version":1,"profiles":{
      "zzzz:zzzz":{"name":"T","style":"ps_style","captured_at":"","controls":{}}}})";
    std::map<std::string, PhysicalProfile> out;
    REQUIRE_NOTHROW(out = profiles_from_json(j));
    REQUIRE(out.empty());
}

TEST_CASE("a well-formed profile object under a wrong-length map key is dropped",
          "[controller_profile][json]") {
    const char* j = R"({"version":1,"profiles":{
      "0810:e5010":{"name":"T","style":"ps_style","captured_at":"","controls":{}}}})";
    std::map<std::string, PhysicalProfile> out;
    REQUIRE_NOTHROW(out = profiles_from_json(j));
    REQUIRE(out.empty());
}

TEST_CASE("an unknown style value drops that profile", "[controller_profile][json]") {
    const char* j = R"({"version":1,"profiles":{
      "0810:e501":{"name":"T","style":"warp_style","captured_at":"","controls":{}}}})";
    std::map<std::string, PhysicalProfile> out;
    REQUIRE_NOTHROW(out = profiles_from_json(j));
    REQUIRE(out.empty());
}

TEST_CASE("an unknown binding kind drops just that control, not the whole profile",
          "[controller_profile][json]") {
    const char* j = R"({"version":1,"profiles":{
      "0810:e501":{"name":"T","style":"ps_style","captured_at":"",
        "controls":{"cross":{"kind":"warp_drive","code":1,"token":"x"}}}}})";
    std::map<std::string, PhysicalProfile> out;
    REQUIRE_NOTHROW(out = profiles_from_json(j));
    REQUIRE(out.size() == 1);                          // profile itself survives
    REQUIRE(out.at("0810:e501").controls.empty());     // bad control dropped
}

TEST_CASE("a non-object profiles node degrades to an empty store",
          "[controller_profile][json]") {
    std::map<std::string, PhysicalProfile> out;
    REQUIRE_NOTHROW(out = profiles_from_json(R"({"version":1,"profiles":"not an object"})"));
    REQUIRE(out.empty());
    REQUIRE_NOTHROW(out = profiles_from_json(R"({"version":1,"profiles":[1,2,3]})"));
    REQUIRE(out.empty());
    REQUIRE_NOTHROW(out = profiles_from_json(R"({"version":1,"profiles":42})"));
    REQUIRE(out.empty());
}

TEST_CASE("resolution order: captured > builtin > N64 fallback", "[controller_profile]") {
    std::map<std::string, PhysicalProfile> store;
    // 1. Unknown pad, empty store -> N64 fallback (legacy behavior)
    auto m = resolve_mapping_for_pad(0x1234, 0x5678, store, "snes9x2010_libretro");
    REQUIRE(m.name == "Super Nintendo");
    // 2. Known builtin -> its style
    m = resolve_mapping_for_pad(0x0079, 0x0006, store, "snes9x2010_libretro");
    REQUIRE(m.name == "Super Nintendo (PS-style)");
    // 3. Captured profile for the SAME vid/pid wins over the builtin
    PhysicalProfile clone = builtin_dragonrise_profile();
    clone.controls[LogicalControl::CROSS].token = "7";   // rewired clone pad
    store[vidpid_key(0x0079, 0x0006)] = clone;
    m = resolve_mapping_for_pad(0x0079, 0x0006, store, "snes9x2010_libretro");
    REQUIRE(m.b_btn == "7");
    // 4. Captured profile for an unknown pad
    PhysicalProfile cap = builtin_dragonrise_profile();
    cap.vid = 0x1234; cap.pid = 0x5678;
    store[vidpid_key(0x1234, 0x5678)] = cap;
    REQUIRE(resolve_mapping_for_pad(0x1234, 0x5678, store, "snes9x2010_libretro").name
            == "Super Nintendo (PS-style)");
}

TEST_CASE("store save/load round-trips through a temp file", "[controller_profile][store]") {
    ::setenv("MAGIC_CONTROLLER_PROFILES_FILE", "/tmp/mdb_test_profiles.json", 1);
    std::remove("/tmp/mdb_test_profiles.json");
    REQUIRE(load_profile_store().empty());          // missing file -> empty, no error
    std::map<std::string, PhysicalProfile> in;
    PhysicalProfile p = builtin_dragonrise_profile();
    p.vid = 0x0810; p.pid = 0xe501;  // match the map key below, not the builtin's 0079:0006
    in["0810:e501"] = p;
    REQUIRE(save_profile_store(in));
    const auto out = load_profile_store();
    REQUIRE(out.size() == 1);
    REQUIRE(out.count("0810:e501") == 1);
    ::unsetenv("MAGIC_CONTROLLER_PROFILES_FILE");
}

// Regression test for Fix 4: save_profile_store's "config directory does not
// exist yet" branch (std::filesystem::create_directories) was never
// exercised -- the test above points at /tmp, whose parent always exists.
// This points at a multi-level nested path under a directory tree that does
// not exist at all yet, so the save must create it before the atomic
// tmp-file-then-rename write can succeed.
TEST_CASE("save_profile_store creates missing parent directories",
          "[controller_profile][store]") {
    const std::string base_dir = "/tmp/mdb_test_profiles_missing_dir";
    const std::string nested_path = base_dir + "/a/b/c/profiles.json";
    std::filesystem::remove_all(base_dir);   // clean slate: base_dir must not exist yet
    REQUIRE_FALSE(std::filesystem::exists(base_dir));

    ::setenv("MAGIC_CONTROLLER_PROFILES_FILE", nested_path.c_str(), 1);
    std::map<std::string, PhysicalProfile> in;
    in["0810:e501"] = builtin_dragonrise_profile();
    REQUIRE(save_profile_store(in));
    REQUIRE(std::filesystem::exists(nested_path));
    REQUIRE(load_profile_store().size() == 1);   // round-trips after directory creation

    ::unsetenv("MAGIC_CONTROLLER_PROFILES_FILE");
    std::filesystem::remove_all(base_dir);       // clean up what this test created
}

// ---------------------------------------------------------------------------
// Menu-navigation overlay derivation (Task 9)
//
// menu_overlay_from_profile() is the PURE half of the overlay feature: it
// turns a captured/builtin PhysicalProfile into the per-model kiosk-menu
// mapping that InputManager layers on top of its hardcoded switch. The
// InputManager half is Pi-compiled only; everything semantic is pinned here.
// ---------------------------------------------------------------------------

TEST_CASE("menu overlay mirrors the kiosk's hardcoded semantics",
          "[controller_profile][overlay]") {
    using platform::InputAction;
    const auto ps = menu_overlay_from_profile(builtin_dragonrise_profile());
    REQUIRE(ps.buttons.at(290) == InputAction::SELECT);          // Cross
    REQUIRE(ps.buttons.at(297) == InputAction::SELECT);          // Start
    REQUIRE(ps.buttons.at(289) == InputAction::SETTINGS_MENU);   // Circle
    REQUIRE(ps.buttons.at(288) == InputAction::PLAY_PAUSE);      // Triangle
    REQUIRE(ps.buttons.at(293) == InputAction::NEXT);            // R1
    REQUIRE(ps.buttons.at(292) == InputAction::PREV);            // L1
    REQUIRE(ps.nav_x_abs == 0);                                  // ABS_X
    REQUIRE(ps.seek_abs == 2);                                   // ABS_Z (right stick X)

    const auto n64 = menu_overlay_from_profile(builtin_n64_adapter_profile());
    REQUIRE(n64.buttons.at(306) == InputAction::SELECT);         // A
    REQUIRE(n64.buttons.at(305) == InputAction::SETTINGS_MENU);  // B
    REQUIRE(n64.buttons.at(310) == InputAction::PLAY_PAUSE);     // Z
    REQUIRE(n64.buttons.at(309) == InputAction::NEXT);           // R
    REQUIRE(n64.buttons.at(308) == InputAction::PREV);           // L
    REQUIRE(n64.seek_abs == -1);   // C cluster is buttons, not an axis
}

// The overlay is ADDITIVE: a code it does not claim must fall through to
// InputManager's hardcoded switch. These cases pin the "claims nothing"
// end of that contract, so a partial or nonsense captured profile can only
// ever leave the kiosk at stock behavior, never below it.
TEST_CASE("menu overlay claims nothing for an empty profile",
          "[controller_profile][overlay]") {
    PhysicalProfile empty;
    empty.style = ControllerStyle::PS_STYLE;
    const auto o = menu_overlay_from_profile(empty);
    REQUIRE(o.buttons.empty());
    REQUIRE(o.nav_x_abs == -1);
    REQUIRE(o.seek_abs == -1);

    PhysicalProfile empty_n64;
    empty_n64.style = ControllerStyle::N64_STYLE;
    const auto n = menu_overlay_from_profile(empty_n64);
    REQUIRE(n.buttons.empty());
    REQUIRE(n.nav_x_abs == -1);
    REQUIRE(n.seek_abs == -1);
}

TEST_CASE("menu overlay ignores bindings of the wrong kind",
          "[controller_profile][overlay]") {
    // A mis-captured profile whose CROSS landed on an axis and whose left
    // stick landed on a button must contribute NOTHING for those controls
    // rather than claiming a code it cannot correctly interpret.
    PhysicalProfile p;
    p.style = ControllerStyle::PS_STYLE;
    p.controls[LogicalControl::CROSS] = {K::AXIS, kAbsX, +1, "+0"};
    p.controls[LogicalControl::LSTICK_RIGHT] = {K::BUTTON, 290, 0, "2"};
    p.controls[LogicalControl::RSTICK_RIGHT] = {K::HAT, kAbsHat0X, +1, "h0right"};
    // A well-formed one alongside them still lands.
    p.controls[LogicalControl::CIRCLE] = {K::BUTTON, 289, 0, "1"};

    const auto o = menu_overlay_from_profile(p);
    REQUIRE(o.buttons.size() == 1);
    REQUIRE(o.buttons.at(289) == platform::InputAction::SETTINGS_MENU);
    REQUIRE(o.buttons.count(kAbsX) == 0);   // the AXIS-kind CROSS is not a button
    REQUIRE(o.nav_x_abs == -1);             // BUTTON-kind stick is not an axis
    REQUIRE(o.seek_abs == -1);              // HAT-kind stick is not an axis either
}

// An N64-style profile must read its stick from the N64 vocabulary, and a
// PS-style one from the PS vocabulary -- crossing them would silently pick
// up nothing (or the wrong axis) on a real captured profile.
TEST_CASE("menu overlay picks stick axes per style", "[controller_profile][overlay]") {
    PhysicalProfile n64;
    n64.style = ControllerStyle::N64_STYLE;
    n64.controls[LogicalControl::N64_STICK_RIGHT] = {K::AXIS, kAbsX, +1, "+0"};
    n64.controls[LogicalControl::N64_C_RIGHT] = {K::AXIS, kAbsZ, +1, "+2"};
    // PS-vocabulary bindings on an N64-style profile are not consulted.
    n64.controls[LogicalControl::LSTICK_RIGHT] = {K::AXIS, kAbsRz, +1, "+3"};
    const auto o = menu_overlay_from_profile(n64);
    REQUIRE(o.nav_x_abs == kAbsX);
    REQUIRE(o.seek_abs == kAbsZ);   // an N64 pad whose C-cluster IS an axis
}
