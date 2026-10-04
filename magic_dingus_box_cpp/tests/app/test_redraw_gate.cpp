// app::RedrawGate — "draw this iteration, or skip render/swap/flip?"
//
// The kiosk used to render, swap and page-flip at 60 Hz even on a menu
// where nothing moved: a full frame of CPU + GPU + memory bandwidth per
// vblank, for an identical picture. The gate lets the loop keep polling
// (input, GPIO, phone remote, watchdog, status file) while skipping the
// GL work — but only where that is provably safe. These cases pin the
// conservative contract:
//   - anything that might change the picture draws THIS iteration
//     (input, video, animation, a non-static screen, a forced frame, or a
//     change in the drawn content's signature);
//   - the first quiet iteration after activity still draws (settle frame);
//   - nothing is ever skipped longer than max_idle (a missed dirty source
//     costs latency, never a frozen screen);
//   - disabled == today's behavior: every iteration draws.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "app/redraw_gate.h"

using app::MainMenuActivity;
using app::RedrawGate;
using app::RedrawInputs;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

namespace {

const Clock::time_point T0 = Clock::time_point{} + std::chrono::hours(1);

Clock::time_point at(int ms) { return T0 + milliseconds(ms); }

RedrawInputs quiet(uint64_t sig = 7) {
    RedrawInputs in;
    in.content_signature = sig;
    return in;
}

// Gate that has drawn its first frame at T0 and settled (one trailing
// frame) — i.e. sitting idle on a static menu.
RedrawGate settled_gate(milliseconds max_idle = milliseconds(250)) {
    RedrawGate g(true, max_idle);
    REQUIRE(g.should_draw(quiet(), at(0)));    // first frame always draws
    REQUIRE_FALSE(g.should_draw(quiet(), at(16)));
    return g;
}

}  // namespace

TEST_CASE("RedrawGate: the first iteration always draws", "[redraw_gate]") {
    RedrawGate g(true);
    CHECK(g.should_draw(quiet(), at(0)));
}

TEST_CASE("RedrawGate: a static, unchanged screen is skipped",
          "[redraw_gate]") {
    RedrawGate g = settled_gate();
    for (int t = 32; t < 240; t += 16) {
        CHECK_FALSE(g.should_draw(quiet(), at(t)));
    }
}

TEST_CASE("RedrawGate: input draws on the same iteration", "[redraw_gate]") {
    RedrawGate g = settled_gate();
    RedrawInputs in = quiet();
    in.input_event = true;
    CHECK(g.should_draw(in, at(40)));
}

TEST_CASE("RedrawGate: every activity source draws", "[redraw_gate]") {
    auto check_source = [](void (*set)(RedrawInputs&)) {
        RedrawGate g = settled_gate();
        RedrawInputs in = quiet();
        set(in);
        CHECK(g.should_draw(in, at(40)));
        // ...and keeps drawing while the source stays on.
        CHECK(g.should_draw(in, at(56)));
    };
    check_source([](RedrawInputs& in) { in.video_frame = true; });
    check_source([](RedrawInputs& in) { in.animation_active = true; });
    check_source([](RedrawInputs& in) { in.screen_requests_continuous = true; });
    check_source([](RedrawInputs& in) { in.forced = true; });
}

TEST_CASE("RedrawGate: a content signature change draws exactly once",
          "[redraw_gate]") {
    // The frame drawn for a changed signature IS the new picture (no
    // in-between state), so unlike activity it earns no settle frame —
    // the 2 Hz selection blink costs 2 frames a second, not 4.
    RedrawGate g = settled_gate();
    CHECK(g.should_draw(quiet(8), at(40)));   // e.g. blink phase flipped
    CHECK_FALSE(g.should_draw(quiet(8), at(56)));
    CHECK(g.should_draw(quiet(7), at(72)));   // flipped back
}

TEST_CASE("RedrawGate: one settle frame after activity ends",
          "[redraw_gate]") {
    // The last ACTIVE frame may show an in-between state (a menu closing
    // at 95%) — the first quiet iteration redraws the final picture.
    RedrawGate g = settled_gate();
    RedrawInputs busy = quiet();
    busy.animation_active = true;
    CHECK(g.should_draw(busy, at(40)));
    CHECK(g.should_draw(busy, at(56)));
    CHECK(g.should_draw(quiet(), at(72)));        // settle
    CHECK_FALSE(g.should_draw(quiet(), at(88)));  // then idle
}

TEST_CASE("RedrawGate: max_idle is a hard ceiling on skipping",
          "[redraw_gate]") {
    RedrawGate g = settled_gate(milliseconds(250));
    CHECK_FALSE(g.should_draw(quiet(), at(249)));
    CHECK(g.should_draw(quiet(), at(250)));
    // Measured from the last DRAW, so a safety-net frame re-arms it.
    CHECK_FALSE(g.should_draw(quiet(), at(400)));
    CHECK(g.should_draw(quiet(), at(500)));
}

TEST_CASE("RedrawGate: a long stall (game session) draws immediately",
          "[redraw_gate]") {
    RedrawGate g = settled_gate();
    CHECK(g.should_draw(quiet(), at(30 * 60 * 1000)));
}

TEST_CASE("RedrawGate: disabled draws every iteration", "[redraw_gate]") {
    RedrawGate g(false);
    for (int t = 0; t < 1000; t += 16) {
        CHECK(g.should_draw(quiet(), at(t)));
    }
    CHECK_FALSE(g.enabled());
}

TEST_CASE("RedrawGate: per-minute report counts drawn and skipped",
          "[redraw_gate]") {
    RedrawGate g(true, milliseconds(250));
    CHECK_FALSE(g.take_report(at(0)).has_value());
    int drawn = 0, skipped = 0;
    for (int t = 0; t < 60000; t += 16) {
        (g.should_draw(quiet(), at(t)) ? drawn : skipped)++;
    }
    CHECK_FALSE(g.take_report(at(59000)).has_value());
    auto r = g.take_report(at(60000));
    REQUIRE(r.has_value());
    CHECK(r->drawn == static_cast<uint64_t>(drawn));
    CHECK(r->skipped == static_cast<uint64_t>(skipped));
    // ~4 Hz of safety-net frames on an idle menu, not 60.
    CHECK(r->drawn < 300);
    // Counters restart for the next window.
    auto again = g.take_report(at(120000));
    REQUIRE(again.has_value());
    CHECK(again->drawn == 0);
    CHECK(again->skipped == 0);
}

TEST_CASE("is_static_main_menu: only the bare menu is static",
          "[redraw_gate]") {
    MainMenuActivity a;
    CHECK(app::is_static_main_menu(a));

    auto check_not_static = [](void (*set)(MainMenuActivity&)) {
        MainMenuActivity x;
        set(x);
        CHECK_FALSE(app::is_static_main_menu(x));
    };
    check_not_static([](MainMenuActivity& x) { x.intro = true; });
    check_not_static([](MainMenuActivity& x) { x.video = true; });
    check_not_static([](MainMenuActivity& x) { x.media_browser = true; });
    check_not_static([](MainMenuActivity& x) { x.settings_menu = true; });
    check_not_static([](MainMenuActivity& x) { x.keyboard = true; });
    check_not_static([](MainMenuActivity& x) { x.ui_fade = true; });
    check_not_static([](MainMenuActivity& x) { x.transient_overlay = true; });
    check_not_static([](MainMenuActivity& x) { x.crt_time_effects = true; });
}

TEST_CASE("redraw_gate_enabled_from_env: default ON, explicit off values",
          "[redraw_gate]") {
    CHECK(app::redraw_gate_enabled_from_env(nullptr));
    CHECK(app::redraw_gate_enabled_from_env(""));
    CHECK(app::redraw_gate_enabled_from_env("1"));
    CHECK(app::redraw_gate_enabled_from_env("on"));
    CHECK_FALSE(app::redraw_gate_enabled_from_env("0"));
    CHECK_FALSE(app::redraw_gate_enabled_from_env("off"));
    CHECK_FALSE(app::redraw_gate_enabled_from_env("false"));
    CHECK_FALSE(app::redraw_gate_enabled_from_env("no"));
    CHECK_FALSE(app::redraw_gate_enabled_from_env("OFF"));
}

TEST_CASE("ContentSignature: order- and value-sensitive", "[redraw_gate]") {
    app::ContentSignature a, b, c;
    a.add(1); a.add(2);
    b.add(1); b.add(2);
    c.add(2); c.add(1);
    CHECK(a.value() == b.value());
    CHECK(a.value() != c.value());

    app::ContentSignature s1, s2;
    s1.add(std::string("Ready"));
    s2.add(std::string("Ready."));
    CHECK(s1.value() != s2.value());
}
