#include <catch2/catch_test_macros.hpp>
#include "retroarch/capture_session.h"

using namespace retroarch;
namespace {
constexpr uint16_t EV_KEY_T = 0x01, EV_ABS_T = 0x03;

CaptureDeviceCaps dragonrise_like() {
    CaptureDeviceCaps c;
    c.vid = 0x0810; c.pid = 0xe501; c.name = "Twin USB";
    for (uint16_t k = 288; k <= 299; ++k) c.key_codes.push_back(k);
    c.axes = {{0, -32768, 32767, 0}, {1, -32768, 32767, 0},
              {2, -32768, 32767, 0}, {5, -32768, 32767, 0},
              {16, -1, 1, 0}, {17, -1, 1, 0}};
    return c;
}
}  // namespace

TEST_CASE("happy path captures a button on press+release", "[capture_session]") {
    CaptureSession s(ControllerStyle::PS_STYLE, dragonrise_like());
    REQUIRE(s.current_control() == LogicalControl::DPAD_UP);
    // D-pad up on the hat: arm with -1, capture on 0
    REQUIRE(s.feed(EV_ABS_T, 17, -1) == CaptureSession::FeedResult::NONE);
    REQUIRE(s.feed(EV_ABS_T, 17, 0) == CaptureSession::FeedResult::CAPTURED);
    REQUIRE(s.current_control() == LogicalControl::DPAD_DOWN);
    REQUIRE(s.feed(EV_ABS_T, 17, +1) == CaptureSession::FeedResult::NONE);
    REQUIRE(s.feed(EV_ABS_T, 17, 0) == CaptureSession::FeedResult::CAPTURED);
    // skip left/right, then CROSS as a button
    s.skip(); s.skip();
    REQUIRE(s.current_control() == LogicalControl::CROSS);
    REQUIRE(s.feed(EV_KEY_T, 290, 1) == CaptureSession::FeedResult::NONE);
    REQUIRE(s.feed(EV_KEY_T, 290, 0) == CaptureSession::FeedResult::CAPTURED);
}

TEST_CASE("duplicates are rejected and reported", "[capture_session]") {
    CaptureSession s(ControllerStyle::PS_STYLE, dragonrise_like());
    s.feed(EV_KEY_T, 290, 1); s.feed(EV_KEY_T, 290, 0);   // DPAD_UP <- btn 290
    REQUIRE(s.current_control() == LogicalControl::DPAD_DOWN);
    s.feed(EV_KEY_T, 290, 1);
    REQUIRE(s.feed(EV_KEY_T, 290, 0) == CaptureSession::FeedResult::DUPLICATE);
    REQUIRE(s.last_duplicate_of() == LogicalControl::DPAD_UP);
    REQUIRE(s.current_control() == LogicalControl::DPAD_DOWN);  // did not advance
}

TEST_CASE("axis capture honors rest position and sign", "[capture_session]") {
    auto caps = dragonrise_like();
    caps.axes[1].rest = 1000;                       // slightly off-center stick
    CaptureSession s(ControllerStyle::PS_STYLE, caps);
    while (s.current_control() != LogicalControl::LSTICK_UP) s.skip();
    // Inverted-feeling axis: up produces POSITIVE values on this pad
    REQUIRE(s.feed(EV_ABS_T, 1, 30000) == CaptureSession::FeedResult::NONE);
    REQUIRE(s.feed(EV_ABS_T, 1, 1500) == CaptureSession::FeedResult::CAPTURED);
    const auto r = s.results();                     // partial results OK for assert
    REQUIRE(r.at(LogicalControl::LSTICK_UP).direction == +1);
    REQUIRE(r.at(LogicalControl::LSTICK_UP).token == "+1");
}

TEST_CASE("small wiggles below 50% never arm", "[capture_session]") {
    CaptureSession s(ControllerStyle::PS_STYLE, dragonrise_like());
    while (s.current_control() != LogicalControl::LSTICK_UP) s.skip();
    REQUIRE(s.feed(EV_ABS_T, 1, -8000) == CaptureSession::FeedResult::NONE);
    REQUIRE(s.feed(EV_ABS_T, 1, 0) == CaptureSession::FeedResult::NONE);   // never armed
}

TEST_CASE("redo_last steps back and unbinds", "[capture_session]") {
    CaptureSession s(ControllerStyle::PS_STYLE, dragonrise_like());
    REQUIRE(!s.redo_last());                        // nothing to redo yet
    s.feed(EV_KEY_T, 290, 1); s.feed(EV_KEY_T, 290, 0);
    REQUIRE(s.redo_last());
    REQUIRE(s.current_control() == LogicalControl::DPAD_UP);
    // the freed binding is reusable without DUPLICATE
    s.feed(EV_KEY_T, 290, 1);
    REQUIRE(s.feed(EV_KEY_T, 290, 0) == CaptureSession::FeedResult::CAPTURED);
}

TEST_CASE("a stickless pad finishes by skipping and yields no stick binds",
          "[capture_session]") {
    CaptureSession s(ControllerStyle::PS_STYLE, dragonrise_like());
    size_t guard = 0;
    while (!s.done() && guard++ < 100) {
        if (s.current_control() == LogicalControl::CROSS) {
            s.feed(EV_KEY_T, 290, 1); s.feed(EV_KEY_T, 290, 0);
        } else {
            s.skip();
        }
    }
    REQUIRE(s.done());
    REQUIRE(s.results().size() == 1);
    REQUIRE(s.results().count(LogicalControl::LSTICK_UP) == 0);
}

TEST_CASE("N64 flow reaches DONE on the final capture", "[capture_session]") {
    CaptureSession s(ControllerStyle::N64_STYLE, dragonrise_like());
    while (s.step_index() + 1 < s.step_count()) s.skip();
    s.feed(EV_KEY_T, 291, 1);
    REQUIRE(s.feed(EV_KEY_T, 291, 0) == CaptureSession::FeedResult::DONE);
    REQUIRE(s.done());
}

// --- Self-review additions ---------------------------------------------

TEST_CASE("redo_last after skip walks back onto the skipped step cleanly",
          "[capture_session]") {
    CaptureSession s(ControllerStyle::PS_STYLE, dragonrise_like());
    s.skip();  // DPAD_UP skipped, never captured
    REQUIRE(s.current_control() == LogicalControl::DPAD_DOWN);
    REQUIRE(s.redo_last());
    REQUIRE(s.current_control() == LogicalControl::DPAD_UP);
    REQUIRE(s.results().count(LogicalControl::DPAD_UP) == 0);  // erase-if-absent is a no-op
    // capturing normally afterward works -- no residual armed state from the skip
    REQUIRE(s.feed(EV_ABS_T, 17, -1) == CaptureSession::FeedResult::NONE);
    REQUIRE(s.feed(EV_ABS_T, 17, 0) == CaptureSession::FeedResult::CAPTURED);
}

TEST_CASE("a release with no prior press does nothing", "[capture_session]") {
    CaptureSession s(ControllerStyle::PS_STYLE, dragonrise_like());
    REQUIRE(s.feed(EV_KEY_T, 290, 0) == CaptureSession::FeedResult::NONE);  // stray release
    REQUIRE(s.current_control() == LogicalControl::DPAD_UP);               // did not advance
    REQUIRE(s.results().empty());
}

TEST_CASE("a different button's press replaces the pending press",
          "[capture_session]") {
    CaptureSession s(ControllerStyle::PS_STYLE, dragonrise_like());
    s.feed(EV_KEY_T, 288, 1);  // press button A, never released
    s.feed(EV_KEY_T, 290, 1);  // press button B before A's release -- replaces pending
    REQUIRE(s.feed(EV_KEY_T, 288, 0) == CaptureSession::FeedResult::NONE);      // stray release of A
    REQUIRE(s.feed(EV_KEY_T, 290, 0) == CaptureSession::FeedResult::CAPTURED);  // B captures
    REQUIRE(s.results().at(LogicalControl::DPAD_UP).code == 290);
}

TEST_CASE("results() reflects only captured steps mid-session", "[capture_session]") {
    CaptureSession s(ControllerStyle::PS_STYLE, dragonrise_like());
    REQUIRE(s.results().empty());
    s.feed(EV_KEY_T, 290, 1); s.feed(EV_KEY_T, 290, 0);  // DPAD_UP captured
    s.skip();                                            // DPAD_DOWN skipped
    REQUIRE(!s.done());
    const auto r = s.results();
    REQUIRE(r.size() == 1);
    REQUIRE(r.count(LogicalControl::DPAD_UP) == 1);
    REQUIRE(r.count(LogicalControl::DPAD_DOWN) == 0);
}

TEST_CASE("an axis event for a code absent from caps never arms or captures",
          "[capture_session]") {
    CaptureSession s(ControllerStyle::PS_STYLE, dragonrise_like());
    while (s.current_control() != LogicalControl::LSTICK_UP) s.skip();
    // code 42 is not in dragonrise_like()'s axes list at all
    REQUIRE(s.feed(EV_ABS_T, 42, 30000) == CaptureSession::FeedResult::NONE);
    REQUIRE(s.feed(EV_ABS_T, 42, 0) == CaptureSession::FeedResult::NONE);
    REQUIRE(s.results().empty());
}

TEST_CASE("8-bit range pad (min=0,max=255,rest=127) arms and captures with correct math",
          "[capture_session]") {
    CaptureDeviceCaps caps;
    caps.axes = {{99, 0, 255, 127}};
    CaptureSession s(ControllerStyle::PS_STYLE, caps);
    // half-range = (255-0)/2 = 127; 50% threshold = 63; 25% threshold = 31
    REQUIRE(s.feed(EV_ABS_T, 99, 180) == CaptureSession::FeedResult::NONE);  // delta=53, not >63: no arm
    REQUIRE(s.feed(EV_ABS_T, 99, 127) == CaptureSession::FeedResult::NONE);  // back to rest, never armed
    REQUIRE(s.feed(EV_ABS_T, 99, 250) == CaptureSession::FeedResult::NONE); // delta=123 >63: arms +1
    REQUIRE(s.feed(EV_ABS_T, 99, 110) == CaptureSession::FeedResult::CAPTURED);  // delta=-17, |delta|<31
    REQUIRE(s.results().at(LogicalControl::DPAD_UP).direction == +1);
    REQUIRE(s.results().at(LogicalControl::DPAD_UP).token == "+0");
}
