// The values the Tube shaders used to compute per pixel from uniforms alone, now
// computed once a frame (tube/tube_derived.h). Each is pinned to the GLSL expression
// it replaces, written out here as the shader had it (the string is that text), and
// every one is finite for every reachable setting.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "tube/tube_derived.h"
#include "tube/tube_programs.h"
#include "tube/tube_setting.h"

using namespace tube;
using Catch::Approx;

namespace {

// GLSL's built-ins as the shader had them, in float.
float smoothstep_(float e0, float e1, float x) {
    const float t = std::min(std::max((x - e0) / (e1 - e0), 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
float mix_(float x, float y, float a) { return x * (1.0f - a) + y * a; }

struct Pin {
    const char* glsl;                                   // the replaced expression, as the shader had it
    std::function<float(const TubeUniforms&)> shader;   // it, evaluated
    float TubeDerived::*field;
};

// Every field that is a function of the uniforms alone (the sizes are below).
std::vector<Pin> pins() {
    using U = const TubeUniforms&;
    return {
        {"1.0 / outW (sp / outSize, uv)", [](U u) { return 1.0f / u.outW; }, &TubeDerived::dInvW},
        {"1.0 / outH (p.y / outH)", [](U u) { return 1.0f / u.outH; }, &TubeDerived::dInvH},
        {"p / (outSize * 0.5): x", [](U u) { return 1.0f / (u.outW * 0.5f); }, &TubeDerived::dInvHalfW},
        {"p / (outSize * 0.5): y", [](U u) { return 1.0f / (u.outH * 0.5f); }, &TubeDerived::dInvHalfH},
        {"vec2(outW / outH, 1.0)", [](U u) { return u.outW / u.outH; }, &TubeDerived::dAspect},
        {"n / vec2(hOpen, vOpen): hOpen = max(smoothstep(0.0, 0.18, power), 0.004)",
         [](U u) { return 1.0f / std::max(smoothstep_(0.0f, 0.18f, u.power), 0.004f); }, &TubeDerived::dOpenInvH},
        {"n / vec2(hOpen, vOpen): vOpen = max(smoothstep(0.18, 1.0, power), 0.003)",
         [](U u) { return 1.0f / std::max(smoothstep_(0.18f, 1.0f, u.power), 0.003f); }, &TubeDerived::dOpenInvV},
        {"nd /= 1.0 + breathe * 0.008 * ablLevel", [](U u) { return 1.0f / (1.0f + u.breathe * 0.008f * u.ablLevel); },
         &TubeDerived::dBreatheInv},
        {"degauss * 0.02", [](U u) { return u.degauss * 0.02f; }, &TubeDerived::dDegaussAmp},
        {"nd / mix(vec2(flatFit), 1.0 - marginPx / (outSize * 0.5), curvature): x",
         [](U u) { return 1.0f / mix_(u.flatFit, 1.0f - u.marginPx / (u.outW * 0.5f), u.curvature); },
         &TubeDerived::dBendInvX},
        {"nd / mix(vec2(flatFit), 1.0 - marginPx / (outSize * 0.5), curvature): y",
         [](U u) { return 1.0f / mix_(u.flatFit, 1.0f - u.marginPx / (u.outH * 0.5f), u.curvature); },
         &TubeDerived::dBendInvY},
        {"0.045 * curvature", [](U u) { return 0.045f * u.curvature; }, &TubeDerived::dBendB},
        {"nd / flatFit", [](U u) { return 1.0f / u.flatFit; }, &TubeDerived::dFlatFitInv},
        {"Ls = outH / lines", [](U u) { return u.outH / u.lines; }, &TubeDerived::dLineHeight},
        {"sp.y / Ls, sp.y / (outH / lines)", [](U u) { return 1.0f / (u.outH / u.lines); }, &TubeDerived::dLinesPerPx},
        {"lines.frag: h * 0.5, h = Ls * 0.5", [](U u) { return (u.outH / u.lines) * 0.5f * 0.5f; }, &TubeDerived::dLineTap},
        {"rows = ceil(lines)", [](U u) { return std::ceil(u.lines); }, &TubeDerived::dRows},
        {"(i0 + 0.5) / rows, gl_FragCoord.y / rows", [](U u) { return 1.0f / std::ceil(u.lines); }, &TubeDerived::dInvRows},
        {"mix(vec3(beamMin), vec3(beamMax), t) = beamMin + t * (beamMax - beamMin)",
         [](U u) { return u.beamMax - u.beamMin; }, &TubeDerived::dBeamRange},
        {"gain = maskGain * brightness - 1.0", [](U u) { return u.maskGain * u.brightness - 1.0f; }, &TubeDerived::dRollGain},
        {"start = 1.0 - 0.2 * min(gain, 1.0)",
         [](U u) { return 1.0f - 0.2f * std::min(u.maskGain * u.brightness - 1.0f, 1.0f); }, &TubeDerived::dRollStart},
        {"room = 1.0 - start",
         [](U u) { return 1.0f - (1.0f - 0.2f * std::min(u.maskGain * u.brightness - 1.0f, 1.0f)); },
         &TubeDerived::dRollRoom},
        {"col *= 1.0 - abl * 0.22 * ablLevel", [](U u) { return 1.0f - u.abl * 0.22f * u.ablLevel; }, &TubeDerived::dAblGain},
        {"p / max(vec2(0.5, 0.6) * pxPerMm, vec2(1.0)): x", [](U u) { return 1.0f / std::max(0.5f * u.pxPerMm, 1.0f); },
         &TubeDerived::dGrainInvX},
        {"p / max(vec2(0.5, 0.6) * pxPerMm, vec2(1.0)): y", [](U u) { return 1.0f / std::max(0.6f * u.pxPerMm, 1.0f); },
         &TubeDerived::dGrainInvY},
        {"6.2832 * (p.y / outH + humPhase): the scale", [](U u) { return 6.2832f / u.outH; }, &TubeDerived::dHumScale},
        {"6.2832 * (p.y / outH + humPhase): the phase", [](U u) { return 6.2832f * u.humPhase; }, &TubeDerived::dHumPhase},
        {"smoothstep(0.0, max(rollHalfWidth, 0.001), dist)", [](U u) { return 1.0f / std::max(u.rollHalfWidth, 0.001f); },
         &TubeDerived::dRollWidthInv},
        {"vec2(purityX, purityY) * outSize: x", [](U u) { return u.purityX * u.outW; }, &TubeDerived::dPurityCx},
        {"vec2(purityX, purityY) * outSize: y", [](U u) { return u.purityY * u.outH; }, &TubeDerived::dPurityCy},
        {"/ pxPerMm", [](U u) { return 1.0f / u.pxPerMm; }, &TubeDerived::dInvMm},
        {"smoothstep(0.0, max(purityRadius, 1.0), ...)", [](U u) { return 1.0f / std::max(u.purityRadius, 1.0f); },
         &TubeDerived::dPurityRadiusInv},
        {"1.0 + 2.5 * (1.0 - max(smoothstep(0.18, 1.0, power), 0.003))",
         [](U u) { return 1.0f + 2.5f * (1.0f - std::max(smoothstep_(0.18f, 1.0f, u.power), 0.003f)); },
         &TubeDerived::dPowerGain},
        {"persistence * vec3(0.68, 0.75, 0.45): r", [](U u) { return u.persistence * 0.68f; }, &TubeDerived::dDecayR},
        {"persistence * vec3(0.68, 0.75, 0.45): g", [](U u) { return u.persistence * 0.75f; }, &TubeDerived::dDecayG},
        {"persistence * vec3(0.68, 0.75, 0.45): b", [](U u) { return u.persistence * 0.45f; }, &TubeDerived::dDecayB},
        {"smoothstep(0.0, max(trackingHalfWidth, 0.0001), dist)",
         [](U u) { return 1.0f / std::max(u.trackingHalfWidth, 0.0001f); }, &TubeDerived::dTrackWidthInv},
        {"smoothstep(0.0, max(flagExtent, 0.001), yN)", [](U u) { return 1.0f / std::max(u.flagExtent, 0.001f); },
         &TubeDerived::dFlagExtentInv},
        {"(float(k) - 2.5) / 2.5 * chromaBleed", [](U u) { return u.chromaBleed / 2.5f; }, &TubeDerived::dChromaStep},
    };
}

// Every look, every row at 0, 50 (the PICTURE rows' neutral; every other row's is 0)
// and 100, on a Modern TV picture and on CRT Native, through one frame each at a few
// renderer states (power-off under way, warm-up, degauss, a measured mean, a tracking
// slip), with the plan applied as the driver applies it.
template <typename F>
void for_every_reachable_frame(F&& check) {
    const Picture pictures[] = {{1440.0, 1080.0, false}, {1280.0, 720.0, true}, {640.0, 480.0, false}};
    for (const Look look : kLooks)
        for (std::size_t row = 0; row <= kEffectCount; ++row)   // row == kEffectCount: the look itself
            for (const int value : {0, 50, 100}) {
                if (row == kEffectCount && value != 0) continue;
                TubeSetting s;
                select_look(s, look);
                if (row < kEffectCount) set_row(s, kEffects[row], value);
                const Resolved r = resolve(s);
                for (const Picture& pic : pictures) {
                    TubeUniforms still = tube_uniforms_for(r, pic);
                    apply_plan(still, plan_passes(r, pic));
                    for (const float power : {1.0f, 0.5f, 0.1f, 0.0f}) {
                        FrameState fs;
                        fs.seconds = 1234.5;
                        fs.frame = 777;
                        fs.tracking_elapsed = 0.1;
                        fs.power = power;
                        fs.degauss = power < 1.0f ? 1.0f : 0.0f;
                        fs.ablLevel = power < 0.5f ? 1.0f : 0.3f;
                        fs.warmupGain = power;
                        fs.spot = 1.0f - power;
                        fs.phases = {0.3, 0.6, 0.9};
                        check(tube_frame(still, r, fs), pic);
                    }
                }
            }
}

}  // namespace

TEST_CASE("tube derived: each value is the shader's own expression", "[tube]") {
    int frames = 0;
    for_every_reachable_frame([&](const TubeUniforms& u, const Picture&) {
        const TubeDerived d = tube_derived(u, DerivedSizes{3, 12, 720, 1080});
        for (const Pin& p : pins()) {
            INFO(p.glsl);
            REQUIRE(d.*(p.field) == Approx(p.shader(u)).epsilon(1e-6));
        }
        // rollOff's reciprocal only where the shader reaches it (gain > 0).
        if (d.dRollGain > 0.0f) REQUIRE(d.dRollRoomInv == Approx(1.0f / d.dRollRoom).epsilon(1e-6));
        ++frames;
    });
    REQUIRE(frames > 1000);
    // The sizes: (floor(p) + 0.5) / uMaskSize, gl_FragCoord.x / uSigSize.x.
    const TubeDerived d = tube_derived(tube_uniforms_for(resolve(TubeSetting{}), Picture{1440.0, 1080.0, false}),
                                       DerivedSizes{3, 12, 720, 1080});
    REQUIRE(d.dInvMaskW == 1.0f / 3.0f);
    REQUIRE(d.dInvMaskH == 1.0f / 12.0f);
    REQUIRE(d.dInvSigW == 1.0f / 720.0f);
    REQUIRE(d.dInvSigH == 1.0f / 1080.0f);
}

TEST_CASE("tube derived: every value is finite for every reachable setting", "[tube]") {
    constexpr std::size_t kFields = sizeof(TubeDerived) / sizeof(float);
    static_assert(sizeof(TubeDerived) == kFields * sizeof(float), "TubeDerived is floats only");
    for_every_reachable_frame([&](const TubeUniforms& u, const Picture&) {
        for (const DerivedSizes s : {DerivedSizes{3, 1, 720, 1080}, DerivedSizes{6, 12, 1440, 1080},
                                     DerivedSizes{0, 0, 0, 0}}) {
            const TubeDerived d = tube_derived(u, s);
            const auto* f = reinterpret_cast<const float*>(&d);
            for (std::size_t i = 0; i < kFields; ++i) {
                INFO("field " << i);
                REQUIRE(std::isfinite(f[i]));
            }
        }
    });
}

TEST_CASE("tube derived: the guards the shader had are kept", "[tube]") {
    TubeUniforms u = tube_uniforms_for(resolve(TubeSetting{}), Picture{1440.0, 1080.0, false});
    u.rollHalfWidth = u.trackingHalfWidth = u.flagExtent = u.purityRadius = 0.0f;   // rows at 0
    u.power = 0.0f;
    const TubeDerived d = tube_derived(u, DerivedSizes{});
    REQUIRE(d.dRollWidthInv == 1.0f / 0.001f);
    REQUIRE(d.dTrackWidthInv == 1.0f / 0.0001f);
    REQUIRE(d.dFlagExtentInv == 1.0f / 0.001f);
    REQUIRE(d.dPurityRadiusInv == 1.0f);
    REQUIRE(d.dOpenInvH == 1.0f / 0.004f);
    REQUIRE(d.dOpenInvV == 1.0f / 0.003f);
    REQUIRE(d.dPowerGain == Approx(1.0f + 2.5f * (1.0f - 0.003f)));
    // gain <= 0: rollOff returns early in the shader; the reciprocal is a harmless 0.
    u.maskGain = 1.0f;
    u.brightness = 1.0f;
    const TubeDerived flat = tube_derived(u, DerivedSizes{});
    REQUIRE(flat.dRollGain == 0.0f);
    REQUIRE(flat.dRollRoomInv == 0.0f);
    // At rest every multiplier is exactly neutral.
    u.power = 1.0f;
    u.degauss = u.abl = u.breathe = u.ablLevel = 0.0f;
    const TubeDerived rest = tube_derived(u, DerivedSizes{});
    REQUIRE(rest.dOpenInvH == 1.0f);
    REQUIRE(rest.dOpenInvV == 1.0f);
    REQUIRE(rest.dPowerGain == 1.0f);
    REQUIRE(rest.dBreatheInv == 1.0f);
    REQUIRE(rest.dAblGain == 1.0f);
    REQUIRE(rest.dDegaussAmp == 0.0f);
}

TEST_CASE("tube derived: the glow blur's weights are blur.frag's", "[tube]") {
    for (int row = 0; row <= 100; ++row) {
        TubeSetting s;
        set_row(s, Effect::glowSize, row);
        for (const Picture pic : {Picture{1440.0, 1080.0, false}, Picture{1920.0, 1080.0, false}, Picture{640.0, 480.0, false}}) {
            const float glow_sigma = glow_sigma_px(resolve(s), pic);
            const BlurWeights b = blur_weights(glow_sigma);
            INFO(row << " sigma " << glow_sigma);
            // tube_renderer_passes.cpp: sigma = max(glow_sigma, 0.1), radius = min(ceil(3 sigma), 24).
            REQUIRE(b.sigma == std::max(glow_sigma, 0.1f));
            REQUIRE(b.radius == std::min(static_cast<int>(std::ceil(3.0f * b.sigma)), 24));
            // blur.frag: sigma = max(uSigma, 0.001); k2 = -0.5 / (sigma * sigma); w = exp(float(k * k) * k2).
            const float sigma = std::max(b.sigma, 0.001f), k2 = -0.5f / (sigma * sigma);
            float wsum = 0.0f;
            for (int k = -b.radius; k <= b.radius; ++k) {
                const float w = std::exp(static_cast<float>(k * k) * k2);
                REQUIRE(b.weight[static_cast<std::size_t>(std::abs(k))] == w);
                wsum += w;
            }
            REQUIRE(b.inv_sum == 1.0f / wsum);
            REQUIRE(std::isfinite(b.inv_sum));
            for (int k = b.radius + 1; k <= kBlurMaxRadius; ++k) REQUIRE(b.weight[static_cast<std::size_t>(k)] == 0.0f);
        }
    }
    const BlurWeights tiny = blur_weights(0.0f);   // the floor: never a 0 sigma
    REQUIRE(tiny.sigma == 0.1f);
    REQUIRE(tiny.radius == 1);
    REQUIRE(std::isfinite(tiny.inv_sum));
}

TEST_CASE("tube derived: a texel's size in uv", "[tube]") {
    REQUIRE(texel_size(360, 270) == std::array<float, 2>{1.0f / 360.0f, 1.0f / 270.0f});
    REQUIRE(texel_size(0, -3) == std::array<float, 2>{1.0f, 1.0f});
}
