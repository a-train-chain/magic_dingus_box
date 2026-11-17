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
// The streak - and so its 60 s buckets - starts at the first present, 40-45 ms after
// t, so a Feed minute and a bucket of the monitor differ by at most one present.
struct Feed {
    app::TubePacingMonitor m;
    app::TubePacingMonitor::Clock::time_point t = app::TubePacingMonitor::Clock::time_point{} + 1h;
    std::uint64_t n = 0;
    void present(bool late, bool video = true) { t += late ? 45ms : 40ms; m.on_present(t, FrameClass::Rich30, 60, video); }
    // `span` of video with `pct` percent of the presents late, spread evenly.
    void run(std::chrono::milliseconds span, int pct) { run_pm(span, pct * 10); }
    void run_pm(std::chrono::milliseconds span, int permille) {
        for (const auto end = t + span; t < end; ++n) present((n + 1) * permille / 1000 != n * permille / 1000);
    }
    void minutes(int count, int pct) { run(std::chrono::minutes(count), pct); }
    void break_with(int kind) {   // 0: a present without video, 1: a 300 ms gap, 2: a 150 ms stall
        if (kind == 0) { present(false, false); return; }
        t += kind == 1 ? 300ms : 150ms; m.on_present(t, FrameClass::Rich30, 60, true);
    }
};
}  // namespace

TEST_CASE("tube pacing: three minutes each more than half late are too heavy, toast once", "[tube]") {
    Feed f; f.minutes(2, 67); f.run(59s, 67); REQUIRE_FALSE(f.m.too_heavy());  // the third minute is not over
    f.run(2s, 67);
    REQUIRE(f.m.too_heavy()); REQUIRE(f.m.take_toast()); REQUIRE_FALSE(f.m.take_toast());
    f.minutes(5, 67); REQUIRE_FALSE(f.m.take_toast());                        // still latched: not again
    f.m.reset(); REQUIRE_FALSE(f.m.too_heavy());
    Feed menu; for (int i = 0; i < 5000; ++i) menu.present(true, false);      // late, but no video
    REQUIRE_FALSE(menu.m.too_heavy());
}

TEST_CASE("tube pacing: exactly half late in each minute is not too heavy by (a)", "[tube]") {
    // On time 35 ms, late 45 ms: a pair is 80 ms and a minute 750 pairs, so every bucket
    // from the first (on-time) present holds 750 late and 750 on time.
    app::TubePacingMonitor m; auto t = app::TubePacingMonitor::Clock::time_point{} + 1h;
    m.on_present(t, FrameClass::Rich30, 60, true);
    const auto start = t;
    while (t - start < 4min) {
        t += 45ms; m.on_present(t, FrameClass::Rich30, 60, true);
        t += 35ms; m.on_present(t, FrameClass::Rich30, 60, true);
    }
    t += 45ms; m.on_present(t, FrameClass::Rich30, 60, true);                  // the fifth minute: four are closed
    REQUIRE_FALSE(m.too_heavy());
    auto r = m.take_report(t); REQUIRE_FALSE(r);
}

TEST_CASE("tube pacing: a busy burst of a few minutes is not too heavy", "[tube]") {
    // Hardware, Pi 5, 2026-11-16: the Media Browser's containers starting again after a
    // game ran a shipped look 12-13% late for two minutes, 20% for half a minute.
    Feed f; f.minutes(3, 13); f.minutes(10, 0); REQUIRE_FALSE(f.m.too_heavy());
    Feed g; g.minutes(3, 20); REQUIRE_FALSE(g.m.too_heavy());
    g.minutes(2, 0); g.minutes(3, 20); REQUIRE_FALSE(g.m.too_heavy());
    // PVM while the containers restarted: 7%, 40%, 20%, then 0.1%; Monitor (the lightest
    // look) in a minute of unexplained box load: 6%, 38%, then 0.5%. Both latched the
    // "half late over 30 s" rule.
    Feed pvm; pvm.minutes(1, 7); pvm.minutes(1, 40); pvm.minutes(1, 20); pvm.run_pm(10min, 1);
    REQUIRE_FALSE(pvm.m.too_heavy());
    Feed monitor; monitor.minutes(1, 6); monitor.minutes(1, 38); monitor.run_pm(10min, 5);
    REQUIRE_FALSE(monitor.m.too_heavy());
    Feed harsh; harsh.minutes(2, 60); harsh.minutes(10, 0); REQUIRE_FALSE(harsh.m.too_heavy());   // two is not three
}

TEST_CASE("tube pacing: three consecutive minutes over half late, whatever came before", "[tube]") {
    Feed f; f.minutes(5, 0); f.minutes(2, 60); f.run(59s, 60); REQUIRE_FALSE(f.m.too_heavy());
    f.run(2s, 60); REQUIRE(f.m.too_heavy());
    Feed g; g.minutes(2, 60); g.minutes(1, 40); g.minutes(2, 60); g.run(30s, 60);   // 40%: the count restarts
    REQUIRE_FALSE(g.m.too_heavy());
    g.run(32s, 60); REQUIRE(g.m.too_heavy());                                 // ... and three are three
}

TEST_CASE("tube pacing: a broken streak restarts the three minutes", "[tube]") {
    for (const int kind : {0, 1, 2}) {
        Feed f; f.minutes(1, 60); f.run(30s, 60); f.break_with(kind);
        f.minutes(2, 60); f.run(58s, 60); REQUIRE_FALSE(f.m.too_heavy());
        f.run(3s, 60); REQUIRE(f.m.too_heavy());
    }
    Feed r; r.minutes(1, 60); r.run(30s, 60); r.m.reset();
    r.minutes(2, 60); r.run(58s, 60); REQUIRE_FALSE(r.m.too_heavy());
    r.run(3s, 60); REQUIRE(r.m.too_heavy());
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
    auto heavy = [&] { for (int i = 0; i < 7000; ++i) { t += (i % 10 < 6) ? 33333us : 16667us; m.on_present(t, FrameClass::Lean60, 60, true); } };
    heavy(); REQUIRE(m.take_toast());
    heavy(); REQUIRE_FALSE(m.take_toast());                                   // same look: not again
    m.reset(); t += 1s; heavy(); REQUIRE(m.take_toast());                     // new look: once more
}

TEST_CASE("tube pacing: too heavy is latched until reset", "[tube]") {
    app::TubePacingMonitor m; auto t = app::TubePacingMonitor::Clock::time_point{} + 1h;
    for (int i = 0; i < 7000; ++i) { t += (i % 10 < 6) ? 33333us : 16667us; m.on_present(t, FrameClass::Lean60, 60, true); }
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

TEST_CASE("tube pacing: a look that is really too heavy latches after three minutes at 20, 15 and 12 fps", "[tube]") {
    for (const auto step : {50000us, 66667us, 83333us}) {
        app::TubePacingMonitor m; auto t = app::TubePacingMonitor::Clock::time_point{} + 1h;
        const auto start = t;
        for (const auto end = start + 179s; t + step < end;) { t += step; m.on_present(t, FrameClass::Rich30, 60, true); }
        REQUIRE_FALSE(m.too_heavy());                                         // 2 min 59 s
        for (const auto end = start + 181s; t + step < end;) { t += step; m.on_present(t, FrameClass::Rich30, 60, true); }
        REQUIRE(m.too_heavy()); REQUIRE(m.take_toast());                      // by 3 min 1 s
    }
}

TEST_CASE("tube pacing: a box that stalls every few seconds is not too heavy", "[tube]") {
    // Hardware, Pi 5, 1080p, 2026-11-16: the PVM look (19 of 23 ms) while the Media
    // Browser's containers restarted - page flips waited 48-514 ms, 475 of 1173 late.
    app::TubePacingMonitor m; auto t = app::TubePacingMonitor::Clock::time_point{} + 1h;
    m.take_report(t);
    for (const auto end = t + 120s; t < end;) {
        t += 120ms; m.on_present(t, FrameClass::Rich30, 60, true);            // a stall
        for (int i = 0; i < 48; ++i) { t += (i % 2 == 0) ? 50000us : 33333us; m.on_present(t, FrameClass::Rich30, 60, true); }
    }
    auto r = m.take_report(t); REQUIRE(r); REQUIRE(r->late * 2 > r->presents);   // more than half late
    REQUIRE_FALSE(m.too_heavy()); REQUIRE_FALSE(m.take_toast());
}

TEST_CASE("tube pacing: a stall counts in the report, a gap does not", "[tube]") {
    app::TubePacingMonitor m; auto t = app::TubePacingMonitor::Clock::time_point{} + 1h;
    m.take_report(t);
    m.on_present(t += 33333us, FrameClass::Rich30, 60, true);
    m.on_present(t += 150ms, FrameClass::Rich30, 60, true);                    // a stall: a late present
    m.on_present(t += 33333us, FrameClass::Rich30, 60, true);
    m.on_present(t += 251ms, FrameClass::Rich30, 60, true);                    // a gap: nothing
    m.on_present(t += 33333us, FrameClass::Rich30, 60, false);
    m.on_present(t += 150ms, FrameClass::Rich30, 60, false);                   // a stall without video
    auto r = m.take_report(t + 60s); REQUIRE(r);
    REQUIRE(r->presents == 5); REQUIRE(r->late == 2);
    REQUIRE(r->video_presents == 3); REQUIRE(r->video_late == 1);
}

TEST_CASE("tube pacing: a stall restarts the ten minutes", "[tube]") {
    Feed f; f.minutes(9, 6);
    f.t += 150ms; f.m.on_present(f.t, FrameClass::Rich30, 60, true);          // a stall
    f.minutes(9, 6); REQUIRE_FALSE(f.m.too_heavy());
    f.minutes(1, 6); f.run(2s, 6); REQUIRE(f.m.too_heavy());
}

TEST_CASE("tube pacing: a stall is an interval over 100 ms", "[tube]") {
    for (const auto stall : {100000us, 100001us}) {
        Feed f; f.run(90s, 100);
        f.t += stall; f.m.on_present(f.t, FrameClass::Rich30, 60, true);
        f.run(91s, 100);
        CHECK(f.m.too_heavy() == (stall == 100000us));                         // 100 ms stays in the streak
        auto r = f.m.take_report(f.t); REQUIRE_FALSE(r);
    }
    app::TubePacingMonitor m; auto t = app::TubePacingMonitor::Clock::time_point{} + 1h;
    m.take_report(t);
    m.on_present(t += 33333us, FrameClass::Rich30, 60, true);
    m.on_present(t += 100ms, FrameClass::Rich30, 60, true);                    // exactly 100 ms: late
    auto r = m.take_report(t + 60s); REQUIRE(r); REQUIRE(r->presents == 2); REQUIRE(r->late == 1);
}
