// tube_inputs.h - a tube setting turned into what the shader reads.
//
// Ported from Sound Factory's panel/TubeInputs.h (tubeUniformsFor, tubeNeutral,
// tubeMaskGain, tubeHumStrength, tubeWhite, tubeMaskType, tubeGlowSigma) and
// panel/CrtGeometry.h (openingDistance, flatFitFor), commit 4ef74a6, curve for
// curve. What differs, and why:
//   - The source IS the output: the shader draws the picture at its own size, so
//     a length Sound Factory converts to the UI's pixels (`uiPx(mm)`) is
//     `mm * pxPerMm` here, and there is no srcSize.
//   - The 4:3 picture is treated as Sound Factory's panel, kTubeMmAcross wide, so
//     a millimetre is `picture.w / kTubeMmAcross` pixels at any output mode.
//   - The opening has straight edges (no walnut bezel), so there is no arcX/arcY
//     and flatFit is Sound Factory's bisection with both arc ends at 1.0.
//   - No gate and no bypass: whatever stands in for them lives with the renderer.
//   - The two whole-pixel sizes count at the 1080-line reference (tube_looks.h).
//   - A picture already on a real CRT (`crt_native`) keeps the tube's own
//     scanlines, mask and glass: those rows are drawn at 0 and the mask as none.
//
// THE CAPS ARE NOT HERE. A row's 100 is the flash-safe ceiling because of what
// the shader multiplies each input by; these curves only say what a row's 100
// hands it, so the moving rows go through unclamped. Only the three rows that
// size something are clamped, because a pitch or a sigma out of range is a
// modulo by nothing or a blur that takes a frame.
//
// Pure: standard library only, no I/O.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "tube/tube_looks.h"
#include "tube/tube_rows.h"
#include "tube/tube_setting.h"

namespace tube {

// The millimetres the picture spans: Sound Factory's panel active area.
inline constexpr double kTubeMmAcross = 196.608;

// The opening's corner fillet and the black border the picture keeps inside it.
inline constexpr double kTubeCornerMm = 5.0;
inline constexpr double kTubeDesignMarginMm = 1.5;

// GLOW is blurred (and CHROMA BLEED read) at this fraction of the picture's size.
inline constexpr int kTubeQuarterRes = 4;

// The picture rectangle in output pixels: 1440 x 1080 at 1080p on a modern TV.
// `crt_native` when the output already is a CRT.
struct Picture {
    double w, h;
    bool crt_native;
};

// What the shader reads, Sound Factory's names. An aggregate: `TubeUniforms u{}`
// is all zeros; tube_uniforms_for() gives the fields their neutral values.
struct TubeUniforms {
    float outW, outH;  // the picture, px
    float pxPerMm;     // picture px to a millimetre of the tube
    float curvature, cornerPx, marginPx, flatFit;
    float scanlines, lines, raster, beamMin, beamMax;
    float maskType, maskStrength, maskGain, maskPitch;  // 0 none 1 grille 2 magenta-green 3 dots 4 slot; whole px a triad
    float glow, sharpness, brightness, contrast, saturation, convergence, persistence, flicker, hum;
    float vignette, rim;
    float noise, jitter, glassBlack, reflection, abl, breathe, chromaBleed;
    std::array<float, 3> tint;  // r g b
};

// The rows a real CRT already draws for itself: on one, these read as 0 (and the
// mask as none), so the tube is not drawn twice.
constexpr bool neutral_in_crt_native(Effect effect) noexcept {
    constexpr std::array<Effect, 8> kOwnedByTheTube{Effect::scanlines, Effect::raster, Effect::maskStrength,
                                                    Effect::maskSize, Effect::curvature, Effect::rim,
                                                    Effect::glassBlack, Effect::reflection};
    for (const Effect owned : kOwnedByTheTube)
        if (owned == effect) return true;
    return false;
}

// HUM 0-30 keeps its old 0-1.5% depth; the upper range smoothly reaches 25%.
// Above-range inputs stay above 1 so the shader's own cap remains testable.
inline float hum_strength(double row) noexcept {
    const double x = std::max(0.0, row / 100.0);
    const double t = std::clamp((x - 0.3) / 0.7, 0.0, 1.0);
    return static_cast<float>(0.2 * x + 0.8 * t * t * (3.0 - 2.0 * t));
}

// THE LIGHT GIVEN BACK: one over the average a mask passes at `strength`, each
// pattern's own average, the row of darker dots and slots' gaps included. The
// same at every pitch.
inline float mask_gain(Mask mask, float strength) noexcept {
    const float lo = 1.0f - strength;
    if (mask == Mask::grille) return 3.0f / (1.0f + 2.0f * lo);
    if (mask == Mask::magentaGreen) return 2.0f / (1.0f + lo);
    if (mask == Mask::dots) return (3.0f / (1.0f + 2.0f * lo)) / ((1.0f + (1.0f - 0.15f * strength)) / 2.0f);
    if (mask == Mask::slot) return (3.0f / (1.0f + 2.0f * lo)) / ((11.0f + (1.0f - 0.2f * strength)) / 12.0f);
    return 1.0f;
}

namespace detail {

// A row that sizes something, inside its range whatever it was handed.
constexpr int size_row(int row) noexcept { return std::clamp(row, kRowMin, kRowMax); }

// The shader's pattern number for `mask`, index-aligned with the enum.
inline constexpr std::array<float, 5> kMaskTypes{1.0f, 2.0f, 3.0f, 4.0f, 0.0f};
static_assert(kMaskTypes.size() == kMasks.size(), "one pattern number per mask");

constexpr float mask_type(Mask mask) noexcept {
    const auto i = static_cast<std::size_t>(mask);
    return i < kMaskTypes.size() ? kMaskTypes[i] : 0.0f;
}

// COLOUR TEMP: white at 50, the spike's warm tint at 0 and its cool one at 100,
// in a line either side.
inline std::array<float, 3> white(double row) noexcept {
    const double amount = std::clamp((row - 50.0) / 50.0, -1.0, 1.0);
    const double along = std::abs(amount);
    const std::array<double, 3> far =
        amount < 0.0 ? std::array<double, 3>{1.0, 0.95, 0.86} : std::array<double, 3>{0.90, 0.97, 1.08};
    return {static_cast<float>(1.0 + (far[0] - 1.0) * along), static_cast<float>(1.0 + (far[1] - 1.0) * along),
            static_cast<float>(1.0 + (far[2] - 1.0) * along)};
}

// Signed distance (picture px, negative inside) from the opening, for a point
// relative to the picture's centre: Sound Factory's openingDistance with both
// arc ends at 1.0, so each edge is the picture's own edge and only the corner
// fillet cuts in.
inline double opening_distance_px(const Picture& picture, double x, double y) noexcept {
    const double hx = picture.w / 2, hy = picture.h / 2;
    const double r = kTubeCornerMm * picture.w / kTubeMmAcross;
    const double qx = (std::abs(x / hx) - 1.0) * hx + r, qy = (std::abs(y / hy) - 1.0) * hy + r;
    return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - r;
}

// THE FLAT PICTURE'S FIT (CURVATURE 0): the one scale, on both axes so a flat
// picture is not stretched, at which its corner sits exactly the design margin
// inside the opening. Bisected, as Sound Factory's flatFitFor: the distance only
// grows with the scale, so the answer is one.
inline double flat_fit_for(const Picture& picture) noexcept {
    const double margin = kTubeDesignMarginMm * picture.w / kTubeMmAcross;
    double clears = 0.5, cut = 1.0;
    for (int halving = 0; halving < 50; ++halving) {
        const double scale = (clears + cut) / 2;
        if (opening_distance_px(picture, scale * picture.w / 2, scale * picture.h / 2) < -margin)
            clears = scale;
        else
            cut = scale;
    }
    return (clears + cut) / 2;
}

// What every tube shares before a setting says anything: an unbent, unmasked,
// unit-gain picture.
inline TubeUniforms tube_neutral(const Picture& picture) noexcept {
    TubeUniforms u{};
    u.outW = static_cast<float>(picture.w);
    u.outH = static_cast<float>(picture.h);
    u.pxPerMm = static_cast<float>(picture.w / kTubeMmAcross);
    u.flatFit = 1.0f;
    u.beamMin = u.beamMax = 1.0f;
    u.maskGain = 1.0f;
    u.maskPitch = 3.0f;
    u.sharpness = 1.0f;
    u.brightness = 1.0f;
    u.contrast = 1.0f;
    u.saturation = 1.0f;
    u.tint = {1.0f, 1.0f, 1.0f};
    return u;
}

}  // namespace detail

// GLOW SIZE AS THE BLUR'S SIGMA: 0.5 mm .. 6 mm of the screen, in px of the
// quarter-resolution picture, which is where the renderer blurs. Not a uniform:
// changing it is a new blur, not a new number.
inline float glow_sigma_px(const Resolved& resolved, const Picture& picture) noexcept {
    const int row = detail::size_row(resolved.effects[index_of(Effect::glowSize)]);
    const double mm = 0.5 + 5.5 * row / 100.0;
    return static_cast<float>(mm * picture.w / kTubeMmAcross / kTubeQuarterRes);
}

// A resolved setting drawn into `picture`. Corner and margin are millimetres, so
// they scale with the picture.
inline TubeUniforms tube_uniforms_for(const Resolved& resolved, const Picture& picture) noexcept {
    const auto value = [&resolved, &picture](Effect effect) {
        return picture.crt_native && neutral_in_crt_native(effect) ? 0 : resolved.effects[index_of(effect)];
    };
    const auto row = [&value](Effect effect) { return static_cast<double>(value(effect)); };
    const auto sized = [&value](Effect effect) { return detail::size_row(value(effect)); };
    const auto fraction = [](double hundredths) { return static_cast<float>(hundredths / 100.0); };
    const double px_per_mm = picture.w / kTubeMmAcross;
    const auto px = [px_per_mm](double mm) { return static_cast<float>(mm * px_per_mm); };

    auto u = detail::tube_neutral(picture);

    // The shape.
    u.curvature = fraction(row(Effect::curvature));
    u.cornerPx = px(kTubeCornerMm);
    u.marginPx = px(kTubeDesignMarginMm);
    u.flatFit = static_cast<float>(detail::flat_fit_for(picture));
    u.rim = fraction(row(Effect::rim));

    // Not light on their own, but the shader divides by them on the way.
    u.beamMin = static_cast<float>(std::max(0.12, 0.32 - 0.2 * row(Effect::beam) / 100.0));
    u.beamMax = static_cast<float>(0.32 + 0.1 * row(Effect::beam) / 100.0);
    u.lines = static_cast<float>(
        std::max(1.0, picture.h / whole_px(line_pitch_row_px(sized(Effect::lineSize)), picture.h)));

    // PICTURE: each of these is neutral at 50, not off at 0.
    u.brightness = static_cast<float>(0.5 + row(Effect::brightness) / 100.0);
    u.contrast = static_cast<float>(1.0 + (row(Effect::contrast) - 50.0) / 100.0 * 0.8);
    u.saturation = static_cast<float>(row(Effect::saturation) / 50.0);
    u.tint = detail::white(row(Effect::white));
    u.sharpness = static_cast<float>(row(Effect::sharpness) / 50.0);

    // SCANLINES
    u.scanlines = fraction(row(Effect::scanlines));
    u.raster = fraction(row(Effect::raster));

    // MASK
    const Mask mask = picture.crt_native ? Mask::none : resolved.mask;
    u.maskType = detail::mask_type(mask);
    u.maskStrength = static_cast<float>(row(Effect::maskStrength) * 0.008);
    u.maskGain = mask_gain(mask, u.maskStrength);
    u.maskPitch = static_cast<float>(whole_px(mask_pitch_row_px(sized(Effect::maskSize)), picture.h));

    // LIGHT (GLOW SIZE is the blur's: glow_sigma_px)
    u.glow = fraction(row(Effect::glow));
    u.persistence = fraction(row(Effect::afterglow));

    // GLASS
    u.vignette = fraction(row(Effect::vignette));
    u.glassBlack = fraction(row(Effect::glassBlack));
    u.reflection = fraction(row(Effect::reflection));

    // SIGNAL
    u.convergence = px(row(Effect::convergence) / 100.0 * 0.60);
    u.chromaBleed = px(row(Effect::chromaBleed) / 100.0 * 3.0);
    u.noise = static_cast<float>(row(Effect::noise) * 0.002);
    u.jitter = fraction(row(Effect::jitter));

    // SUPPLY
    u.flicker = fraction(row(Effect::flicker));
    u.hum = hum_strength(row(Effect::hum));
    u.abl = fraction(row(Effect::abl));
    u.breathe = static_cast<float>(row(Effect::breathing) * 0.01);
    return u;
}

}  // namespace tube
