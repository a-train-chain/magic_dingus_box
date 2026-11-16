#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

#include "tube/tube_driver.h"

using namespace tube;
using Catch::Approx;

const Picture k1080{1440.0, 1080.0, false};

// ---- The brief's tests, verbatim ---------------------------------------------

TEST_CASE("tube driver: warm-up from start, then still", "[tube]") {
    FrameDriver d; TubeSetting s; set_row(s, Effect::afterglow, 0); d.start(100.0, 0);   // no settle debt
    REQUIRE(d.frame(s, k1080, 100.1, 0).u.warmupGain == 0.0f);
    REQUIRE(d.peek(s, k1080, 101.0, 0).moving);
    REQUIRE(d.frame(s, k1080, 102.0, 0).u.warmupGain == 1.0f);
    REQUIRE_FALSE(d.peek(s, k1080, 102.1, 0).moving);           // nothing moves, nothing owed
}
TEST_CASE("tube driver: degauss when the counter rises, never at start", "[tube]") {
    FrameDriver d; TubeSetting s; set_row(s, Effect::afterglow, 0); d.start(0.0, 7);
    REQUIRE(d.frame(s, k1080, 3.0, 7).u.degauss == 0.0f);
    REQUIRE(d.frame(s, k1080, 4.0, 8).u.degauss == Approx(1.0f));
    REQUIRE(d.frame(s, k1080, 4.7, 8).u.degauss == Approx(0.5f));
    REQUIRE(d.peek(s, k1080, 5.45, 8).moving);
    REQUIRE(d.frame(s, k1080, 5.6, 8).u.degauss == 0.0f);
    REQUIRE_FALSE(d.peek(s, k1080, 5.6, 8).moving);
}
TEST_CASE("tube driver: the plan changes only when a row crosses neutral", "[tube]") {
    FrameDriver d; TubeSetting s; d.start(0.0, 0); const PassPlan p0 = d.frame(s, k1080, 2.0, 0).plan;
    set_row(s, Effect::brightness, 70);
    auto f = d.frame(s, k1080, 2.1, 0); REQUIRE(f.still_changed); REQUIRE(f.plan == p0);
    REQUIRE_FALSE(d.frame(s, k1080, 2.2, 0).still_changed);
    set_row(s, Effect::afterglow, 0); set_row(s, Effect::glow, 0); set_row(s, Effect::sharpness, 50);
    set_row(s, Effect::noise, 10);
    REQUIRE_FALSE(d.frame(s, k1080, 2.3, 0).plan == p0);
}
TEST_CASE("tube driver: the glass rebakes only for its own rows", "[tube]") {
    FrameDriver d; TubeSetting s; d.start(0.0, 0);
    REQUIRE(d.frame(s, k1080, 2.0, 0).glass_changed);          // the first frame bakes
    set_row(s, Effect::brightness, 70); REQUIRE_FALSE(d.frame(s, k1080, 2.1, 0).glass_changed);
    set_row(s, Effect::vignette, 60);   REQUIRE(d.frame(s, k1080, 2.2, 0).glass_changed);
    REQUIRE(d.frame(s, Picture{960.0, 720.0, false}, 2.3, 0).glass_changed);
}
TEST_CASE("tube driver: a new picture size rebuilds the still", "[tube]") {
    FrameDriver d; TubeSetting s; d.start(0.0, 0);
    auto a = d.frame(s, k1080, 2.0, 0);
    auto b = d.frame(s, Picture{960.0, 720.0, false}, 2.1, 0);
    REQUIRE(b.still_changed); REQUIRE(a.u.lines == Approx(270.0)); REQUIRE(b.u.lines == Approx(240.0));
    auto c = d.frame(s, Picture{1280.0, 720.0, true}, 2.2, 0);     // CRT Native
    REQUIRE(c.u.maskType == 0.0f); REQUIRE(c.u.scanlines == 0.0f); REQUIRE(c.u.cornerPx == 0.0f);
}
TEST_CASE("tube driver: the band moves by dt, and not at all while not drawn", "[tube]") {
    FrameDriver d; TubeSetting s; set_row(s, Effect::rollBand, 50); d.start(0.0, 0);
    const float p0 = d.frame(s, k1080, 2.0, 0).u.rollPosition;
    const float p1 = d.frame(s, k1080, 2.1, 0).u.rollPosition;
    REQUIRE(p1 - p0 == Approx(0.1 / 21.6).margin(1e-6));
    const float p2 = d.frame(s, k1080, 600.0, 0).u.rollPosition;  // ten minutes in the Media Browser
    REQUIRE(p2 - p1 == Approx(0.1 / 21.6).margin(1e-6));
}
TEST_CASE("tube driver: the limiter follows the mean on the frames' clock", "[tube]") {
    AblLimiter a; a.on_mean(0.8); REQUIRE(a.level() == Approx(0.8));  // the first picture is not news
    a.on_mean(0.2); a.step(10.0);                                      // from rest: one 30 Hz frame
    REQUIRE(a.level() == Approx(0.8 - 0.6 * (1.0 - std::exp(-(1.0 / 30.0) / 0.1))));
}
TEST_CASE("tube driver: the mask tile passes what its gain gives back", "[tube]") {
    for (auto m : {Mask::grille, Mask::magentaGreen, Mask::dots, Mask::slot}) for (int pitch : {3, 4, 6}) {
        const MaskTile t = mask_tile(m, pitch, 0.48f); double sum = 0;
        for (std::size_t i = 0; i < t.rgb.size(); ++i) sum += t.rgb[i] / 255.0;
        INFO(mask_name(m) << " " << pitch);
        REQUIRE(sum / t.rgb.size() * mask_gain(m, 0.48f) == Approx(1.0).margin(0.01));
    }
    REQUIRE(mask_tile(Mask::none, 3, 0.5f).w == 1);
}

// ---- Added: what the brief's tests do not pin --------------------------------

namespace {
// PVM with every 30 fps row at neutral: the lean path (bend, beam, tone, displace).
TubeSetting lean_pvm() {
    TubeSetting s;
    for (Effect e : {Effect::glow, Effect::afterglow, Effect::abl, Effect::breathing}) set_row(s, e, 0);
    set_row(s, Effect::sharpness, 50);
    return s;
}
}  // namespace

TEST_CASE("tube driver: the mask tile is one period of the pattern, sized for it", "[tube]") {
    const MaskTile none = mask_tile(Mask::none, 3, 0.5f);
    REQUIRE(none.h == 1);
    REQUIRE(none.rgb == std::vector<std::uint8_t>{255, 255, 255});
    const MaskTile grille = mask_tile(Mask::grille, 3, 0.8f);
    REQUIRE(grille.w == 3); REQUIRE(grille.h == 1);
    REQUIRE(grille.rgb.size() == 9u);
    // Pitch 3, full-height stripes: red, green, blue, each the others at 1 - strength.
    REQUIRE(grille.rgb[0] == 255); REQUIRE(grille.rgb[1] == 51); REQUIRE(grille.rgb[2] == 51);
    REQUIRE(grille.rgb[4] == 255); REQUIRE(grille.rgb[8] == 255);
    const MaskTile dots = mask_tile(Mask::dots, 3, 0.5f);
    REQUIRE(dots.w == 3); REQUIRE(dots.h == 4);   // 4 x max(floor((3 + 1) / 3), 1) rows
    const MaskTile slot = mask_tile(Mask::slot, 4, 0.5f);
    REQUIRE(slot.w == 4); REQUIRE(slot.h == 16);  // two slot heights of 2 x pitch
    REQUIRE(mask_tile(Mask::grille, 0, 0.5f).w >= 1);   // a nonsense pitch never makes an empty tile
}

TEST_CASE("tube driver: the mask's pixels ride only the frame that changed them", "[tube]") {
    FrameDriver d; TubeSetting s; d.start(0.0, 0);
    const DriverFrame first = d.frame(s, k1080, 2.0, 0);
    REQUIRE(first.mask_changed);
    REQUIRE(first.mask.rgb.size() == static_cast<std::size_t>(first.mask.w * first.mask.h * 3));
    REQUIRE_FALSE(first.mask.rgb.empty());
    // An unchanged mask: no pixels copied (the renderer keeps the texture it has), the
    // size still said, and the driver's own copy still there for a re-upload.
    set_row(s, Effect::brightness, 70);
    const DriverFrame next = d.frame(s, k1080, 2.1, 0);
    REQUIRE_FALSE(next.mask_changed);
    REQUIRE(next.mask.rgb.empty());
    REQUIRE(next.mask.w == first.mask.w); REQUIRE(next.mask.h == first.mask.h);
    REQUIRE(d.mask().rgb == first.mask.rgb);
    // Strength, pitch or pattern: a new tile.
    set_row(s, Effect::maskStrength, 30);
    const DriverFrame weaker = d.frame(s, k1080, 2.2, 0);
    REQUIRE(weaker.mask_changed); REQUIRE_FALSE(weaker.mask.rgb.empty());
    REQUIRE(weaker.mask.rgb != first.mask.rgb);
    set_mask(s, Mask::slot);
    const DriverFrame slot = d.frame(s, k1080, 2.3, 0);
    REQUIRE(slot.mask_changed); REQUIRE(slot.mask.h == 4 * slot.mask.w);
}

TEST_CASE("tube driver: apply_plan zeroes the rows of a pass the plan dropped", "[tube]") {
    TubeUniforms u{};
    u.glow = 0.3f; u.persistence = 0.4f;
    PassPlan p; p.cls = FrameClass::Rich30; p.glow = true; p.afterglow = true;
    apply_plan(u, p);
    REQUIRE(u.glow == 0.3f); REQUIRE(u.persistence == 0.4f);
    p.afterglow = false;
    apply_plan(u, p);
    REQUIRE(u.glow == 0.3f); REQUIRE(u.persistence == 0.0f);
    p.glow = false;
    apply_plan(u, p);
    REQUIRE(u.glow == 0.0f);
}

TEST_CASE("tube driver: a picture whose plan drops glow draws no glow", "[tube]") {
    // The TV look at 1440x1110: over the rich budget as asked and without afterglow, under
    // it without both, so plan_passes drops the afterglow and the glow (tube_programs.h).
    // Every rich output program still compiles OUT_GLOW, so a glow left in the uniforms
    // would read a stale texture.
    TubeSetting s; select_look(s, Look::tv);
    const Picture wide{1440.0, 1110.0, false};
    const Resolved r = resolve(s);
    REQUIRE(r.effects[index_of(Effect::glow)] > 0);
    REQUIRE(r.effects[index_of(Effect::afterglow)] > 0);
    REQUIRE_FALSE(plan_passes(r, wide).glow);
    FrameDriver d; d.start(0.0, 0);
    const DriverFrame f = d.frame(s, wide, 2.0, 0);
    REQUIRE_FALSE(f.plan.glow); REQUIRE_FALSE(f.plan.afterglow);
    REQUIRE(f.u.glow == 0.0f);
    REQUIRE(f.u.persistence == 0.0f);
    REQUIRE(f.glow_sigma == 0.0f);
    // The same look at the reference size keeps the glow; it cannot afford afterglow there
    // either (the rule applies at every picture size), so its persistence reads 0 too.
    const DriverFrame ref = d.frame(s, k1080, 2.1, 0);
    REQUIRE(ref.plan.glow); REQUIRE(ref.u.glow > 0.0f);
    REQUIRE_FALSE(ref.plan.afterglow); REQUIRE(ref.u.persistence == 0.0f);
    REQUIRE(ref.glow_sigma > 0.0f);
    // Trinitron at the reference size affords both.
    TubeSetting trin; select_look(trin, Look::trinitron);
    const DriverFrame both = d.frame(trin, k1080, 2.2, 0);
    REQUIRE(both.plan.glow); REQUIRE(both.plan.afterglow);
    REQUIRE(both.u.glow > 0.0f); REQUIRE(both.u.persistence > 0.0f);
}

TEST_CASE("tube driver: no mean is asked for on a plan without the probe, and the limiter rests", "[tube]") {
    FrameDriver d; const TubeSetting s = lean_pvm(); d.start(0.0, 0);
    const DriverFrame f = d.frame(s, k1080, 2.0, 0);
    REQUIRE(f.plan.cls == FrameClass::Lean60);
    REQUIRE_FALSE(f.plan.mean_probe);
    REQUIRE_FALSE(d.mean_due(2.0));
    REQUIRE_FALSE(d.mean_due(100.0));
    d.on_mean(0.9);                                    // a stray reading is ignored
    const DriverFrame g = d.frame(s, k1080, 2.1, 0);
    REQUIRE(g.u.ablLevel == 0.0f);
    REQUIRE_FALSE(g.moving);
}

TEST_CASE("tube driver: a rich plan with the probe asks for a mean every period and follows it", "[tube]") {
    FrameDriver d; TubeSetting s = lean_pvm(); set_row(s, Effect::abl, 50); d.start(0.0, 0);
    const DriverFrame f = d.frame(s, k1080, 2.0, 0);
    REQUIRE(f.plan.cls == FrameClass::Rich30);        // the probe is not in the measured lean set
    REQUIRE(f.plan.mean_probe);
    REQUIRE(d.mean_due(2.0));
    d.note_mean_requested(2.0);
    REQUIRE_FALSE(d.mean_due(2.0 + kMeanPeriodSeconds / 2));
    REQUIRE(d.mean_due(2.0 + kMeanPeriodSeconds));
    d.on_mean(0.5);                                    // the first reading lands at once
    REQUIRE(d.frame(s, k1080, 2.05, 0).u.ablLevel == Approx(0.5f));
    REQUIRE_FALSE(d.peek(s, k1080, 2.06, 0).moving);
    d.on_mean(0.1);                                    // a darker picture: the limiter travels, frames owed
    const DriverFrame g = d.frame(s, k1080, 2.1, 0);
    REQUIRE(g.u.ablLevel < 0.5f); REQUIRE(g.u.ablLevel > 0.1f);
    REQUIRE(g.moving);
    double now = 2.1;
    int frames = 1;
    while (d.peek(s, k1080, now, 0).moving && frames < 100) {
        now += 1.0 / 30.0;
        d.frame(s, k1080, now, 0);
        ++frames;
    }
    REQUIRE(frames == settle_frames(f.u, 0.4));        // exactly the debt the new target armed
    REQUIRE(d.frame(s, k1080, now + 1.0, 0).u.ablLevel == Approx(0.1f).margin(1e-3));
    // Leaving the probe's plan rests the limiter at its neutral level.
    set_row(s, Effect::abl, 0);
    const DriverFrame off = d.frame(s, k1080, now + 2.0, 0);
    REQUIRE_FALSE(off.plan.mean_probe);
    REQUIRE(off.u.ablLevel == 0.0f);
    REQUIRE_FALSE(d.mean_due(now + 10.0));
}

TEST_CASE("tube driver: afterglow owes its settle frames after a still change", "[tube]") {
    FrameDriver d; TubeSetting s; set_row(s, Effect::abl, 0); set_row(s, Effect::breathing, 0); d.start(0.0, 0);
    const DriverFrame f = d.frame(s, k1080, 2.0, 0);   // PVM's AFTERGLOW 10: 4 frames, this one included
    REQUIRE(settle_frames(f.u, 0.0) == 4);
    REQUIRE(f.moving);
    REQUIRE(d.frame(s, k1080, 2.1, 0).moving);
    REQUIRE(d.frame(s, k1080, 2.2, 0).moving);
    REQUIRE_FALSE(d.frame(s, k1080, 2.3, 0).moving);
}

TEST_CASE("tube driver: a tracking row restarts the tracking clock", "[tube]") {
    FrameDriver d; TubeSetting s; set_row(s, Effect::afterglow, 0); set_row(s, Effect::tracking, 50);
    d.start(0.0, 0);
    REQUIRE(d.frame(s, k1080, 0.05, 0).u.trackingHalfWidth > 0.0f);   // a slip at the clock's start
    REQUIRE(d.frame(s, k1080, 5.0, 0).u.trackingHalfWidth == 0.0f);    // asleep between slips
    REQUIRE_FALSE(d.peek(s, k1080, 5.0, 0).moving);
    set_row(s, Effect::trackingRate, 30);
    REQUIRE(d.frame(s, k1080, 5.05, 0).u.trackingHalfWidth > 0.0f);   // the clock restarted
    set_row(s, Effect::brightness, 70);                                 // any other row leaves it alone
    REQUIRE(d.frame(s, k1080, 9.0, 0).u.trackingHalfWidth == 0.0f);
}

TEST_CASE("tube driver: peek changes nothing and calls a pending change moving", "[tube]") {
    FrameDriver d; TubeSetting s; set_row(s, Effect::afterglow, 0); d.start(0.0, 3);
    REQUIRE(d.frame(s, k1080, 5.0, 3).u.degauss == 0.0f);
    REQUIRE_FALSE(d.peek(s, k1080, 5.1, 3).moving);
    REQUIRE(d.peek(s, k1080, 5.1, 4).moving);                // a degauss is waiting
    TubeSetting noisy = s; set_row(noisy, Effect::noise, 20);
    const DriverPeek p = d.peek(noisy, k1080, 5.1, 3);       // a setting not yet drawn
    REQUIRE(p.moving); REQUIRE(p.cls == FrameClass::Rich30);
    REQUIRE_FALSE(d.peek(s, k1080, 5.1, 3).moving);          // and none of it stuck
    REQUIRE(d.frame(s, k1080, 5.2, 3).u.degauss == 0.0f);
    REQUIRE_FALSE(d.frame(s, k1080, 5.3, 3).still_changed);
}

TEST_CASE("tube driver: a counter that goes backwards is a new baseline, not a degauss", "[tube]") {
    FrameDriver d; TubeSetting s; set_row(s, Effect::afterglow, 0); d.start(0.0, 9);
    REQUIRE(d.frame(s, k1080, 3.0, 2).u.degauss == 0.0f);   // the op log restarted its count
    REQUIRE(d.frame(s, k1080, 4.0, 3).u.degauss == Approx(1.0f));
}

// ---- Fix round 1 ---------------------------------------------------------------

namespace {
// PVM, the limiter out of it, TRACKING 50 at AFTERGLOW `afterglow`.
TubeSetting slipping(int afterglow) {
    TubeSetting s;
    set_row(s, Effect::abl, 0); set_row(s, Effect::breathing, 0);
    set_row(s, Effect::afterglow, afterglow); set_row(s, Effect::tracking, 50);
    return s;
}
// Draws 30 Hz frames from `from` until the slip that starts there has ended; returns
// the time of the last frame that drew a displaced band.
double last_slip_frame(FrameDriver& d, const TubeSetting& s, double from) {
    double last = -1.0;
    for (int i = 0; i < 60; ++i) {
        const double now = from + i / 30.0;
        if (d.frame(s, k1080, now, 0).u.trackingOffset != 0.0f) last = now;
    }
    return last;
}
}  // namespace

TEST_CASE("tube driver: a tracking slip owes the afterglow's settle frames after it", "[tube]") {
    const TubeSetting s = slipping(30);
    FrameDriver d; d.start(0.0, 0);
    // Past the first slip (0 .. 0.42 s), the start's settling and the warm-up.
    REQUIRE(last_slip_frame(d, s, 0.0) > 0.0);
    REQUIRE_FALSE(d.peek(s, k1080, 5.0, 0).moving);
    TubeSetting again = s; set_row(again, Effect::trackingRate, 30);   // restarts the clock at 5.0
    const int owed = settle_frames(d.frame(again, k1080, 5.0, 0).u, 0.0);
    REQUIRE(owed == 6);                                // AFTERGLOW 30
    // Replay the slip on a twin, frame for frame, to find its last displaced frame.
    FrameDriver twin = d;
    const double last = last_slip_frame(twin, again, 5.0 + 1.0 / 30.0);
    REQUIRE(last > 5.0);
    double now = 5.0;
    while (now < last - 1e-9) { now += 1.0 / 30.0; d.frame(again, k1080, now, 0); }
    // The slip's own clean-frame window (tracking_moving: + 0.05 s) is long gone by
    // the time the history has decayed; the debt alone keeps the frames coming.
    for (int i = 1; i < owed; ++i) {
        now += 1.0 / 30.0;
        INFO("frame " << i << " after the slip");
        REQUIRE(d.frame(again, k1080, now, 0).moving);
    }
    now += 1.0 / 30.0;
    REQUIRE_FALSE(d.frame(again, k1080, now, 0).moving);
}

TEST_CASE("tube driver: a tracking slip owes one clean frame even without afterglow", "[tube]") {
    TubeSetting s = slipping(0);
    FrameDriver d; d.start(0.0, 0);
    last_slip_frame(d, s, 0.0);                        // past the first slip and the warm-up
    set_row(s, Effect::trackingRate, 30);              // restarts the clock at 5.0
    double last = -1.0, now = 0.0;
    for (int i = 0; i < 60; ++i) {
        now = 5.0 + i / 30.0;
        if (d.frame(s, k1080, now, 0).u.trackingOffset != 0.0f) { last = now; break; }
    }
    REQUIRE(last >= 0.0);
    // Well past the slip's own window, the frame it owes is still owed.
    REQUIRE(d.peek(s, k1080, last + 3.0, 0).moving);
    // Draw on until the slip ends: one more frame after its last displaced one, then rest.
    double end = last;
    for (int i = 0; i < 60; ++i) {
        now += 1.0 / 30.0;
        if (d.frame(s, k1080, now, 0).u.trackingOffset != 0.0f) end = now;
        else if (now > end + 0.5) break;
    }
    REQUIRE_FALSE(d.peek(s, k1080, now + 0.01, 0).moving);
}

TEST_CASE("tube driver: a clock that runs backwards keeps degauss and warm-up in range", "[tube]") {
    FrameDriver d; TubeSetting s; d.start(10.0, 5);
    const auto in_range = [](const DriverFrame& f) {
        float fields[sizeof(TubeUniforms) / sizeof(float)];
        static_assert(sizeof(TubeUniforms) % sizeof(float) == 0, "TubeUniforms is floats");
        std::memcpy(fields, &f.u, sizeof(fields));
        for (float v : fields) REQUIRE(std::isfinite(v));
        REQUIRE(f.u.degauss >= 0.0f); REQUIRE(f.u.degauss <= 1.0f);
        REQUIRE(f.u.warmupGain >= 0.0f); REQUIRE(f.u.warmupGain <= 1.0f);
    };
    in_range(d.frame(s, k1080, 4.0, 5));            // before start()
    in_range(d.frame(s, k1080, 12.0, 6));           // a degauss starts at 12
    const DriverFrame back = d.frame(s, k1080, 11.0, 6);   // and the clock steps back past it
    in_range(back);
    REQUIRE(back.u.degauss == 1.0f);
    in_range(d.frame(s, k1080, -100.0, 6));
}

TEST_CASE("tube driver: the tile follows the still's mask type, CRT Native included", "[tube]") {
    FrameDriver d; TubeSetting s; set_mask(s, Mask::dots); d.start(0.0, 0);
    REQUIRE(d.frame(s, k1080, 2.0, 0).mask.rgb == mask_tile(Mask::dots, 3, 60 * 0.008f).rgb);
    const DriverFrame native = d.frame(s, Picture{1280.0, 720.0, true}, 2.1, 0);
    REQUIRE(native.u.maskType == 0.0f);
    REQUIRE(native.mask_changed);
    REQUIRE(native.mask.w == 1); REQUIRE(native.mask.h == 1);
    REQUIRE(native.mask.rgb == std::vector<std::uint8_t>{255, 255, 255});
}

TEST_CASE("tube driver: gpu_lost re-sends the glass, mask and targets and touches no clock", "[tube]") {
    TubeSetting s; set_row(s, Effect::rollBand, 50);
    FrameDriver d, twin; d.start(0.0, 0); twin.start(0.0, 0);
    d.frame(s, k1080, 0.5, 0); twin.frame(s, k1080, 0.5, 0);
    d.frame(s, k1080, 0.6, 0); twin.frame(s, k1080, 0.6, 0);
    d.gpu_lost();
    const DriverFrame f = d.frame(s, k1080, 0.7, 0), g = twin.frame(s, k1080, 0.7, 0);
    REQUIRE(f.glass_changed); REQUIRE(f.mask_changed); REQUIRE(f.still_changed);
    REQUIRE_FALSE(f.mask.rgb.empty());
    REQUIRE(f.mask.rgb == d.mask().rgb);
    REQUIRE_FALSE(g.glass_changed); REQUIRE_FALSE(g.mask_changed); REQUIRE_FALSE(g.still_changed);
    REQUIRE(f.u.warmupGain == g.u.warmupGain); REQUIRE(f.u.warmupGain < 1.0f);   // still warming up
    REQUIRE(f.u.rollPosition == g.u.rollPosition);                               // the phases go on
    REQUIRE(f.u.ablLevel == g.u.ablLevel);
    const DriverFrame next = d.frame(s, k1080, 0.8, 0);
    REQUIRE_FALSE(next.glass_changed); REQUIRE_FALSE(next.mask_changed); REQUIRE_FALSE(next.still_changed);
    REQUIRE(next.mask.rgb.empty());
    REQUIRE(next.u.rollPosition == twin.frame(s, k1080, 0.8, 0).u.rollPosition);
}

// ---- Whole-branch fix wave ------------------------------------------------------

TEST_CASE("tube driver: a degauss asked for while the tube was not drawing is skipped", "[tube]") {
    FrameDriver d; TubeSetting s; set_row(s, Effect::afterglow, 0); d.start(0.0, 3);
    REQUIRE(d.frame(s, k1080, 5.0, 3).u.degauss == 0.0f);
    // The Media Browser had the screen: frame() was not called while the op log's
    // counter rose from 3 to 5. Without the skip, the first frame back degausses.
    REQUIRE(d.peek(s, k1080, 60.0, 5).moving);
    d.skip_degauss_to(5);
    REQUIRE_FALSE(d.peek(s, k1080, 60.0, 5).moving);          // peek agrees: nothing owed
    const DriverFrame back = d.frame(s, k1080, 60.0, 5);
    REQUIRE(back.u.degauss == 0.0f);
    REQUIRE_FALSE(back.moving);
    REQUIRE_FALSE(back.still_changed);                         // nothing else was touched
    // A press after the tube is drawing again still degausses.
    REQUIRE(d.peek(s, k1080, 61.0, 6).moving);
    REQUIRE(d.frame(s, k1080, 61.0, 6).u.degauss == Approx(1.0f));
}

TEST_CASE("tube driver: skipping to a lower counter is only a new baseline", "[tube]") {
    FrameDriver d; TubeSetting s; set_row(s, Effect::afterglow, 0); d.start(0.0, 9);
    REQUIRE(d.frame(s, k1080, 5.0, 9).u.degauss == 0.0f);
    d.skip_degauss_to(2);                                      // the op log restarted its count
    REQUIRE_FALSE(d.peek(s, k1080, 6.0, 2).moving);
    REQUIRE(d.frame(s, k1080, 6.0, 2).u.degauss == 0.0f);
    REQUIRE(d.frame(s, k1080, 7.0, 3).u.degauss == Approx(1.0f));
}
