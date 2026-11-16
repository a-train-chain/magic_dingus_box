// A tube setting turned into what the shader reads (tube/tube_inputs.h).
//
// The first block is the plan's own cases. The second is ported from Sound
// Factory's engine/tests/test_tube_inputs.cpp at commit 4ef74a6 (its
// test_crt_looks.cpp / test_crt_wire.cpp have no case that calls the ported
// functions): the cases that still apply once the picture is the source's own
// size, the whole-pixel sizes count at the 1080-line reference, the opening has
// straight edges and there is no gate or bypass. Their worked numbers are
// recomputed for a 1440 x 1080 picture where the size changed them.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>
#include "tube/tube_inputs.h"
using namespace tube;
using Catch::Approx;

const Picture k1080{1440.0, 1080.0, false};
TEST_CASE("tube inputs: pvm at 1080p", "[tube]") {
    TubeSetting s; auto u = tube_uniforms_for(resolve(s), k1080);
    REQUIRE(u.pxPerMm     == Approx(1440.0 / 196.608));
    REQUIRE(u.brightness  == Approx(1.05));
    REQUIRE(u.contrast    == Approx(1.08));
    REQUIRE(u.saturation  == Approx(1.16));
    REQUIRE(u.sharpness   == Approx(1.26));
    REQUIRE(u.scanlines   == Approx(0.35));
    REQUIRE(u.beamMin     == Approx(0.16));
    REQUIRE(u.beamMax     == Approx(0.40));
    REQUIRE(u.raster      == 0.0f);
    REQUIRE(u.maskType    == 1.0f);
    REQUIRE(u.maskStrength== Approx(0.48));
    REQUIRE(u.maskGain    == Approx(3.0 / 2.04));
    REQUIRE(u.maskPitch   == 3.0f);
    REQUIRE(u.lines       == Approx(270.0));
    REQUIRE(u.glow        == Approx(0.08));
    REQUIRE(u.persistence == Approx(0.10));
    REQUIRE(u.curvature   == Approx(0.70));
    REQUIRE(u.abl         == Approx(0.20));
    REQUIRE(u.breathe     == Approx(0.15));
    REQUIRE(u.hum == 0.0f); REQUIRE(u.noise == 0.0f); REQUIRE(u.jitter == 0.0f); REQUIRE(u.flicker == 0.0f);
    REQUIRE(u.tint[0] == 1.0f); REQUIRE(u.tint[1] == 1.0f); REQUIRE(u.tint[2] == 1.0f);
}
TEST_CASE("tube inputs: 720p keeps whole pixels", "[tube]") {
    TubeSetting s; auto u = tube_uniforms_for(resolve(s), Picture{960.0, 720.0, false});
    REQUIRE(u.lines == Approx(240.0));
    REQUIRE(u.maskPitch == 2.0f);
}
TEST_CASE("tube inputs: neutral rows give exactly neutral inputs", "[tube]") {
    TubeSetting s;
    for (auto e : {Effect::brightness, Effect::contrast, Effect::saturation, Effect::white, Effect::sharpness}) set_row(s, e, 50);
    auto u = tube_uniforms_for(resolve(s), k1080);
    REQUIRE(u.brightness == 1.0f); REQUIRE(u.contrast == 1.0f); REQUIRE(u.saturation == 1.0f); REQUIRE(u.sharpness == 1.0f);
    REQUIRE(u.tint[0] == 1.0f); REQUIRE(u.tint[2] == 1.0f);
}
TEST_CASE("tube inputs: a moving row's 100 is what the shader's cap expects", "[tube]") {
    TubeSetting s;
    for (auto e : {Effect::noise, Effect::jitter, Effect::flicker, Effect::hum}) set_row(s, e, 100);
    auto u = tube_uniforms_for(resolve(s), k1080);
    REQUIRE(u.noise == Approx(0.2)); REQUIRE(u.jitter == Approx(1.0));
    REQUIRE(u.flicker == Approx(1.0)); REQUIRE(u.hum == Approx(1.0));
}
TEST_CASE("tube inputs: hum's curve", "[tube]") {
    REQUIRE(hum_strength(0) == 0.0f);
    REQUIRE(hum_strength(30) == Approx(0.06));
    REQUIRE(hum_strength(65) == Approx(0.53));
    REQUIRE(hum_strength(100) == Approx(1.0));
}
TEST_CASE("tube inputs: the mask gives back what it takes", "[tube]") {
    REQUIRE(mask_gain(Mask::none, 0.8f) == 1.0f);
    REQUIRE(mask_gain(Mask::grille, 0.48f) == Approx(1.4705882));
    REQUIRE(mask_gain(Mask::magentaGreen, 0.304f) == Approx(2.0 / 1.696));
    REQUIRE(mask_gain(Mask::dots, 0.0f) == 1.0f);
    REQUIRE(mask_gain(Mask::slot, 0.0f) == 1.0f);
}
TEST_CASE("tube inputs: colour temp and millimetre sizes", "[tube]") {
    TubeSetting s; set_row(s, Effect::white, 0); set_row(s, Effect::chromaBleed, 50); set_row(s, Effect::convergence, 17);
    auto u = tube_uniforms_for(resolve(s), k1080);
    REQUIRE(u.tint[0] == Approx(1.0)); REQUIRE(u.tint[1] == Approx(0.95)); REQUIRE(u.tint[2] == Approx(0.86));
    REQUIRE(u.chromaBleed == Approx(1.5 * 1440.0 / 196.608));
    REQUIRE(u.convergence == Approx(0.102 * 1440.0 / 196.608));
}
TEST_CASE("tube inputs: a real CRT keeps its own scanlines, mask and glass", "[tube]") {
    TubeSetting s; select_look(s, Look::tv);
    auto u = tube_uniforms_for(resolve(s), Picture{960.0, 720.0, true});
    REQUIRE(u.scanlines == 0.0f); REQUIRE(u.raster == 0.0f);
    REQUIRE(u.maskType == 0.0f);  REQUIRE(u.maskGain == 1.0f);
    REQUIRE(u.curvature == 0.0f); REQUIRE(u.rim == 0.0f);
    REQUIRE(u.glassBlack == 0.0f); REQUIRE(u.reflection == 0.0f);
    REQUIRE(u.chromaBleed > 0.0f);                    // signal rows still work
}
TEST_CASE("tube inputs: the flat picture fits inside its margin", "[tube]") {
    TubeSetting s; auto u = tube_uniforms_for(resolve(s), k1080);
    REQUIRE(u.flatFit > 0.9f); REQUIRE(u.flatFit < 1.0f);
}

// ---- Ported from Sound Factory engine/tests/test_tube_inputs.cpp @ 4ef74a6 ----

namespace {

Resolved look_resolved(Look look) {
    TubeSetting s;
    select_look(s, look);
    return resolve(s);
}

int& row_of(Resolved& r, Effect e) { return r.effects[index_of(e)]; }

// A value no other row shares, so an input wired to the wrong row reads wrong.
Resolved every_row_distinct() {
    Resolved r = look_resolved(Look::tv);
    for (std::size_t i = 0; i < kEffectCount; ++i) r.effects[i] = 10 + static_cast<int>(i) * 3;
    return r;
}

Resolved every_row_at(int value) {
    Resolved r = look_resolved(Look::everything);
    r.mask = Mask::grille;
    r.effects.fill(value);
    return r;
}

const double kPxPerMm1080 = 1440.0 / 196.608;

}  // namespace

TEST_CASE("tube inputs: the bezel's corner and margin are millimetres, so they scale with the picture", "[tube]") {
    const auto r = look_resolved(Look::pvm);
    const auto big = tube_uniforms_for(r, k1080);
    CHECK(big.cornerPx == Approx(5.0 * kPxPerMm1080));
    CHECK(big.marginPx == Approx(1.5 * kPxPerMm1080));

    const auto small = tube_uniforms_for(r, Picture{960.0, 720.0, false});
    CHECK(small.cornerPx == Approx(5.0 * 960.0 / 196.608));
    CHECK(small.marginPx == Approx(1.5 * 960.0 / 196.608));
    CHECK(small.outW == 960.0f);
    CHECK(small.outH == 720.0f);
}

TEST_CASE("tube inputs: each row reaches its own input, by its own curve", "[tube]") {
    auto settings = std::vector<Resolved>{every_row_distinct()};
    for (const Look look : kLooks) settings.push_back(look_resolved(look));

    for (const auto& r : settings) {
        INFO(look_name(r.look));
        const auto row = [&r](Effect e) { return static_cast<double>(r.effects[index_of(e)]); };
        const auto u = tube_uniforms_for(r, k1080);

        CHECK(u.brightness == Approx(0.5 + row(Effect::brightness) / 100));
        CHECK(u.contrast == Approx(1 + (row(Effect::contrast) - 50) / 100 * 0.8));
        CHECK(u.saturation == Approx(row(Effect::saturation) / 50));
        CHECK(u.sharpness == Approx(row(Effect::sharpness) / 50));

        CHECK(u.scanlines == Approx(row(Effect::scanlines) / 100));
        CHECK(u.raster == Approx(row(Effect::raster) / 100));
        CHECK(u.beamMin == Approx(std::max(0.12, 0.32 - 0.2 * row(Effect::beam) / 100)));
        CHECK(u.beamMax == Approx(0.32 + 0.1 * row(Effect::beam) / 100));

        CHECK(u.maskStrength == Approx(row(Effect::maskStrength) * 0.008));
        CHECK(u.maskPitch == static_cast<float>(whole_px(mask_pitch_row_px(r.effects[index_of(Effect::maskSize)]), 1080.0)));
        CHECK(u.lines == Approx(1080.0 / whole_px(line_pitch_row_px(r.effects[index_of(Effect::lineSize)]), 1080.0)));

        CHECK(u.glow == Approx(row(Effect::glow) / 100));
        CHECK(u.persistence == Approx(row(Effect::afterglow) / 100));

        CHECK(u.curvature == Approx(row(Effect::curvature) / 100));
        CHECK(u.vignette == Approx(row(Effect::vignette) / 100));
        CHECK(u.rim == Approx(row(Effect::rim) / 100));
        CHECK(u.glassBlack == Approx(row(Effect::glassBlack) / 100));
        CHECK(u.reflection == Approx(row(Effect::reflection) / 100));

        CHECK(u.convergence == Approx(row(Effect::convergence) / 100 * 0.60 * kPxPerMm1080));
        CHECK(u.chromaBleed == Approx(row(Effect::chromaBleed) / 100 * 3.0 * kPxPerMm1080));
        CHECK(u.noise == Approx(row(Effect::noise) * 0.002));
        CHECK(u.jitter == Approx(row(Effect::jitter) / 100));

        CHECK(u.flicker == Approx(row(Effect::flicker) / 100));
        CHECK(u.hum == Approx(hum_strength(row(Effect::hum))));
        CHECK(u.abl == Approx(row(Effect::abl) / 100));
        CHECK(u.breathe == Approx(row(Effect::breathing) * 0.01));
    }
}

TEST_CASE("tube inputs: the picture's rows are neutral at 50 and reach their own ends", "[tube]") {
    const auto at = [](Effect e, int value) {
        auto r = look_resolved(Look::pvm);
        row_of(r, e) = value;
        return tube_uniforms_for(r, k1080);
    };

    CHECK(at(Effect::brightness, 0).brightness == Approx(0.5));
    CHECK(at(Effect::brightness, 50).brightness == Approx(1.0));
    CHECK(at(Effect::brightness, 100).brightness == Approx(1.5));

    CHECK(at(Effect::contrast, 0).contrast == Approx(0.6));
    CHECK(at(Effect::contrast, 50).contrast == Approx(1.0));
    CHECK(at(Effect::contrast, 100).contrast == Approx(1.4));

    CHECK(at(Effect::saturation, 0).saturation == 0.0f);  // mono
    CHECK(at(Effect::saturation, 50).saturation == Approx(1.0));
    CHECK(at(Effect::saturation, 100).saturation == Approx(2.0));

    CHECK(at(Effect::sharpness, 0).sharpness == 0.0f);
    CHECK(at(Effect::sharpness, 50).sharpness == Approx(1.0));
    CHECK(at(Effect::sharpness, 100).sharpness == Approx(2.0));

    // COLOUR TEMP, the spike's two tints either side of white.
    const auto tint_is = [&at](int value, double red, double green, double blue) {
        const auto u = at(Effect::white, value);
        CHECK(u.tint[0] == Approx(red));
        CHECK(u.tint[1] == Approx(green));
        CHECK(u.tint[2] == Approx(blue));
    };
    tint_is(0, 1.0, 0.95, 0.86);
    tint_is(25, 1.0, 0.975, 0.93);
    tint_is(50, 1.0, 1.0, 1.0);
    tint_is(75, 0.95, 0.985, 1.04);
    tint_is(100, 0.90, 0.97, 1.08);
}

TEST_CASE("tube inputs: the two whole-pixel sizes follow the picture and cannot reach nothing", "[tube]") {
    const auto pitch_of = [](int row, double h) {
        auto r = look_resolved(Look::pvm);
        row_of(r, Effect::maskSize) = row;
        return tube_uniforms_for(r, Picture{h * 4.0 / 3.0, h, false}).maskPitch;
    };
    const auto lines_of = [](int row, double h) {
        auto r = look_resolved(Look::pvm);
        row_of(r, Effect::lineSize) = row;
        return tube_uniforms_for(r, Picture{h * 4.0 / 3.0, h, false}).lines;
    };

    // MASK SIZE: pixels at the 1080-line reference, scaled and rounded, never under two.
    CHECK(pitch_of(0, 1080) == 2.0f);
    CHECK(pitch_of(30, 1080) == 5.0f);
    CHECK(pitch_of(100, 1080) == 12.0f);
    CHECK(pitch_of(100, 720) == 8.0f);
    CHECK(pitch_of(0, 720) == 2.0f);

    // A whole number of pixels, at every size, because a fractional pitch beats against the grid into moire.
    for (const double h : {480.0, 576.0, 720.0, 1080.0, 1440.0, 2160.0})
        for (int row = 0; row <= 100; row += 5) {
            const auto pitch = pitch_of(row, h);
            CHECK(pitch == std::round(pitch));
            CHECK(pitch >= 2.0f);
        }

    // SCANLINE SIZE: lines over the picture's height, so the pitch is what the row says.
    CHECK(lines_of(0, 1080) == Approx(360.0));          // 3 px
    CHECK(lines_of(100, 1080) == Approx(1080.0 / 13));
    CHECK(lines_of(70, 1080) == Approx(108.0));         // 10 px
    CHECK(lines_of(0, 720) == Approx(360.0));           // 2 px, also the least

    // A ROW OUT OF RANGE IS THE RANGE'S END, for the three that are sizes.
    CHECK(pitch_of(500, 1080) == 12.0f);
    CHECK(pitch_of(-30, 1080) == 2.0f);
    CHECK(lines_of(500, 1080) == Approx(1080.0 / 13));
    CHECK(lines_of(-30, 1080) == Approx(360.0));
}

TEST_CASE("tube inputs: a size in millimetres follows the picture", "[tube]") {
    auto r = every_row_at(100);

    CHECK(tube_uniforms_for(r, k1080).pxPerMm == Approx(1440.0 / 196.608));
    CHECK(tube_uniforms_for(r, Picture{960.0, 720.0, false}).pxPerMm == Approx(960.0 / 196.608));

    const auto u = tube_uniforms_for(r, k1080);
    CHECK(u.convergence == Approx(0.60 * kPxPerMm1080));
    CHECK(u.chromaBleed == Approx(3.0 * kPxPerMm1080));

    // GLOW SIZE IS THE BLUR'S SIGMA, in the quarter-resolution picture's px: 0.5 mm .. 6 mm.
    const auto sigma_at = [&r](int row, double w) {
        row_of(r, Effect::glowSize) = row;
        return glow_sigma_px(r, Picture{w, w * 0.75, false});
    };
    CHECK(sigma_at(0, 1440) == Approx(0.5 * 1440 / 196.608 / 4));
    CHECK(sigma_at(20, 1440) == Approx(1.6 * 1440 / 196.608 / 4));
    CHECK(sigma_at(100, 1440) == Approx(6.0 * 1440 / 196.608 / 4));
    CHECK(sigma_at(100, 960) == Approx(6.0 * 960 / 196.608 / 4));

    // Out of range is the range's end here too.
    CHECK(sigma_at(500, 1440) == Approx(6.0 * 1440 / 196.608 / 4));
    CHECK(sigma_at(-30, 1440) == Approx(0.5 * 1440 / 196.608 / 4));
}

TEST_CASE("tube inputs: what a look used to hide is rows now", "[tube]") {
    const auto beam_of = [](int swell) {
        auto r = look_resolved(Look::pvm);
        row_of(r, Effect::beam) = swell;
        return tube_uniforms_for(r, k1080);
    };

    for (const Look look : kLooks) {
        INFO(look_name(look));
        const auto r = look_resolved(look);
        const auto u = tube_uniforms_for(r, k1080);

        CHECK(u.raster == Approx(r.effects[index_of(Effect::raster)] / 100.0));
        CHECK(u.vignette == Approx(r.effects[index_of(Effect::vignette)] / 100.0));
        CHECK(u.rim == Approx(r.effects[index_of(Effect::rim)] / 100.0));

        // The beam the right way up, and never nothing.
        CHECK(u.beamMin > 0.0f);
        CHECK(u.beamMin <= u.beamMax);
    }

    CHECK(beam_of(0).beamMin == Approx(0.32));
    CHECK(beam_of(0).beamMax == Approx(0.32));
    CHECK(beam_of(50).beamMin == Approx(0.22));
    CHECK(beam_of(50).beamMax == Approx(0.37));
    CHECK(beam_of(80).beamMin == Approx(0.16));
    CHECK(beam_of(80).beamMax == Approx(0.40));
    CHECK(beam_of(100).beamMin == Approx(0.12));
    CHECK(beam_of(100).beamMax == Approx(0.42));
    CHECK(beam_of(500).beamMin == Approx(0.12));  // the floor, for a row the doors would never pass

    // Another row moved leaves the raster and beam; the look's word reaches nothing.
    auto moved = look_resolved(Look::arcade);
    const auto arcade = tube_uniforms_for(moved, k1080);
    row_of(moved, Effect::scanlines) = 1;
    CHECK(tube_uniforms_for(moved, k1080).raster == Approx(arcade.raster));
    CHECK(tube_uniforms_for(moved, k1080).beamMin == Approx(arcade.beamMin));
    moved.look = Look::pvm;
    CHECK(tube_uniforms_for(moved, k1080).raster == Approx(arcade.raster));
}

TEST_CASE("tube inputs: every mask draws its own pattern and gets its own light back", "[tube]") {
    const auto with = [](Mask mask) {
        auto r = look_resolved(Look::pvm);
        r.mask = mask;
        row_of(r, Effect::maskStrength) = 56;  // 0.448
        return tube_uniforms_for(r, k1080);
    };

    // The spike's maskGainFor at MASK STRENGTH 56 (0.448), worked by hand.
    CHECK(with(Mask::none).maskType == 0.0f);
    CHECK(with(Mask::none).maskGain == 1.0f);
    CHECK(with(Mask::grille).maskType == 1.0f);
    CHECK(with(Mask::grille).maskGain == Approx(1.42586).margin(0.0001));
    CHECK(with(Mask::magentaGreen).maskType == 2.0f);
    CHECK(with(Mask::magentaGreen).maskGain == Approx(1.28866).margin(0.0001));
    CHECK(with(Mask::dots).maskType == 3.0f);
    CHECK(with(Mask::dots).maskGain == Approx(1.47543).margin(0.0001));
    CHECK(with(Mask::slot).maskType == 4.0f);
    CHECK(with(Mask::slot).maskGain == Approx(1.43658).margin(0.0001));

    // A mask with no strength takes no light, so there is none to give back.
    auto no_strength = look_resolved(Look::pvm);
    row_of(no_strength, Effect::maskStrength) = 0;
    CHECK(tube_uniforms_for(no_strength, k1080).maskGain == 1.0f);
}

TEST_CASE("tube inputs: every row at 100 hands the shader the flash-safe ceiling", "[tube]") {
    const auto top = tube_uniforms_for(every_row_at(100), k1080);

    CHECK(top.flicker == Approx(1.0));
    CHECK(top.hum == Approx(1.0));
    CHECK(top.noise == Approx(0.2));
    CHECK(top.jitter == Approx(1.0));

    CHECK(top.curvature == Approx(1.0));
    CHECK(top.maskStrength == Approx(0.8));
    CHECK(top.convergence == Approx(0.60 * kPxPerMm1080));
    CHECK(top.glassBlack == Approx(1.0));
    CHECK(top.reflection == Approx(1.0));
    CHECK(top.abl == Approx(1.0));
    CHECK(top.breathe == Approx(1.0));

    const auto bottom = tube_uniforms_for(every_row_at(0), k1080);
    CHECK(bottom.flicker == 0.0f);
    CHECK(bottom.hum == 0.0f);
    CHECK(bottom.noise == 0.0f);
    CHECK(bottom.jitter == 0.0f);
}

TEST_CASE("tube inputs: the flat picture's one scale leaves the design margin at its corner", "[tube]") {
    for (const Picture& p : {k1080, Picture{960.0, 720.0, false}}) {
        INFO(p.h);
        const double px_per_mm = p.w / kTubeMmAcross;
        const double fit = detail::flat_fit_for(p);
        CHECK(detail::opening_distance_px(p, fit * p.w / 2, fit * p.h / 2) ==
              Approx(-kTubeDesignMarginMm * px_per_mm).margin(1e-6));
        CHECK(tube_uniforms_for(look_resolved(Look::pvm), p).flatFit == Approx(fit));
    }
    // The same millimetres at any size: the fit is a fraction of the picture, so it does not move.
    CHECK(detail::flat_fit_for(k1080) == Approx(detail::flat_fit_for(Picture{960.0, 720.0, false})));
}

TEST_CASE("tube inputs: the flat fit agrees with a closed form worked without the header's bisection", "[tube]") {
    // Straight edges: the scaled picture's corner sits inside the corner fillet
    // (radius r), at distance r - hypot(r - a W, r - a H) inward, a = (1 - fit) / 2.
    // That is the margin m when (r - a W)^2 + (r - a H)^2 = (r - m)^2, a quadratic in a:
    //   (W^2 + H^2) a^2 - 2 r (W + H) a + 2 r^2 - (r - m)^2 = 0.
    // The smaller root is the one with the corner inside the fillet (both legs positive).
    const double w = 1440.0, h = 1080.0, px_per_mm = w / 196.608;
    const double r = 5.0 * px_per_mm, m = 1.5 * px_per_mm;
    const double qa = w * w + h * h, qb = -2.0 * r * (w + h), qc = 2.0 * r * r - (r - m) * (r - m);
    const double a = (-qb - std::sqrt(qb * qb - 4.0 * qa * qc)) / (2.0 * qa);
    REQUIRE(r - a * w > 0.0);
    REQUIRE(r - a * h > 0.0);
    const double fit = 1.0 - 2.0 * a;   // 0.97033 at 1440 x 1080
    CHECK(fit == Approx(0.97033).margin(1e-5));
    CHECK(tube_uniforms_for(resolve(TubeSetting{}), k1080).flatFit == Approx(fit).margin(1e-4));
}

TEST_CASE("tube inputs: hum retains subtle lows and a visible bounded upper range", "[tube]") {
    CHECK(hum_strength(0) == 0.0f);
    CHECK(hum_strength(10) * 0.25 == Approx(0.005));
    CHECK(hum_strength(30) * 0.25 == Approx(0.015));
    CHECK(hum_strength(65) * 0.25 == Approx(0.1325));
    CHECK(hum_strength(100) == Approx(1.0));
    for (int row = 1; row <= 100; ++row) CHECK(hum_strength(row) > hum_strength(row - 1));
}
