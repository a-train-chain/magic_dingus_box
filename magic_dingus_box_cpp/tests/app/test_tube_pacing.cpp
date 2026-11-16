#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>

#include "app/tube_pacing.h"
#include "app/tube_redraw.h"
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

namespace {
// Rich30 at 60 Hz: on time is 40 ms, late is 45 ms (the line is 33.3 + 8.3 = 41.7 ms).
// With those two intervals every 30 s window of an alternating run holds exactly as
// many late presents as on-time ones (30 s mod 85 ms = 80 ms >= 45 ms).
struct Feed {
    app::TubePacingMonitor m;
    app::TubePacingMonitor::Clock::time_point t = app::TubePacingMonitor::Clock::time_point{} + 1h;
    std::uint64_t n = 0;
    void present(bool late, bool video = true) { t += late ? 45ms : 40ms; m.on_present(t, FrameClass::Rich30, 60, video); }
    // `span` of video with `pct` percent of the presents late, spread evenly.
    void run(std::chrono::milliseconds span, int pct) {
        for (const auto end = t + span; t < end; ++n) present((n + 1) * pct / 100 != n * pct / 100);
    }
    void minutes(int count, int pct) { run(std::chrono::minutes(count), pct); }
};
}  // namespace

TEST_CASE("tube pacing: too heavy after 30 s of video more than half late, toast once", "[tube]") {
    Feed f; f.run(31s, 67);                                                   // two late in three
    REQUIRE(f.m.too_heavy()); REQUIRE(f.m.take_toast()); REQUIRE_FALSE(f.m.take_toast());
    f.m.reset(); REQUIRE_FALSE(f.m.too_heavy());
    Feed half; for (int i = 0; i < 1000; ++i) half.present(i % 2 == 1);       // exactly half, 42 s
    REQUIRE_FALSE(half.m.too_heavy());
    Feed short_run; short_run.run(29s, 100);                                  // all late, but 29 s
    REQUIRE_FALSE(short_run.m.too_heavy());
    short_run.run(2s, 100); REQUIRE(short_run.m.too_heavy());
    Feed menu; for (int i = 0; i < 1900; ++i) menu.present(true, false);
    REQUIRE_FALSE(menu.m.too_heavy());
}

TEST_CASE("tube pacing: a busy burst of a few minutes is not too heavy", "[tube]") {
    // Hardware, Pi 5, 2026-11-16: the Media Browser's containers starting again after a
    // game ran a shipped look 12-13% late for two minutes, 20% for half a minute.
    Feed f; f.minutes(3, 13); f.minutes(10, 0); REQUIRE_FALSE(f.m.too_heavy());
    Feed g; g.minutes(3, 20); REQUIRE_FALSE(g.m.too_heavy());
    g.minutes(2, 0); g.minutes(3, 20); REQUIRE_FALSE(g.m.too_heavy());
}

TEST_CASE("tube pacing: ten minutes each more than 5% late are too heavy", "[tube]") {
    Feed f; f.minutes(9, 6); REQUIRE_FALSE(f.m.too_heavy());
    f.run(59s, 6); REQUIRE_FALSE(f.m.too_heavy());                            // the tenth minute is not over
    f.run(2s, 6); REQUIRE(f.m.too_heavy());
    REQUIRE(f.m.take_toast()); REQUIRE_FALSE(f.m.take_toast());               // once, by this rule too
    f.minutes(5, 6); REQUIRE_FALSE(f.m.take_toast());
    Feed g; g.minutes(9, 6); g.minutes(1, 4); g.minutes(9, 6);                // a quiet minute resets the count
    REQUIRE_FALSE(g.m.too_heavy());
    g.minutes(1, 6); g.run(2s, 6); REQUIRE(g.m.too_heavy());
}

TEST_CASE("tube pacing: a broken streak restarts the ten minutes", "[tube]") {
    Feed gap; gap.minutes(5, 6); gap.run(20s, 6);
    gap.t += 300ms; gap.m.on_present(gap.t, FrameClass::Rich30, 60, true);   // a gap
    gap.minutes(9, 6); gap.run(30s, 6); REQUIRE_FALSE(gap.m.too_heavy());
    gap.run(32s, 6); REQUIRE(gap.m.too_heavy());
    Feed menu; menu.minutes(5, 6); menu.run(20s, 6);
    menu.present(false, false);                                               // video stopped
    menu.minutes(9, 6); menu.run(30s, 6); REQUIRE_FALSE(menu.m.too_heavy());
    menu.run(32s, 6); REQUIRE(menu.m.too_heavy());
}

TEST_CASE("tube pacing: reset clears a partial ten minutes", "[tube]") {
    Feed f; f.minutes(9, 6); f.run(30s, 6); f.m.reset();
    f.minutes(1, 6); f.run(2s, 6); REQUIRE_FALSE(f.m.too_heavy());
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
    auto heavy = [&] { for (int i = 0; i < 1900; ++i) { t += (i % 10 < 6) ? 33333us : 16667us; m.on_present(t, FrameClass::Lean60, 60, true); } };
    heavy(); REQUIRE(m.take_toast());
    heavy(); REQUIRE_FALSE(m.take_toast());                                   // same look: not again
    m.reset(); t += 1s; heavy(); REQUIRE(m.take_toast());                     // new look: once more
}

TEST_CASE("tube pacing: too heavy is latched until reset", "[tube]") {
    app::TubePacingMonitor m; auto t = app::TubePacingMonitor::Clock::time_point{} + 1h;
    for (int i = 0; i < 1900; ++i) { t += (i % 10 < 6) ? 33333us : 16667us; m.on_present(t, FrameClass::Lean60, 60, true); }
    REQUIRE(m.too_heavy());
    for (int i = 0; i < 100; ++i) { t += 16667us; m.on_present(t, FrameClass::Lean60, 60, false); }   // a menu
    REQUIRE(m.too_heavy());
    t += 400ms; m.on_present(t, FrameClass::Lean60, 60, true);                                        // a gap
    for (int i = 0; i < 2000; ++i) { t += 16667us; m.on_present(t, FrameClass::Lean60, 60, true); }  // recovered
    REQUIRE(m.too_heavy());
    m.reset(); REQUIRE_FALSE(m.too_heavy());
}

TEST_CASE("tube pacing: presents before the first take_report are not in it", "[tube]") {
    app::TubePacingMonitor m; auto t = app::TubePacingMonitor::Clock::time_point{} + 1h;
    for (int i = 0; i < 5; ++i) { t += 16667us; m.on_present(t, FrameClass::Lean60, 60, true); }
    REQUIRE(m.take_report(t) == std::nullopt);
    t += 16667us; m.on_present(t, FrameClass::Lean60, 60, true);
    auto r = m.take_report(t + 60s); REQUIRE(r); REQUIRE(r->presents == 1);
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

TEST_CASE("tube pacing: a lean look at 120 Hz presents every two vblanks and is not late", "[tube]") {
    // The kiosk caps every frame at 60 fps (utils::frame_pacing_for(60, hz)): two
    // refreshes at 120 Hz, one at 50 and 60 Hz.
    REQUIRE(app::TubePacingMonitor::cadence_vblanks(FrameClass::Lean60, 120) == 2);
    REQUIRE(app::TubePacingMonitor::cadence_vblanks(FrameClass::Lean60, 60) == 1);
    REQUIRE(app::TubePacingMonitor::cadence_vblanks(FrameClass::Lean60, 50) == 1);
    REQUIRE(app::TubePacingMonitor::cadence_vblanks(FrameClass::Rich30, 120) == 4);
    REQUIRE(app::TubePacingMonitor::cadence_vblanks(FrameClass::Rich30, 60) == 2);
    REQUIRE(app::TubePacingMonitor::cadence_vblanks(FrameClass::Rich30, 50) == 2);
    app::TubePacingMonitor m; auto t = app::TubePacingMonitor::Clock::time_point{} + 1h;
    m.take_report(t);
    for (int i = 0; i < 2000; ++i) { t += 16667us; m.on_present(t, FrameClass::Lean60, 120, true); }
    t += 30000us; m.on_present(t, FrameClass::Lean60, 120, true);             // 30 ms > 16.7 + 4.2: late
    auto r = m.take_report(t + 60s); REQUIRE(r); REQUIRE(r->late == 1); REQUIRE(r->video_late == 1);
    REQUIRE_FALSE(m.too_heavy());
}

TEST_CASE("tube pacing: the report is INFO only when it says something new", "[tube]") {
    app::TubePacingMonitor m; auto t = app::TubePacingMonitor::Clock::time_point{} + 1h;
    m.take_report(t);
    auto minute = [&](bool video, bool one_late) {
        for (int i = 0; i < 100; ++i) { t += 16667us; m.on_present(t, FrameClass::Lean60, 60, video); }
        if (one_late) { t += 30000us; m.on_present(t, FrameClass::Lean60, 60, video); }
        t += 60s; return m.take_report(t);
    };
    auto r1 = minute(false, false);                         // the first report since the tube started
    REQUIRE(r1); REQUIRE(r1->first); REQUIRE(app::TubePacingMonitor::report_at_info(*r1));
    auto r2 = minute(false, true);                          // a still menu: late, but no video
    REQUIRE(r2); REQUIRE_FALSE(r2->first); REQUIRE_FALSE(app::TubePacingMonitor::report_at_info(*r2));
    auto r3 = minute(true, true);                           // a late present with video
    REQUIRE(r3); REQUIRE(r3->video_late == 1); REQUIRE(app::TubePacingMonitor::report_at_info(*r3));
    m.reset();                                              // a new look or class, or the tube restarted
    auto r4 = minute(false, false);
    REQUIRE(r4); REQUIRE(r4->first); REQUIRE(app::TubePacingMonitor::report_at_info(*r4));
}

TEST_CASE("tube pacing: the late threshold agrees with the held cadence of each class", "[tube]") {
    // Wherever a frame is held, the monitor's cadence is the hold's: a held
    // present on time is never late, and one a vblank later always is.
    for (const int hz : {50, 60, 100, 120, 144}) {
        for (const FrameClass cls : {FrameClass::Lean60, FrameClass::Rich30}) {
            const auto r = app::tube_redraw(true, cls, true, hz);
            const int cadence = app::TubePacingMonitor::cadence_vblanks(cls, hz);
            if (r.hold_present) CHECK(cadence == utils::held_vblanks_at(r.hold_fps, hz));
            const std::chrono::microseconds period{1000000 / hz};
            app::TubePacingMonitor m; auto t = app::TubePacingMonitor::Clock::time_point{} + 1h;
            m.take_report(t);
            for (int i = 0; i < 20; ++i) { t += period * cadence; m.on_present(t, cls, hz, true); }
            t += period * (cadence + 1); m.on_present(t, cls, hz, true);
            const auto rep = m.take_report(t + 60s);
            REQUIRE(rep); CHECK(rep->late == 1);
        }
    }
    // Lean at 120 Hz: held to two vblanks (16.67 ms); late past 20.8 ms.
    CHECK(app::TubePacingMonitor::cadence_vblanks(FrameClass::Lean60, 120) == 2);
}
