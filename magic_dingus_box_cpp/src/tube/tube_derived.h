// tube_derived.h - the values the Tube shaders used to work out PER PIXEL from
// uniforms alone, worked out once a frame here instead (measured on the Pi 5: the
// output pass's time follows its instruction count, and a reciprocal, a smoothstep
// of a uniform or a product of two uniforms costs every one of 1.5 M pixels).
//
// Each field is the SAME expression the shader had, in float (the shader's
// precision), with the same guards: a `max(...)` floor the shader put on a divisor
// is kept here, and where the shader only used a value behind `if (x > 0.0)` the
// value is still only used behind that test (the test stays in the GLSL). Every
// field is finite for every reachable setting (tests/app/test_tube_derived.cpp
// sweeps them).
//
// NOT HERE, measured: the glow blur's weights, the lines pass's and the quarter pass's
// reciprocals. Moved here (the weights as a uniform array) they measured SLOWER on the
// Pi 5 (blur 1.83 -> 1.99 ms, lines 1.02 -> 1.06 ms; the quarter no different): those
// passes are bound by their texture reads, not their arithmetic. Those three files
// keep their own arithmetic.
//
// NOT HERE, measured: the bend (output.frag bend()). Its divisor worked out on the CPU,
// as a reciprocal or as the divisor itself, moved sp by an ulp and with it a few dozen
// pixels on the picture's edge from one side of the `lit` test to the other (up to
// 107/255 on Trinitron's scene programs, shaderstat --compare); the GLSL keeps it.
//
// NOT HERE, deliberately: the flash-safety caps (`min(noise, 0.2)`,
// `min(flicker, 1.0) * 0.025 * clamp(flickerWave, ...)`, `min(hum, 1.0) * 0.25`,
// `min(jitter, 1.0) * 0.50 * mm`) and every other clamp the shader puts on an
// uploaded amount (tracking and flagging offsets, the rolling band's amount and
// tint, purity's amount and hue) stay in the GLSL on the raw uniform, so the
// shader bounds them whatever the CPU uploads.
//
// The renderer uploads the fields by name (kDerivedFields below; a static_assert
// keeps the table complete) to the programs whose uniforms_of() lists them.
//
// Pure: standard library only, no I/O, no GL.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "tube/tube_inputs.h"

namespace tube {

// The pass sizes the derived values need, all known to the renderer each frame.
struct DerivedSizes {
    int mask_w = 1, mask_h = 1;       // the mask tile, texels (one period)
    int signal_w = 1, signal_h = 1;   // the signal target (= the lines target's width)
};

// One frame's derived values. Floats only: each is one GLSL float uniform of the
// same name.
struct TubeDerived {
    // The picture.
    float dInvW, dInvH;            // 1.0 / outW, 1.0 / outH
    float dInvHalfW, dInvHalfH;    // 1.0 / (outW * 0.5), 1.0 / (outH * 0.5)
    float dAspect;                 // outW / outH
    // ndOf (the output's raster deflection).
    float dOpenInvH, dOpenInvV;    // 1 / max(smoothstep(0, 0.18, power), 0.004), 1 / max(smoothstep(0.18, 1, power), 0.003)
    float dBreatheInv;             // 1.0 / (1.0 + breathe * 0.008 * ablLevel)
    float dDegaussAmp;             // degauss * 0.02
    // The beam and the lines.
    float dLinesPerPx;             // 1.0 / (outH / lines): sp.y / Ls == sp.y * dLinesPerPx
    float dInvRows;                // 1.0 / ceil(lines)
    float dBeamRange;              // beamMax - beamMin
    // rollOff (gain > 0 only; the shader tests dRollGain).
    float dRollGain;               // maskGain * brightness - 1.0
    float dRollStart, dRollRoom;   // 1 - 0.2 * min(gain, 1), 1 - start
    float dRollRoomInv;            // 1.0 / room when gain > 0, else 0 (never read)
    // The mask.
    float dInvMaskW, dInvMaskH;    // 1.0 / mask size
    // ABL.
    float dAblGain;                // 1.0 - abl * 0.22 * ablLevel
    // The motion block (the caps stay in the GLSL).
    float dGrainInvX, dGrainInvY;  // 1 / max(vec2(0.5, 0.6) * pxPerMm, 1.0)
    float dHumScale, dHumPhase;    // 6.2832 / outH, 6.2832 * humPhase
    float dRollWidthInv;           // 1.0 / max(rollHalfWidth, 0.001)
    float dPurityCx, dPurityCy;    // purityX * outW, purityY * outH
    float dInvMm;                  // 1.0 / pxPerMm
    float dPurityRadiusInv;        // 1.0 / max(purityRadius, 1.0)
    // Power and afterglow.
    float dPowerGain;              // 1 + 2.5 * (1 - max(smoothstep(0.18, 1, power), 0.003))
    float dDecayR, dDecayG, dDecayB;   // persistence * vec3(0.68, 0.75, 0.45)
    // The displacements (output OUT_DISPLACE and the signal pass).
    float dTrackWidthInv;          // 1.0 / max(trackingHalfWidth, 0.0001)
    float dFlagExtentInv;          // 1.0 / max(flagExtent, 0.001)
    // The signal pass.
    float dInvSigW, dInvSigH;      // 1.0 / signal size
};

// Every field by its GLSL name, for the renderer's upload table and the tools.
struct DerivedField {
    const char* name;
    float TubeDerived::*member;
};
inline constexpr std::array<DerivedField, 36> kDerivedFields{{
    DerivedField{"dInvW", &TubeDerived::dInvW},
    DerivedField{"dInvH", &TubeDerived::dInvH},
    DerivedField{"dInvHalfW", &TubeDerived::dInvHalfW},
    DerivedField{"dInvHalfH", &TubeDerived::dInvHalfH},
    DerivedField{"dAspect", &TubeDerived::dAspect},
    DerivedField{"dOpenInvH", &TubeDerived::dOpenInvH},
    DerivedField{"dOpenInvV", &TubeDerived::dOpenInvV},
    DerivedField{"dBreatheInv", &TubeDerived::dBreatheInv},
    DerivedField{"dDegaussAmp", &TubeDerived::dDegaussAmp},
    DerivedField{"dLinesPerPx", &TubeDerived::dLinesPerPx},
    DerivedField{"dInvRows", &TubeDerived::dInvRows},
    DerivedField{"dBeamRange", &TubeDerived::dBeamRange},
    DerivedField{"dRollGain", &TubeDerived::dRollGain},
    DerivedField{"dRollStart", &TubeDerived::dRollStart},
    DerivedField{"dRollRoom", &TubeDerived::dRollRoom},
    DerivedField{"dRollRoomInv", &TubeDerived::dRollRoomInv},
    DerivedField{"dInvMaskW", &TubeDerived::dInvMaskW},
    DerivedField{"dInvMaskH", &TubeDerived::dInvMaskH},
    DerivedField{"dAblGain", &TubeDerived::dAblGain},
    DerivedField{"dGrainInvX", &TubeDerived::dGrainInvX},
    DerivedField{"dGrainInvY", &TubeDerived::dGrainInvY},
    DerivedField{"dHumScale", &TubeDerived::dHumScale},
    DerivedField{"dHumPhase", &TubeDerived::dHumPhase},
    DerivedField{"dRollWidthInv", &TubeDerived::dRollWidthInv},
    DerivedField{"dPurityCx", &TubeDerived::dPurityCx},
    DerivedField{"dPurityCy", &TubeDerived::dPurityCy},
    DerivedField{"dInvMm", &TubeDerived::dInvMm},
    DerivedField{"dPurityRadiusInv", &TubeDerived::dPurityRadiusInv},
    DerivedField{"dPowerGain", &TubeDerived::dPowerGain},
    DerivedField{"dDecayR", &TubeDerived::dDecayR},
    DerivedField{"dDecayG", &TubeDerived::dDecayG},
    DerivedField{"dDecayB", &TubeDerived::dDecayB},
    DerivedField{"dTrackWidthInv", &TubeDerived::dTrackWidthInv},
    DerivedField{"dFlagExtentInv", &TubeDerived::dFlagExtentInv},
    DerivedField{"dInvSigW", &TubeDerived::dInvSigW},
    DerivedField{"dInvSigH", &TubeDerived::dInvSigH},
}};
static_assert(sizeof(TubeDerived) == kDerivedFields.size() * sizeof(float),
              "a TubeDerived field is missing from kDerivedFields (it would never be uploaded)");

namespace detail {
// GLSL's smoothstep, in float.
inline float glsl_smoothstep(float e0, float e1, float x) noexcept {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
}  // namespace detail

inline TubeDerived tube_derived(const TubeUniforms& u, const DerivedSizes& s) noexcept {
    using detail::glsl_smoothstep;
    TubeDerived d{};
    d.dInvW = 1.0f / u.outW;
    d.dInvH = 1.0f / u.outH;
    d.dInvHalfW = 1.0f / (u.outW * 0.5f);
    d.dInvHalfH = 1.0f / (u.outH * 0.5f);
    d.dAspect = u.outW / u.outH;

    const float vOpen = std::max(glsl_smoothstep(0.18f, 1.0f, u.power), 0.003f);
    const float hOpen = std::max(glsl_smoothstep(0.0f, 0.18f, u.power), 0.004f);
    d.dOpenInvH = 1.0f / hOpen;
    d.dOpenInvV = 1.0f / vOpen;
    d.dBreatheInv = 1.0f / (1.0f + u.breathe * 0.008f * u.ablLevel);
    d.dDegaussAmp = u.degauss * 0.02f;

    d.dLinesPerPx = 1.0f / (u.outH / u.lines);
    d.dInvRows = 1.0f / std::ceil(u.lines);
    d.dBeamRange = u.beamMax - u.beamMin;

    d.dRollGain = u.maskGain * u.brightness - 1.0f;
    d.dRollStart = 1.0f - 0.2f * std::min(d.dRollGain, 1.0f);
    d.dRollRoom = 1.0f - d.dRollStart;
    d.dRollRoomInv = d.dRollGain > 0.0f ? 1.0f / d.dRollRoom : 0.0f;

    d.dInvMaskW = 1.0f / static_cast<float>(std::max(s.mask_w, 1));
    d.dInvMaskH = 1.0f / static_cast<float>(std::max(s.mask_h, 1));

    d.dAblGain = 1.0f - u.abl * 0.22f * u.ablLevel;

    d.dGrainInvX = 1.0f / std::max(0.5f * u.pxPerMm, 1.0f);
    d.dGrainInvY = 1.0f / std::max(0.6f * u.pxPerMm, 1.0f);
    d.dHumScale = 6.2832f / u.outH;
    d.dHumPhase = 6.2832f * u.humPhase;
    d.dRollWidthInv = 1.0f / std::max(u.rollHalfWidth, 0.001f);
    d.dPurityCx = u.purityX * u.outW;
    d.dPurityCy = u.purityY * u.outH;
    d.dInvMm = 1.0f / u.pxPerMm;
    d.dPurityRadiusInv = 1.0f / std::max(u.purityRadius, 1.0f);

    d.dPowerGain = 1.0f + 2.5f * (1.0f - vOpen);
    d.dDecayR = u.persistence * 0.68f;
    d.dDecayG = u.persistence * 0.75f;
    d.dDecayB = u.persistence * 0.45f;

    d.dTrackWidthInv = 1.0f / std::max(u.trackingHalfWidth, 0.0001f);
    d.dFlagExtentInv = 1.0f / std::max(u.flagExtent, 0.001f);

    d.dInvSigW = 1.0f / static_cast<float>(std::max(s.signal_w, 1));
    d.dInvSigH = 1.0f / static_cast<float>(std::max(s.signal_h, 1));
    return d;
}

}  // namespace tube
