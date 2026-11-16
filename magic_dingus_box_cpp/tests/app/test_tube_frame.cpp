// What one frame owns (tube/tube_inputs.h): the phases, the optional
// character, warm-up and the frames a picture owes before it settles.
//
// The first block is the plan's own cases. The second is ported from Sound
// Factory's engine/tests/test_tube_inputs.cpp at commit 4ef74a6: the cases that
// still apply once there is no gate and no bypass and the picture is the
// source's own size (so a millimetre is `u.pxPerMm` pixels).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include "tube/tube_inputs.h"
using namespace tube;
using Catch::Approx;

TEST_CASE("tube frame: warm-up", "[tube]") {
    REQUIRE(warmup_gain(0.0) == 0.0);  REQUIRE(warmup_gain(0.2) == 0.0);
    REQUIRE(warmup_gain(0.5) == Approx(0.4));
    REQUIRE(warmup_gain(0.8) == Approx(0.8));
    REQUIRE(warmup_gain(1.15) == Approx(0.9));
    REQUIRE(warmup_gain(1.5) == Approx(1.0));  REQUIRE(warmup_gain(99.0) == 1.0);
}
TEST_CASE("tube frame: settling debt", "[tube]") {
    TubeUniforms u{};
    REQUIRE(settle_frames(u, 0.0) == 0);
    u.persistence = 0.10f; REQUIRE(settle_frames(u, 0.0) == 4);
    u.persistence = 0.30f; REQUIRE(settle_frames(u, 0.0) == 6);
    u.persistence = 1.00f; REQUIRE(settle_frames(u, 0.0) == 30);
    u.persistence = 0.0f; u.abl = 0.2f; REQUIRE(settle_frames(u, 1.0) == 26);
    u.abl = 0.0f; REQUIRE(settle_frames(u, 1.0) == 0);
}
TEST_CASE("tube frame: phases stay smooth after 30 days", "[tube]") {
    const double t0 = 30.0 * 86400.0, dt = 1.0 / 60.0;
    TubeUniforms a{}, b{};
    tube_frame_phases(a, t0, 155'520'000ull);
    tube_frame_phases(b, t0 + dt, 155'520'001ull);
    double d = b.humPhase - a.humPhase; if (d < 0) d += 1.0;
    REQUIRE(d == Approx(dt / 16.7).margin(1e-6));           // advanced by exactly one frame's worth
    REQUIRE(a.humPhase >= 0.0f); REQUIRE(a.humPhase < 1.0f);
    REQUIRE(std::abs(a.flickerWave) <= 1.0f);
    REQUIRE(b.flickerWave != a.flickerWave);                // not frozen
    REQUIRE(a.degaussPhaseA >= 0.0f); REQUIRE(a.degaussPhaseA < 6.2832f);
    REQUIRE(a.jitterSeed < 1024.0f);
    REQUIRE(b.noiseSeed != a.noiseSeed);
}
TEST_CASE("tube frame: the rolling band", "[tube]") {
    TubeSetting s; set_row(s, Effect::rollBand, 100);        // pvm: rollSpeed 35, rollWidth 35, rollTint 50
    auto r = resolve(s); auto u = tube_uniforms_for(r, Picture{1440.0, 1080.0, false});
    CharacterPhases p; p.roll = 0.5;                         // half a pass
    tube_character_frame(u, r, p, 0.0, true);
    REQUIRE(u.rollAmount == Approx(2.0));
    REQUIRE(u.rollPosition == Approx(0.5));                  // the band sits where its phase says
    REQUIRE(roll_period(r) == Approx(21.6));                 // pvm: a pass every 21.6 s
    REQUIRE(u.rollHalfWidth == Approx(13.1 * (1440.0 / 196.608) / 1080.0));
    REQUIRE(u.rollTint == Approx(0.0));
    tube_character_frame(u, r, p, 0.0, false);
    REQUIRE(u.rollAmount == 0.0f);                           // disabled zeroes everything
}
TEST_CASE("tube frame: a speed step does not move the band", "[tube]") {
    TubeSetting s; set_row(s, Effect::rollBand, 100);          // pvm: rollSpeed 35 -> 21.6 s
    auto r = resolve(s); auto u = tube_uniforms_for(r, Picture{1440.0, 1080.0, false});
    CharacterPhases p;
    for (int i = 0; i < 108; ++i) advance_character_phases(p, r, 0.1);
    REQUIRE(p.roll == Approx(0.5).margin(1e-9));
    tube_character_frame(u, r, p, 0.0, true); const float before = u.rollPosition;
    set_row(s, Effect::rollSpeed, 100); r = resolve(s);         // 6 s a pass
    tube_character_frame(u, r, p, 0.0, true);
    REQUIRE(u.rollPosition == before);                          // the step moved nothing
    advance_character_phases(p, r, 0.1);
    REQUIRE(p.roll == Approx(0.5 + 0.1 / 6.0).margin(1e-9));    // only the rate changed
}
TEST_CASE("tube frame: drift and flagging follow their accumulated phase", "[tube]") {
    auto r = resolve(TubeSetting{});
    r.effects[index_of(Effect::colorDrift)] = 100; r.effects[index_of(Effect::flagging)] = 100;
    auto u = tube_uniforms_for(r, Picture{1440.0, 1080.0, false});
    CharacterPhases p; p.drift = 0.25; p.flag = 0.0;
    tube_character_frame(u, r, p, 0.0, true);
    REQUIRE(u.driftOffset == Approx(0.25 * u.pxPerMm * 3.0));   // sin(pi/2) at character_strength(1, 3) = 3
    REQUIRE(u.flagOffset == Approx(0.0).margin(1e-6));
}
TEST_CASE("tube frame: a long gap advances by at most 0.1 s", "[tube]") {
    auto r = resolve(TubeSetting{}); CharacterPhases p;
    advance_character_phases(p, r, 5.0);                          // a game, the Media Browser
    REQUIRE(p.roll == Approx(0.1 / roll_period(r)));
    advance_character_phases(p, r, -1.0);                         // a clock that went backwards
    REQUIRE(p.roll == Approx(0.1 / roll_period(r)));
}
TEST_CASE("tube frame: accumulated phases stay in one cycle after a month of frames", "[tube]") {
    auto r = resolve(TubeSetting{}); CharacterPhases p;
    p.roll = 0.999999; p.drift = 0.999999; p.flag = 0.999999;
    for (int i = 0; i < 1000; ++i) advance_character_phases(p, r, 1.0 / 60.0);
    for (double x : {p.roll, p.drift, p.flag}) { REQUIRE(x >= 0.0); REQUIRE(x < 1.0); }
}
TEST_CASE("tube frame: each speed row sets its own period", "[tube]") {
    auto r = resolve(TubeSetting{});
    r.effects[index_of(Effect::rollSpeed)] = 0; r.effects[index_of(Effect::driftSpeed)] = 0;
    r.effects[index_of(Effect::flagSpeed)] = 0;
    CHECK(roll_period(r) == Approx(30.0)); CHECK(drift_period(r) == Approx(24.0)); CHECK(flag_period(r) == Approx(12.0));
    r.effects[index_of(Effect::rollSpeed)] = 100; r.effects[index_of(Effect::driftSpeed)] = 100;
    r.effects[index_of(Effect::flagSpeed)] = 100;
    CHECK(roll_period(r) == Approx(6.0)); CHECK(drift_period(r) == Approx(6.0)); CHECK(flag_period(r) == Approx(2.0));
    CharacterPhases p;
    advance_character_phases(p, r, 0.05);
    CHECK(p.roll == Approx(0.05 / 6.0)); CHECK(p.drift == Approx(0.05 / 6.0)); CHECK(p.flag == Approx(0.05 / 2.0));
    const CharacterPhases held = p;
    advance_character_phases(p, r, std::nan(""));                 // a broken clock moves nothing
    CHECK(p.roll == held.roll); CHECK(p.drift == held.drift); CHECK(p.flag == held.flag);
}
TEST_CASE("tube frame: tracking sleeps between slips", "[tube]") {
    TubeSetting s; auto off = resolve(s);                    // pvm: tracking 0
    REQUIRE_FALSE(tracking_moving(off, 0.1));
    set_row(s, Effect::tracking, 50); auto r = resolve(s);   // rate 15, recovery 43
    REQUIRE(tracking_period(r) == Approx(51.6));
    REQUIRE(tracking_duration(r) == Approx(0.55));
    REQUIRE(tracking_moving(r, 0.1));
    REQUIRE_FALSE(tracking_moving(r, 5.0));
}
TEST_CASE("tube frame: the colour patch starts top left", "[tube]") {
    TubeSetting s; set_row(s, Effect::purity, 100); set_row(s, Effect::purityPosition, 0);
    auto r = resolve(s); auto u = tube_uniforms_for(r, Picture{1440.0, 1080.0, false});
    tube_character_frame(u, r, CharacterPhases{}, 0.0, true);
    REQUIRE(u.purityX == Approx(0.5 - 0.5 * std::sqrt(0.5)));
    REQUIRE(u.purityY == Approx(0.5 - 0.5 * std::sqrt(0.5)));
}
TEST_CASE("tube frame: what moves on its own", "[tube]") {
    TubeSetting s; REQUIRE_FALSE(moves_on_its_own(resolve(s)));      // pvm is still
    set_row(s, Effect::hum, 1); REQUIRE(moves_on_its_own(resolve(s)));
    TubeSetting e; select_look(e, Look::everything); REQUIRE(moves_on_its_own(resolve(e)));
}

// ---- Ported from Sound Factory ----------------------------------------------

namespace {

const Picture kFrame1080{1440.0, 1080.0, false};

// Every row at `value`, as Sound Factory's everyRowAt.
Resolved every_row_at(int value) {
    Resolved r;
    r.look = Look::everything;
    r.effects.fill(value);
    return r;
}

Resolved with_row(Resolved r, Effect effect, int value) {
    r.effects[index_of(effect)] = value;
    return r;
}

}  // namespace

TEST_CASE("tube frame: the per-frame fields start neutral, and u{} stays zero", "[tube]") {
    const auto u = tube_uniforms_for(resolve(TubeSetting{}), kFrame1080);
    CHECK(u.warmupGain == 1.0f);
    CHECK(u.power == 1.0f);
    CHECK(u.jitterBias == -0.5f);
    const TubeUniforms zero{};
    CHECK(zero.warmupGain == 0.0f);
    CHECK(zero.power == 0.0f);
    CHECK(zero.jitterBias == 0.0f);
}

TEST_CASE("tube frame: warm-up holds dark, emerges monotonically and settles at exactly 1", "[tube]") {
    CHECK(warmup_gain(-1.0) == 0.0);
    CHECK(warmup_gain(0.2) == 0.0);
    CHECK(warmup_gain(0.5) == Approx(0.4));
    CHECK(warmup_gain(0.8) == Approx(0.8));
    CHECK(warmup_gain(kWarmupSeconds) == 1.0);
    CHECK(warmup_gain(100.0) == 1.0);
    for (int i = 1; i <= 150; ++i) CHECK(warmup_gain(i / 100.0) >= warmup_gain((i - 1) / 100.0));
}

TEST_CASE("tube frame: a picture owes the frames its afterglow and its limiter need, and no more", "[tube]") {
    const auto owed = [](int afterglow, int abl, double distance) {
        auto r = with_row(with_row(every_row_at(0), Effect::afterglow, afterglow), Effect::abl, abl);
        return settle_frames(tube_uniforms_for(r, kFrame1080), distance);
    };

    SECTION("the afterglow: the strongest channel keeps persistence x 0.75 a frame, down to 0.02 %") {
        CHECK(owed(0, 0, 0.0) == 0);
        CHECK(owed(10, 0, 0.0) == 4);
        CHECK(owed(30, 0, 0.0) == 6);
        CHECK(owed(100, 0, 0.0) == 30);

        // Never under: the frames it owes land inside the bound, and one fewer would not.
        for (int row = 1; row <= kRowMax; ++row) {
            INFO("AFTERGLOW " << row);
            const auto frames = owed(row, 0, 0.0);
            const auto kept = row / 100.0 * 0.75;
            CHECK(std::pow(kept, frames) <= kSettledWithin);
            CHECK(std::pow(kept, frames - 1) > kSettledWithin);
        }
    }

    SECTION("the limiter: only while it has distance to travel, and it lands inside the bound") {
        CHECK(owed(0, 30, 0.0) == 0);
        CHECK(owed(0, 30, 0.0001) == 0);
        CHECK(owed(0, 30, 1.0) == 26);
        CHECK(owed(0, 30, -1.0) == 26);
        CHECK(owed(0, 0, 1.0) == 0);  // with no limiter, nothing is travelling

        // Never under: the renderer's own step, at the rate settling frames render.
        const double step = std::exp(-(1.0 / kSettleFramesPerSecond) / kAblSeconds);
        for (const double distance : {0.001, 0.01, 0.1, 0.5, 1.0}) {
            INFO("distance " << distance);
            const auto frames = owed(0, 30, distance);
            double left = distance;
            for (int frame = 0; frame < frames; ++frame) left *= step;
            CHECK(left <= kSettledWithin);
            CHECK(left / step > kSettledWithin);
        }
    }

    SECTION("whichever needs longer") {
        CHECK(owed(100, 30, 1.0) == 30);
        CHECK(owed(10, 30, 1.0) == 26);
        CHECK(owed(10, 30, 0.0) == 4);
    }
}

TEST_CASE("tube frame: signal motion preserves controls, limits and cheap paths", "[tube]") {
    auto base = tube_uniforms_for(resolve(TubeSetting{}), kFrame1080);
    base.jitter = 1.0f;
    base.breathe = 1.0f;
    base.sharpness = 0.8f;
    auto off = base;
    tube_signal_motion(off, 4.0, 0.0, 1.0);
    CHECK(off.jitter == base.jitter);
    CHECK(off.sharpness == base.sharpness);
    CHECK(off.jitterBias == -0.5f);
    double movement = 0.0;
    for (int i = 0; i < 1000; ++i) {
        auto u = base;
        tube_signal_motion(u, i / 60.0, 1.0, 1.0);
        CHECK(u.jitter * (0.5 + std::abs(u.jitterBias + 0.5)) <= 0.500001);
        CHECK(u.sharpness >= base.sharpness * 0.96999f);
        CHECK(u.sharpness < base.sharpness);
        movement += std::abs(u.jitterBias + 0.5);
    }
    CHECK(movement > 1.0);
    for (float sharpness : {0.0f, 1.0f}) {
        auto u = base;
        u.sharpness = sharpness;
        u.jitter = u.breathe = 0.0f;
        tube_signal_motion(u, 3.0, 1.0, 1.0);
        CHECK(u.sharpness == sharpness);
        CHECK(u.jitter == 0.0f);
    }
    auto settled = base;
    tube_signal_motion(settled, 3.0, 1.0, 0.0);
    CHECK(settled.sharpness == base.sharpness);
    auto neutral = base;
    neutral.sharpness = 1.0f;
    tube_signal_motion(neutral, 3.0, 1.0, 1.0);
    CHECK(neutral.sharpness == 1.0f);
}

TEST_CASE("tube frame: optional character is bounded, deterministic, disabled and off by default", "[tube]") {
    auto r = resolve(TubeSetting{});
    auto u = tube_uniforms_for(r, kFrame1080);
    CharacterPhases p;
    for (int i = 0; i < 20; ++i) advance_character_phases(p, r, 0.1);   // two seconds in
    tube_character_frame(u, r, p, 0.08, true);
    CHECK(u.rollAmount == 0.0f); CHECK(u.trackingOffset == 0.0f); CHECK(u.driftOffset == 0.0f);
    r = with_row(with_row(with_row(r, Effect::rollBand, 100), Effect::tracking, 100), Effect::colorDrift, 100);
    tube_character_frame(u, r, p, 0.08, false);
    CHECK(u.rollAmount == 0.0f); CHECK(u.trackingOffset == 0.0f); CHECK(u.driftOffset == 0.0f);
    tube_character_frame(u, r, p, 0.08, true);
    CHECK(u.rollAmount == 2.0f); CHECK(u.trackingOffset > 0.0f); CHECK(u.driftOffset != 0.0f);
    const auto position = u.rollPosition;
    advance_character_phases(p, r, 0.1);
    tube_character_frame(u, r, p, 0.08, true);
    CHECK(u.rollPosition > position);  // screen y increases downward
    p = CharacterPhases{};
    for (int i = 0; i < 1000; ++i) {
        tube_character_frame(u, r, p, i * 0.01, true);
        advance_character_phases(p, r, 0.1);
        CHECK(std::abs(u.trackingOffset) <= 3.60001f * u.pxPerMm);
        CHECK(std::abs(u.driftOffset) <= 0.75001f * u.pxPerMm);
    }
    CHECK(tracking_moving(r, 0.58));  // clean final frame after the offset settles
    CHECK_FALSE(tracking_moving(r, 1.0));
    tube_character_frame(u, r, p, 1.0, true);
    CHECK(u.trackingOffset == 0.0f);
}

TEST_CASE("tube frame: worn character controls keep gentle lows and independent parameters", "[tube]") {
    for (double row = 0; row <= .3; row += .01) CHECK(detail::character_strength(row, 3) == Approx(row));
    CHECK(detail::character_strength(1, 2) == 2);
    CHECK(detail::character_strength(1, 3) == 3);
    auto r = resolve(TubeSetting{});
    auto u = tube_uniforms_for(r, kFrame1080);
    r = with_row(with_row(r, Effect::flagging, 100), Effect::purity, 100);
    CharacterPhases p;
    for (int i = 0; i < 20; ++i) advance_character_phases(p, r, 0.1);   // two seconds in
    tube_character_frame(u, r, p, .08, true);
    CHECK(u.flagOffset != 0); CHECK(u.flagExtent > 0); CHECK(u.purityAmount == 1);
    const auto x = u.purityX, y = u.purityY;
    r = with_row(r, Effect::purityPosition, 50);
    tube_character_frame(u, r, p, .08, true);
    CHECK(u.purityX != x); CHECK(u.purityY != y);
    r = with_row(r, Effect::trackingRecovery, 100);
    CHECK(tracking_duration(r) == Approx(1.12));
    r = with_row(r, Effect::tracking, 100);
    CHECK(tracking_moving(r, 1.14)); CHECK_FALSE(tracking_moving(r, 1.18));
    tube_character_frame(u, r, p, .08, false);
    CHECK(u.flagOffset == 0); CHECK(u.purityAmount == 0); CHECK(u.trackingOffset == 0);
}

// ---- The per-frame entry point: tube_frame() --------------------------------

namespace {

Resolved look_resolved(Look look) {
    TubeSetting s;
    select_look(s, look);
    return resolve(s);
}

// TubeUniforms is floats only, so two that hold the same values hold the same bytes.
bool same_uniforms(const TubeUniforms& a, const TubeUniforms& b) {
    static_assert(std::is_trivially_copyable_v<TubeUniforms>, "compared byte for byte");
    return std::memcmp(&a, &b, sizeof(TubeUniforms)) == 0;
}

// A frame in the middle of a warm-up with the light rising: every step of
// tube_frame() has something to change.
FrameState warming_frame() {
    FrameState state;
    state.seconds = 4.25;
    state.frame = 255;
    state.tracking_elapsed = 1.5;
    state.motion_amount = 1.0;
    state.rising_light = 1.0;
    state.ablLevel = 0.4f;
    state.power = 0.9f;
    state.degauss = 0.3f;
    state.spot = 0.2f;
    state.warmupGain = 0.5f;
    state.phases.roll = 0.3;
    state.phases.drift = 0.6;
    state.phases.flag = 0.15;
    return state;
}

}  // namespace

TEST_CASE("tube frame: tube_frame is the same frame however often it runs from the same still", "[tube]") {
    const auto r = look_resolved(Look::everything);
    const auto still = tube_uniforms_for(r, kFrame1080);
    REQUIRE(still.jitter > 0.0f);       // tube_signal_motion has something to take
    REQUIRE(still.breathe > 0.0f);      // ...and FOCUS has light to soften with
    REQUIRE(still.sharpness != 1.0f);   // ...through an active filter
    const TubeUniforms before = still;
    const FrameState state = warming_frame();
    const auto first = tube_frame(still, r, state);
    const auto second = tube_frame(still, r, state);
    CHECK(same_uniforms(first, second));
    CHECK(same_uniforms(still, before));   // the cached still is never touched
}

TEST_CASE("tube frame: tube_frame takes JITTER's share once, which is why it exists", "[tube]") {
    const auto r = look_resolved(Look::everything);
    const auto still = tube_uniforms_for(r, kFrame1080);
    FrameState state = warming_frame();
    state.warmupGain = 1.0f;
    auto once = still;
    tube_signal_motion(once, state.seconds, state.motion_amount, state.rising_light);
    auto twice = once;
    tube_signal_motion(twice, state.seconds, state.motion_amount, state.rising_light);
    const auto frame = tube_frame(still, r, state);
    CHECK(frame.jitter == once.jitter);
    CHECK(frame.jitter != twice.jitter);
    CHECK(frame.sharpness == once.sharpness);
    CHECK(frame.sharpness != twice.sharpness);
}

TEST_CASE("tube frame: tube_frame is Sound Factory's frame, step for step", "[tube]") {
    const auto r = with_row(with_row(look_resolved(Look::everything), Effect::rollBand, 60), Effect::flagging, 40);
    const auto still = tube_uniforms_for(r, kFrame1080);
    const FrameState state = warming_frame();
    // TubeRenderer.mm encodeTubeInto: the frame's clock, the renderer's own
    // fields, warm-up's softening, signal motion, then the character.
    auto want = still;
    tube_frame_phases(want, state.seconds, state.frame);
    want.ablLevel = state.ablLevel;
    want.power = state.power;
    want.warmupGain = state.warmupGain;
    want.sharpness *= 0.8f + 0.2f * state.warmupGain;
    want.degauss = state.degauss;
    tube_signal_motion(want, state.seconds, state.motion_amount, state.rising_light);
    tube_character_frame(want, r, state.phases, state.tracking_elapsed, true);
    want.spot = state.spot;
    CHECK(same_uniforms(tube_frame(still, r, state), want));
}

TEST_CASE("tube frame: tube_frame draws the character at the renderer's phases, not the clock's", "[tube]") {
    const auto r = with_row(with_row(with_row(look_resolved(Look::everything), Effect::rollBand, 60),
                                     Effect::colorDrift, 60), Effect::flagging, 40);
    const auto still = tube_uniforms_for(r, kFrame1080);
    FrameState state = warming_frame();
    const auto at = tube_frame(still, r, state);
    CHECK(at.rollPosition == Approx(state.phases.roll));
    FrameState later = state;
    later.seconds += 3.0;                     // the clock alone moves no band, drift or bend
    const auto clock_only = tube_frame(still, r, later);
    CHECK(clock_only.rollPosition == at.rollPosition);
    CHECK(clock_only.driftOffset == at.driftOffset);
    CHECK(clock_only.flagOffset == at.flagOffset);
    later.phases.roll = 0.7;
    CHECK(tube_frame(still, r, later).rollPosition == Approx(0.7));
}

TEST_CASE("tube frame: tube_frame writes what only the renderer knows", "[tube]") {
    const auto r = look_resolved(Look::everything);
    const auto still = tube_uniforms_for(r, kFrame1080);
    FrameState state = warming_frame();
    state.rising_light = 0.0;   // FOCUS only softens with rising light: warm-up's softening alone
    const auto u = tube_frame(still, r, state);
    CHECK(u.ablLevel == state.ablLevel);
    CHECK(u.power == state.power);
    CHECK(u.degauss == state.degauss);
    CHECK(u.spot == state.spot);
    CHECK(u.warmupGain == state.warmupGain);
    CHECK(u.sharpness == still.sharpness * (0.8f + 0.2f * state.warmupGain));
    FrameState lit = state;
    lit.warmupGain = 1.0f;
    CHECK(tube_frame(still, r, lit).sharpness == still.sharpness);   // warmed up: no softening
}

TEST_CASE("tube frame: tube_frame's switches hold motion and character back", "[tube]") {
    const auto r = with_row(look_resolved(Look::everything), Effect::rollBand, 60);
    const auto still = tube_uniforms_for(r, kFrame1080);
    FrameState state = warming_frame();
    state.motion_enabled = false;
    state.character_enabled = false;
    const auto u = tube_frame(still, r, state);
    CHECK(u.jitter == still.jitter);
    CHECK(u.jitterBias == -0.5f);
    CHECK(u.rollAmount == 0.0f);
    state.character_enabled = true;
    CHECK(tube_frame(still, r, state).rollAmount > 0.0f);
}

// ---- The long uptime: every phase stays continuous across its wrap ---------

TEST_CASE("tube frame: hum's bar rolls on smoothly across its wrap", "[tube]") {
    const double dt = 1.0 / 60.0, t0 = kHumPeriodSeconds * 1000 - 0.004;
    TubeUniforms a{}, b{};
    tube_frame_phases(a, t0, 0);
    tube_frame_phases(b, t0 + dt, 1);
    REQUIRE(b.humPhase < a.humPhase);   // the phase did wrap between the two frames
    for (const TubeUniforms& u : {a, b}) {
        CHECK(u.humPhase >= 0.0f);
        CHECK(u.humPhase < 1.0f);
    }
    // What the shader draws, sin(2 pi phase), moves no further than one frame's advance.
    const double step = std::abs(std::sin(detail::kTwoPi * b.humPhase) - std::sin(detail::kTwoPi * a.humPhase));
    CHECK(step <= detail::kTwoPi * dt / kHumPeriodSeconds + 1e-5);
}

TEST_CASE("tube frame: degauss's two phases are 31 t and 27 t, after a month too", "[tube]") {
    for (const double t : {0.37, 30.0 * 86400.0 + 0.37}) {
        INFO(t);
        TubeUniforms u{};
        tube_frame_phases(u, t, 0);
        CHECK(std::sin(u.degaussPhaseA) == Approx(std::sin(std::fmod(31.0 * t, detail::kTwoPi))).margin(1e-4));
        CHECK(std::sin(u.degaussPhaseB) == Approx(std::sin(std::fmod(27.0 * t, detail::kTwoPi))).margin(1e-4));
    }
}

TEST_CASE("tube frame: flicker's wave is the same sum after a month", "[tube]") {
    for (const double t : {0.37, 30.0 * 86400.0}) {
        INFO(t);
        TubeUniforms u{};
        tube_frame_phases(u, t, 0);
        CHECK(u.flickerWave == Approx(0.6 * std::sin(37.7 * t) + 0.4 * std::sin(61.3 * t + 1.7)).margin(1e-4));
    }
}

TEST_CASE("tube frame: the noise seed is fract(frame * 0.618) * 913", "[tube]") {
    const std::uint64_t f = 12345;
    TubeUniforms u{};
    tube_frame_phases(u, 0.0, f);
    const double x = static_cast<double>(f) * 0.618;
    CHECK(u.noiseSeed == Approx((x - std::floor(x)) * 913.0).margin(1e-4));
}

TEST_CASE("tube frame: the character keeps moving, one frame at a time, across its wrap", "[tube]") {
    auto r = resolve(TubeSetting{});
    r = with_row(with_row(with_row(r, Effect::rollBand, 70), Effect::colorDrift, 70), Effect::flagging, 70);
    const double dt = 1.0 / 60.0;
    // A month of frames ends just short of every phase's wrap: the frame after crosses it.
    CharacterPhases pa;
    pa.roll = pa.drift = pa.flag = 1.0 - 0.25 * dt / roll_period(r);
    CharacterPhases pb = pa;
    advance_character_phases(pb, r, dt);
    REQUIRE(pb.roll < pa.roll);   // the roll did wrap between the two frames
    auto a = tube_uniforms_for(r, kFrame1080), b = a;
    tube_character_frame(a, r, pa, 0.0, true);
    tube_character_frame(b, r, pb, dt, true);

    double advance = static_cast<double>(b.rollPosition) - a.rollPosition;
    if (advance < 0.0) advance += 1.0;
    CHECK(advance == Approx(dt / roll_period(r)).margin(1e-6));

    // Loose: no faster than the shortest period each row can reach, at its largest swing.
    const double px_per_mm = a.pxPerMm;
    const double drift_most = 0.25 * px_per_mm * 3.0 * detail::kTwoPi / 6.0 * dt;
    const double flag_most = 4.0 * px_per_mm * (0.8 + 0.2 * 2.0) * detail::kTwoPi / 2.0 * dt;
    for (const TubeUniforms& u : {a, b}) {
        CHECK(std::isfinite(u.driftOffset));
        CHECK(std::isfinite(u.flagOffset));
    }
    CHECK(b.driftOffset != a.driftOffset);   // not frozen
    CHECK(b.flagOffset != a.flagOffset);
    CHECK(std::abs(b.driftOffset - a.driftOffset) <= drift_most);   // not jumped
    CHECK(std::abs(b.flagOffset - a.flagOffset) <= flag_most);
}
