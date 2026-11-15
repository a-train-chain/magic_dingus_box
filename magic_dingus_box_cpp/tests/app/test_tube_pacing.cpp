#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "app/tube_pacing.h"
#include "tube/tube_gpu_timing.h"

using namespace std::chrono_literals;
using tube::FrameClass;

TEST_CASE("tube pacing: late is half a refresh past the class's cadence", "[tube]") {
    app::TubePacingMonitor m; auto t = app::TubePacingMonitor::Clock::time_point{} + 1h;
    m.take_report(t);
    for (int i = 0; i < 10; ++i) { t += 33333us; m.on_present(t, FrameClass::Rich30, 60, true); }
    t += 50000us; m.on_present(t, FrameClass::Rich30, 60, true);              // missed: 50 ms
    t += 41000us; m.on_present(t, FrameClass::Rich30, 60, true);              // 41 ms < 33.3 + 8.3: on time
    auto r = m.take_report(t + 60s); REQUIRE(r); REQUIRE(r->presents == 12); REQUIRE(r->late == 1);
    app::TubePacingMonitor hz120; auto u = t + 1h;                            // 120 Hz: the cadence is four vblanks
    for (int i = 0; i < 5; ++i) { u += 33333us; hz120.on_present(u, FrameClass::Rich30, 120, true); }
    REQUIRE(hz120.take_report(u) == std::nullopt);
    REQUIRE(hz120.take_report(u + 61s)->late == 0);
}

TEST_CASE("tube pacing: too heavy after 30 s of video with more than 5% late, toast once", "[tube]") {
    app::TubePacingMonitor m; auto t = app::TubePacingMonitor::Clock::time_point{} + 1h;
    for (int i = 0; i < 1900; ++i) { t += (i % 10 == 0) ? 33333us : 16667us; m.on_present(t, FrameClass::Lean60, 60, true); }
    REQUIRE(m.too_heavy()); REQUIRE(m.take_toast()); REQUIRE_FALSE(m.take_toast());
    m.reset(); REQUIRE_FALSE(m.too_heavy());
    app::TubePacingMonitor menu; t += 1s;
    for (int i = 0; i < 1900; ++i) { t += 33333us; menu.on_present(t, FrameClass::Lean60, 60, false); }
    REQUIRE_FALSE(menu.too_heavy());
}

TEST_CASE("tube pacing: a refresh of zero or less is read as 60", "[tube]") {
    for (const int hz : {0, -5}) {
        app::TubePacingMonitor m; auto t = app::TubePacingMonitor::Clock::time_point{} + 1h;
        m.take_report(t);
        t += 16667us; m.on_present(t, FrameClass::Lean60, hz, true);
        t += 20000us; m.on_present(t, FrameClass::Lean60, hz, true);          // 20 ms < 16.7 + 8.3: on time
        t += 30000us; m.on_present(t, FrameClass::Lean60, hz, true);          // late at 60 Hz
        auto r = m.take_report(t + 60s); REQUIRE(r); REQUIRE(r->presents == 3); REQUIRE(r->late == 1);
    }
}

TEST_CASE("tube pacing: the first present after a gap counts nothing", "[tube]") {
    app::TubePacingMonitor m; auto t = app::TubePacingMonitor::Clock::time_point{} + 1h;
    m.take_report(t);
    m.on_present(t += 16667us, FrameClass::Lean60, 60, true);
    m.on_present(t += 300ms, FrameClass::Lean60, 60, true);                   // gap: neither a present nor late
    m.on_present(t += 16667us, FrameClass::Lean60, 60, true);
    auto r = m.take_report(t + 60s); REQUIRE(r); REQUIRE(r->presents == 2); REQUIRE(r->late == 0);
}

TEST_CASE("tube pacing: reset re-arms the toast for the new look", "[tube]") {
    app::TubePacingMonitor m; auto t = app::TubePacingMonitor::Clock::time_point{} + 1h;
    auto heavy = [&] { for (int i = 0; i < 1900; ++i) { t += (i % 10 == 0) ? 33333us : 16667us; m.on_present(t, FrameClass::Lean60, 60, true); } };
    heavy(); REQUIRE(m.take_toast());
    heavy(); REQUIRE_FALSE(m.take_toast());                                   // same look: not again
    m.reset(); t += 1s; heavy(); REQUIRE(m.take_toast());                     // new look: once more
}

TEST_CASE("tube gpu timing: one line every ten seconds", "[tube]") {
    tube::PassTimingAverager a; using P = tube::PassTimingAverager::Pass;
    REQUIRE_FALSE(a.take_line(0.0));
    for (int i = 0; i < 2; ++i) { a.add(P::output, 7.0 + i); a.add(P::probe, 0.2); a.end_frame(); }
    REQUIRE_FALSE(a.take_line(5.0));
    REQUIRE(*a.take_line(10.0) ==
            "tube gpu: 2 frames: quarter - glow - signal - lines - output 7.50 history - probe 0.20 = 7.70 ms");
    REQUIRE_FALSE(a.take_line(15.0));                                         // a new window began at 10 s
}
