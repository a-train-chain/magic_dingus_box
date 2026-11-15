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
//   - On a real CRT the picture is not inset either: no corner fillet, no design
//     margin, flatFit 1 (design section 3, owner question 1). The real tube's own
//     glass is the opening; Sound Factory always draws its bezel's.
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
#include <cstdint>

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
// `crt_native` when the output already is a CRT. Precondition: w > 0 && h > 0
// (a millimetre is w / kTubeMmAcross pixels, and the shader divides by both).
struct Picture {
    double w, h;
    bool crt_native;
};

// What the shader reads, Sound Factory's names. An aggregate: `TubeUniforms u{}`
// is all zeros; tube_uniforms_for() gives the fields their neutral values.
// NOT laid out for a GLSL uniform block (not std140: `tint` is three packed
// floats, and nothing is padded to a vec4): upload it field by field, never hand
// the struct to glBufferData as one block.
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

    // What one frame owns: tube_frame_phases() writes these, each already
    // wrapped, so no float the shader reads grows with the kiosk's uptime.
    float humPhase;                      // 0..1 of the hum bar's roll
    float flickerWave;                   // 0.6 sin(37.7 t) + 0.4 sin(61.3 t + 1.7)
    float noiseSeed;                     // fract(frame * 0.618) * 913
    float jitterSeed;                    // frame mod 1024
    float degaussPhaseA, degaussPhaseB;  // 31 t and 27 t, radians wrapped to 2 pi
    float jitterBias;                    // -0.5 at rest; tube_signal_motion() moves it

    // Written by the renderer alone: the limiter's level, the power animations,
    // the degauss wobble's decay, the power-off dot, the warm-up's light.
    float ablLevel, power, degauss, spot, warmupGain;

    // The optional character: tube_character_frame() writes these once a frame.
    float rollAmount, rollPosition, rollHalfWidth, rollTint;
    float trackingOffset, trackingPosition, trackingHalfWidth, driftOffset;
    float flagOffset, flagExtent;
    float purityAmount, purityX, purityY, purityRadius, purityR, purityG, purityB;
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
    u.power = u.warmupGain = 1.0f;
    u.jitterBias = -0.5f;
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
// they scale with the picture. On a real CRT (`crt_native`) there are none and
// the picture fills the output: the tube's own glass already rounds and frames it.
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
    if (!picture.crt_native) {   // CRT Native keeps tube_neutral's 0, 0 and 1
        u.cornerPx = px(kTubeCornerMm);
        u.marginPx = px(kTubeDesignMarginMm);
        u.flatFit = static_cast<float>(detail::flat_fit_for(picture));
    }
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

// ---- What one frame owns ---------------------------------------------------
//
// THE PER-FRAME CONTRACT.
//   - tube_uniforms_for() gives the STILL uniforms: everything the setting and
//     the picture decide. It bisects, so a renderer may cache it until either
//     changes.
//   - tube_frame() is the per-frame entry point: still uniforms + FrameState in,
//     this frame's uniforms out. It never touches the still. `seconds` drives
//     hum, flicker, degauss and signal motion; the rolling band, colour drift
//     and flagging are drawn at FrameState::phases, which the renderer advances
//     once a drawn frame with advance_character_phases() (see "What differs").
//   - tube_frame_phases(), tube_signal_motion() and tube_character_frame() are
//     its parts. Anyone calling them directly must apply them to a FRESH COPY of
//     the still uniforms each frame, in that order (tube_frame's own order, which
//     is Sound Factory's TubeRenderer). tube_signal_motion() must never run twice
//     on the same uniforms: it scales them in place (JITTER loses its share again,
//     SHARPNESS softens again), so a second run decays JITTER and drifts
//     SHARPNESS a little every frame, with nothing logged. Sound Factory is safe
//     only because it rebuilds the uniforms every frame.
//   - tube_character_frame() needs uniforms that came from tube_uniforms_for():
//     it divides by `outH` and scales by `pxPerMm`, both 0 in a `TubeUniforms{}`.
//   - Only the renderer writes ablLevel, power, degauss, spot and warmupGain
//     (through FrameState); the still holds their at-rest values.
//   - Not here: Sound Factory zeroes glow, chromaBleed, abl and breathe when it
//     has no quarter-resolution pass. That is what the renderer can draw, not
//     what the frame is, so a renderer without that pass does it to the still.
//
// Ported from Sound Factory's panel/TubeInputs.h (tubeWarmupGain,
// tubeSignalMotion, tubeCharacterStrength, tubeTracking*, tubeCharacterFrame,
// tubeSettleFrames and their constants), commit 4ef74a6. What differs:
//   - Sound Factory hands its shader raw `time` and `frame` floats and does
//     sin(time * 37.7), time / humPeriod, hash(line, frame) and
//     fract(frame * 0.618) per pixel. The kiosk runs for weeks and a float
//     seconds-since-start loses the precision those need, so each term is
//     worked here in double once a frame and handed over wrapped:
//     tube_frame_phases().
//   - A millimetre is `u.pxPerMm` pixels: the source is the output.
//   - No bypass: `enabled` is the one switch.
//   - The three speed rows' phases are ACCUMULATED, not derived from time.
//     Sound Factory draws the rolling band at fmod(time / period, 1) and colour
//     drift and flagging at sin(time * 2 pi / period), so moving ROLL SPEED,
//     DRIFT SPEED or FLAG SPEED while the kiosk has run a while jumps the band,
//     the drift and the bend to wherever the new period puts that much time.
//     Here the renderer keeps a CharacterPhases and advances it each frame
//     (advance_character_phases), so a speed step changes only the RATE. TRACKING
//     keeps Sound Factory's clock (`tracking_elapsed`): a slip restarts when a
//     tracking row changes, which is the behaviour it had there.

// Seconds per roll of the hum bar: 60 Hz mains against a 59.94 Hz field beat.
inline constexpr double kHumPeriodSeconds = 16.7;

namespace detail {

constexpr double kTwoPi = 6.283185307179586;

// `x` wrapped into [0, period), as a float that is still below `period`.
inline float wrapped(double x, double period) noexcept {
    const double w = x - std::floor(x / period) * period;
    const float f = static_cast<float>(w);
    return f >= static_cast<float>(period) || f < 0.0f ? 0.0f : f;
}

inline double smooth(double x) noexcept {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

// A row as a 0..1 fraction, clamped.
inline double row_fraction(const Resolved& resolved, Effect effect) noexcept {
    return std::clamp(resolved.effects[index_of(effect)] / 100.0, 0.0, 1.0);
}

// Optional character's curve: the row itself up to 0.3, reaching `ceiling` at 1.
inline double character_strength(double value, double ceiling) noexcept {
    const double x = std::clamp(value, 0.0, 1.0);
    const double t = std::clamp((x - 0.3) / 0.7, 0.0, 1.0);
    return x + (ceiling - 1.0) * t * t * (3.0 - 2.0 * t);
}

}  // namespace detail

// THE FRAME'S PHASES, from `seconds` since the kiosk started and the frame's
// count. All the wrapping is here, in double, so the shader only ever sees
// small numbers.
inline void tube_frame_phases(TubeUniforms& u, double seconds, std::uint64_t frame) noexcept {
    const double f = static_cast<double>(frame);
    u.humPhase = detail::wrapped(seconds / kHumPeriodSeconds, 1.0);
    u.flickerWave = static_cast<float>(0.6 * std::sin(seconds * 37.7) + 0.4 * std::sin(seconds * 61.3 + 1.7));
    u.noiseSeed = static_cast<float>(std::fmod(f * 0.618, 1.0) * 913.0);
    u.jitterSeed = static_cast<float>(frame % 1024u);  // repeats every 1024 frames (~17 s at 60 Hz) on purpose:
                                                        // small and exact in a float; not an unbounded counter
    u.degaussPhaseA = detail::wrapped(seconds * 31.0, detail::kTwoPi);
    u.degaussPhaseB = detail::wrapped(seconds * 27.0, detail::kTwoPi);
}

// A compressed impression of cathode warm-up, not a model-specific simulation:
// a full-sized raster, 0.2 s dark, then light emerging and settling by 1.5 s.
// At rest exactly 1.
inline constexpr double kWarmupSeconds = 1.5;
inline double warmup_gain(double seconds) noexcept {
    if (seconds <= 0.2) return 0.0;
    if (seconds < 0.8) return 0.8 * detail::smooth((seconds - 0.2) / 0.6);
    return 0.8 + 0.2 * detail::smooth((seconds - 0.8) / 0.7);
}

// Refines the signal and supply rows once a frame, never a saved control: 15%
// of JITTER's displacement budget goes to correlated line motion, and FOCUS
// softens a little with rising light, but only through an already-active filter
// (neutral sharpness adds no texture reads).
inline void tube_signal_motion(TubeUniforms& u, double seconds, double amount, double rising_light) noexcept {
    const double mix = std::clamp(amount, 0.0, 1.0);
    const auto share = static_cast<float>(u.jitter >= 0.0f && u.jitter <= 1.0f ? 0.15 * mix : 0.0);
    u.jitterBias = -0.5f + share / (1.0f - share) *
                               static_cast<float>(0.5 * (0.65 * std::sin(seconds * 2.1) + 0.35 * std::sin(seconds * 3.7)));
    u.jitter *= 1.0f - share;  // malformed rows still reach the shader's own cap
    if (u.sharpness != 1.0f)
        u.sharpness *= static_cast<float>(1.0 - 0.03 * mix * std::clamp(static_cast<double>(u.breathe), 0.0, 1.0) *
                                                    std::clamp(rising_light, 0.0, 1.0));
}

// ---- The accumulated phases of the three speed rows -------------------------

// Each a fraction of one cycle, in [0, 1). The renderer owns one, advances it
// once a drawn frame with advance_character_phases() and hands it to the frame
// through FrameState::phases.
struct CharacterPhases {
    double roll = 0.0, drift = 0.0, flag = 0.0;
};

// The most one advance may move the phases: a frame after a long gap (a game,
// the Media Browser, a stalled loop) resumes where it left off rather than
// leaping, and a clock that went backwards moves nothing.
inline constexpr double kMaxPhaseStepSeconds = 0.1;

// Seconds a cycle, from each speed row (Sound Factory's periods).
inline double roll_period(const Resolved& resolved) noexcept {
    return 30.0 - 24.0 * detail::row_fraction(resolved, Effect::rollSpeed);
}
inline double drift_period(const Resolved& resolved) noexcept {
    return 24.0 - 18.0 * detail::row_fraction(resolved, Effect::driftSpeed);
}
inline double flag_period(const Resolved& resolved) noexcept {
    return 12.0 - 10.0 * detail::row_fraction(resolved, Effect::flagSpeed);
}

// `dt` seconds more of each phase at the current speeds, `dt` clamped to
// [0, kMaxPhaseStepSeconds], each kept in one cycle.
inline void advance_character_phases(CharacterPhases& phases, const Resolved& resolved, double dt) noexcept {
    const double step = dt > 0.0 ? std::min(dt, kMaxPhaseStepSeconds) : 0.0;  // a NaN dt moves nothing
    const auto advance = [step](double& phase, double period) {
        const double x = phase + step / period;
        phase = x - std::floor(x);
        if (!(phase >= 0.0 && phase < 1.0)) phase = 0.0;  // only a phase that was already NaN or infinite
    };
    advance(phases.roll, roll_period(resolved));
    advance(phases.drift, drift_period(resolved));
    advance(phases.flag, flag_period(resolved));
}

// TRACKING: one slip every period, lasting the duration (RECOVERY).
inline double tracking_duration(const Resolved& resolved) noexcept {
    return 0.12 + detail::row_fraction(resolved, Effect::trackingRecovery);
}
inline double tracking_period(const Resolved& resolved) noexcept {
    return 60.0 - 56.0 * detail::row_fraction(resolved, Effect::trackingRate);
}
// True while a slip is under way, plus one clean frame after it settles.
inline bool tracking_moving(const Resolved& resolved, double elapsed) noexcept {
    return resolved.effects[index_of(Effect::tracking)] > 0 &&
           std::fmod(std::max(0.0, elapsed), tracking_period(resolved)) < tracking_duration(resolved) + 0.05;
}

// THE OPTIONAL CHARACTER, once a frame: the rolling band, the tracking slip,
// colour drift, flagging and the purity patch. Disabled, every field is 0. The
// band, drift and flagging are drawn at `phases` (accumulated by the renderer);
// tracking at `tracking_elapsed`.
inline void tube_character_frame(TubeUniforms& u, const Resolved& resolved, const CharacterPhases& phases,
                                 double tracking_elapsed, bool enabled) noexcept {
    u.rollAmount = u.rollPosition = u.rollHalfWidth = u.rollTint = 0.0f;
    u.trackingOffset = u.trackingPosition = u.driftOffset = 0.0f;
    u.trackingHalfWidth = u.flagOffset = u.flagExtent = 0.0f;
    u.purityAmount = u.purityX = u.purityY = u.purityRadius = u.purityR = u.purityG = u.purityB = 0.0f;
    if (!enabled) return;
    const auto row = [&resolved](Effect effect) { return detail::row_fraction(resolved, effect); };
    const double px_per_mm = u.pxPerMm;
    constexpr double kTwoPi = detail::kTwoPi;
    if (row(Effect::rollBand) > 0.0) {
        u.rollAmount = static_cast<float>(detail::character_strength(row(Effect::rollBand), 2.0));
        u.rollPosition = static_cast<float>(phases.roll);
        u.rollHalfWidth = static_cast<float>((4.0 + 26.0 * row(Effect::rollWidth)) * px_per_mm / u.outH);
        u.rollTint = static_cast<float>(2.0 * row(Effect::rollTint) - 1.0);
    }
    if (tracking_moving(resolved, tracking_elapsed)) {
        const double period = tracking_period(resolved), elapsed = std::max(0.0, tracking_elapsed);
        const double phase = std::fmod(elapsed, period), cycle = std::floor(elapsed / period);
        const double duration = tracking_duration(resolved), attack = duration * (0.08 / 0.55);
        const double envelope =
            detail::smooth(phase / attack) * (1.0 - detail::smooth((phase - attack) / (duration - attack)));
        u.trackingHalfWidth = static_cast<float>((1.0 + 15.0 * row(Effect::trackingWidth)) * px_per_mm / u.outH);
        u.trackingOffset = static_cast<float>(1.2 * px_per_mm * detail::character_strength(row(Effect::tracking), 3.0) *
                                              envelope * std::cos((phase - attack) / duration * 9.9));
        u.trackingPosition = static_cast<float>(0.15 + 0.70 * std::fmod(0.37 + cycle * 0.61803398875, 1.0));
    }
    if (row(Effect::colorDrift) > 0.0)
        u.driftOffset = static_cast<float>(0.25 * px_per_mm * detail::character_strength(row(Effect::colorDrift), 3.0) *
                                           std::sin(kTwoPi * phases.drift));
    if (row(Effect::flagging) > 0.0) {
        const double phase = kTwoPi * phases.flag;
        u.flagOffset = static_cast<float>(4.0 * px_per_mm * row(Effect::flagging) *
                                          (0.8 * std::sin(phase) + 0.2 * std::sin(phase * 2.0)));
        u.flagExtent = static_cast<float>(0.05 + 0.30 * row(Effect::flagHeight));
    }
    if (row(Effect::purity) > 0.0) {
        const double angle = (-0.375 + row(Effect::purityPosition)) * kTwoPi;
        u.purityAmount = static_cast<float>(row(Effect::purity));
        u.purityX = static_cast<float>(0.5 + 0.50 * std::cos(angle));
        u.purityY = static_cast<float>(0.5 + 0.50 * std::sin(angle));
        u.purityRadius = static_cast<float>(10.0 + 60.0 * row(Effect::puritySize));
        const double hue = row(Effect::purityHue) * kTwoPi;
        u.purityR = static_cast<float>(0.5 + 0.5 * std::cos(hue));
        u.purityG = static_cast<float>(0.5 + 0.5 * std::cos(hue - 2.094395102393195));
        u.purityB = static_cast<float>(0.5 + 0.5 * std::cos(hue + 2.094395102393195));
    }
}

// What the renderer alone knows about this frame.
struct FrameState {
    double seconds = 0.0;           // since kiosk start, double precision
    std::uint64_t frame = 0;        // drawn-frame counter
    double tracking_elapsed = 0.0;  // seconds since tracking's clock started
    bool motion_enabled = true;     // false while powering off or a transition is open
    bool character_enabled = true;  // as motion_enabled, and false too wherever the renderer gates the tube
    double motion_amount = 1.0;     // tube_signal_motion's `amount`
    double rising_light = 0.0;      // tube_signal_motion's `rising_light`
    float ablLevel = 0.0f;          // measured average light, smoothed
    float power = 1.0f;             // 0 off .. 1 on
    float degauss = 0.0f;           // 1 .. 0 decaying
    float spot = 0.0f;              // power-off dot
    float warmupGain = 1.0f;        // warmup_gain(seconds since the tube started)
    CharacterPhases phases{};       // the band, drift and flagging: advance_character_phases()
};

// ONE FRAME, from the still uniforms (tube_uniforms_for, cacheable) and what the
// renderer knows. Sound Factory's TubeRenderer encodeTubeInto, step for step: the
// clock's phases, the renderer's own fields, warm-up softening the picture
// through the existing bandwidth filter, signal motion, then the character. The
// still is copied, never changed, so this may run any number of times.
inline TubeUniforms tube_frame(const TubeUniforms& still, const Resolved& resolved, const FrameState& state) noexcept {
    TubeUniforms u = still;
    tube_frame_phases(u, state.seconds, state.frame);
    u.ablLevel = state.ablLevel;
    u.power = state.power;
    u.warmupGain = state.warmupGain;
    if (u.warmupGain < 1.0f) u.sharpness *= 0.8f + 0.2f * u.warmupGain;
    u.degauss = state.degauss;
    if (state.motion_enabled) tube_signal_motion(u, state.seconds, state.motion_amount, state.rising_light);
    tube_character_frame(u, resolved, state.phases, state.tracking_elapsed, state.character_enabled);
    u.spot = state.spot;
    return u;
}

// What changes on screen with nothing else changing: the rows that move every
// frame. TRACKING is asked separately, through tracking_moving(), because it
// sleeps between slips.
inline bool moves_on_its_own(const Resolved& resolved) noexcept {
    constexpr std::array<Effect, 7> kMoving{Effect::noise, Effect::jitter,     Effect::flicker, Effect::hum,
                                            Effect::rollBand, Effect::colorDrift, Effect::flagging};
    for (const Effect effect : kMoving)
        if (resolved.effects[index_of(effect)] > 0) return true;
    return false;
}

// The brightness limiter's time constant, seconds.
inline constexpr double kAblSeconds = 0.1;

// What "landed" means, for the afterglow's trail and the limiter alike: 0.02 %.
inline constexpr double kSettledWithin = 0.0002;

// How often a settling frame renders when nothing else is drawing: every second
// 60 Hz tick.
inline constexpr double kSettleFramesPerSecond = 30.0;

// HOW MANY RENDERED FRAMES A PICTURE OWES, ITSELF INCLUDED, before the GPU can
// rest. Two things left to land, each to kSettledWithin, and the debt is
// whichever needs longer:
//   - THE AFTERGLOW decays per rendered frame; the strongest channel keeps
//     `persistence * 0.75` a frame: 4 frames at AFTERGLOW 10, 6 at 30, 30 at 100.
//   - THE LIMITER runs on the clock and owes frames only while it has distance
//     to travel (`abl_distance`): 26 across the whole range, none for a picture
//     as bright as the last.
inline int settle_frames(const TubeUniforms& u, double abl_distance) noexcept {
    constexpr double kStrongestKept = 0.75;
    const double kept = std::clamp(static_cast<double>(u.persistence), 0.0, 1.0) * kStrongestKept;
    double frames = kept > 0.0 ? std::ceil(std::log(kSettledWithin) / std::log(kept)) : 0.0;
    const double distance = std::abs(abl_distance);
    if ((u.abl > 0.0f || u.breathe > 0.0f) && distance > kSettledWithin)
        frames = std::max(frames,
                          std::ceil(kAblSeconds * std::log(distance / kSettledWithin) * kSettleFramesPerSecond));
    return static_cast<int>(frames);
}

}  // namespace tube
