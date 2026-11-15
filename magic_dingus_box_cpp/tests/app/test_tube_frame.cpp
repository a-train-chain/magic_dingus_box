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
    tube_character_frame(u, r, 10.8, 0.0, true);
    REQUIRE(u.rollAmount == Approx(2.0));
    REQUIRE(u.rollPosition == Approx(0.5));                  // 10.8 s of a 21.6 s pass
    REQUIRE(u.rollHalfWidth == Approx(13.1 * (1440.0 / 196.608) / 1080.0));
    REQUIRE(u.rollTint == Approx(0.0));
    tube_character_frame(u, r, 10.8, 0.0, false);
    REQUIRE(u.rollAmount == 0.0f);                           // disabled zeroes everything
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
    tube_character_frame(u, r, 0.0, 0.0, true);
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
    tube_character_frame(u, r, 2.0, 0.08, true);
    CHECK(u.rollAmount == 0.0f); CHECK(u.trackingOffset == 0.0f); CHECK(u.driftOffset == 0.0f);
    r = with_row(with_row(with_row(r, Effect::rollBand, 100), Effect::tracking, 100), Effect::colorDrift, 100);
    tube_character_frame(u, r, 2.0, 0.08, false);
    CHECK(u.rollAmount == 0.0f); CHECK(u.trackingOffset == 0.0f); CHECK(u.driftOffset == 0.0f);
    tube_character_frame(u, r, 2.0, 0.08, true);
    CHECK(u.rollAmount == 2.0f); CHECK(u.trackingOffset > 0.0f); CHECK(u.driftOffset != 0.0f);
    const auto position = u.rollPosition;
    tube_character_frame(u, r, 2.1, 0.08, true);
    CHECK(u.rollPosition > position);  // screen y increases downward
    for (int i = 0; i < 1000; ++i) {
        tube_character_frame(u, r, i * 0.1, i * 0.01, true);
        CHECK(std::abs(u.trackingOffset) <= 3.60001f * u.pxPerMm);
        CHECK(std::abs(u.driftOffset) <= 0.75001f * u.pxPerMm);
    }
    CHECK(tracking_moving(r, 0.58));  // clean final frame after the offset settles
    CHECK_FALSE(tracking_moving(r, 1.0));
    tube_character_frame(u, r, 3.0, 1.0, true);
    CHECK(u.trackingOffset == 0.0f);
}

TEST_CASE("tube frame: worn character controls keep gentle lows and independent parameters", "[tube]") {
    for (double row = 0; row <= .3; row += .01) CHECK(detail::character_strength(row, 3) == Approx(row));
    CHECK(detail::character_strength(1, 2) == 2);
    CHECK(detail::character_strength(1, 3) == 3);
    auto r = resolve(TubeSetting{});
    auto u = tube_uniforms_for(r, kFrame1080);
    r = with_row(with_row(r, Effect::flagging, 100), Effect::purity, 100);
    tube_character_frame(u, r, 2, .08, true);
    CHECK(u.flagOffset != 0); CHECK(u.flagExtent > 0); CHECK(u.purityAmount == 1);
    const auto x = u.purityX, y = u.purityY;
    r = with_row(r, Effect::purityPosition, 50);
    tube_character_frame(u, r, 2, .08, true);
    CHECK(u.purityX != x); CHECK(u.purityY != y);
    r = with_row(r, Effect::trackingRecovery, 100);
    CHECK(tracking_duration(r) == Approx(1.12));
    r = with_row(r, Effect::tracking, 100);
    CHECK(tracking_moving(r, 1.14)); CHECK_FALSE(tracking_moving(r, 1.18));
    tube_character_frame(u, r, 2, .08, false);
    CHECK(u.flagOffset == 0); CHECK(u.purityAmount == 0); CHECK(u.trackingOffset == 0);
}
