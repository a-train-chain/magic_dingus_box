// tube_driver.h - the Tube renderer's per-frame driver: the setting, the picture and
// the clock in; this frame's uniforms, pass plan, target sizes, mask tile and what
// changed out.
//
// It sits between the pure core (tube_inputs.h, tube_programs.h) and the GL renderer,
// which uploads what it is handed and draws the plan. It owns the clocks (warm-up,
// degauss, tracking, the accumulated phases), the brightness limiter and the settle
// debt, and caches the still uniforms, so the bisection in tube_uniforms_for() runs
// only when the setting or the picture changes.
//
// RUNS ON THE KIOSK'S MAIN THREAD EVERY FRAME (a 10 s watchdog, a 16.7 / 33.3 ms
// deadline): a frame whose setting and picture are unchanged allocates nothing. The
// mask tile's pixels ride in DriverFrame only on the frame that changed them
// (`mask_changed`); on every other frame `mask.rgb` is empty and `mask.w` / `mask.h`
// still say the tile's size. The driver keeps its own copy (`FrameDriver::mask()`)
// for a renderer that must upload it again (a rebuilt GL context).
//
// Ported from Sound Factory's TubeRenderer.mm (readMean, stepBrightnessLimitTo, the
// FrameState assembly in encodeTubeInto at lines 937-958, owesSettling, the "moving"
// test) and crt.metal's maskAt (through the spike's mask_at), commit 4ef74a6. What
// differs, and why:
//   - The rolling band, colour drift and flagging are drawn at ACCUMULATED phases
//     (tube_inputs.h, advance_character_phases), advanced by each drawn frame's dt:
//     Sound Factory derives them from `time`, so a speed step jumped them.
//   - Degauss is started by a COUNTER from the web's op log (AppState's
//     tube_live.degauss_requests) rising above the baseline the driver last saw, not
//     by a method call: the driver only reads state. start() takes the counter's
//     current value as the baseline, so a tube never degausses on starting.
//   - No power-off animation and no transition gate here: power 1, spot 0, motion and
//     character always enabled. Whatever stands in for them lives with the renderer.
//   - The mean probe exists only on a plan that asks for it (`plan.mean_probe`, which
//     the measured lean set puts only on Rich30 plans). Without it the limiter rests
//     at its neutral level (ablLevel 0, the shader's no-op) and no mean is asked for,
//     as Sound Factory's ablLevel reads 0 with no prepass.
//   - on_mean() takes the light already reduced to luma (0.2126 R + 0.7152 G +
//     0.0722 B, linear): the GL side reads the 1x1 mip and does that sum.
//
// Pure: standard library only, no I/O, no GL.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "tube/tube_frame_class.h"
#include "tube/tube_inputs.h"
#include "tube/tube_programs.h"
#include "tube/tube_rows.h"
#include "tube/tube_setting.h"

namespace tube {

// The glass bake's RGB is written x32 into its RGBA8 target (glass.frag) and divided
// back by the output pass (output.frag): glass black at 1 is about one 8-bit step.
inline constexpr float kGlassLightScale = 32.0f;

// Seconds the degauss wobble decays over, and how long a degauss keeps frames coming.
inline constexpr double kDegaussSeconds = 1.4, kDegaussMovesSeconds = 1.5;
// How long a starting tube keeps frames coming: the warm-up and one clean frame.
inline constexpr double kWarmupMovesSeconds = kWarmupSeconds + 0.1;
// The most often the renderer reads the picture's mean light.
inline constexpr double kMeanPeriodSeconds = 0.2;

// ---- The mask tile -----------------------------------------------------------

// ONE PERIOD of the mask pattern, linear light 0..255, three bytes a texel, rows
// counted DOWN from the top (the output pass indexes it with the top-down floor(p),
// NEAREST + REPEAT). `none` is one white texel.
struct MaskTile {
    int w = 1, h = 1;
    std::vector<std::uint8_t> rgb;
};

namespace detail {

// crt.metal's placeIn: `at` placed in its period, centred on the texel.
inline float place_in(float at, float period) noexcept { return at - period * std::floor((at + 0.5f) / period); }

// crt.metal's stripesCovering: how much of the one-pixel span at `a` each third of
// the triad covers.
inline std::array<float, 3> stripes(float a, float pitch) noexcept {
    std::array<float, 3> r{};
    for (int i = 0; i < 3; ++i) {
        const float from = pitch * static_cast<float>(i) / 3.0f, to = pitch * static_cast<float>(i + 1) / 3.0f;
        r[static_cast<std::size_t>(i)] = std::clamp(std::min(a + 1.0f, to) - std::max(a, from), 0.0f, 1.0f);
    }
    return r;
}

// crt.metal's maskAt at the pixel (x, y), the spike's mask_at line for line.
inline std::array<float, 3> mask_at(Mask mask, float x, float y, float pitch, float strength) noexcept {
    const float lo = 1.0f - strength;
    const auto mixed = [lo](const std::array<float, 3>& c, float g) {
        return std::array<float, 3>{(lo + (1.0f - lo) * c[0]) * g, (lo + (1.0f - lo) * c[1]) * g,
                                    (lo + (1.0f - lo) * c[2]) * g};
    };
    switch (mask) {
        case Mask::grille: return mixed(stripes(place_in(x, pitch), pitch), 1.0f);
        case Mask::magentaGreen: {
            const float green = std::clamp(place_in(x, pitch) + 1.0f - pitch * 0.5f, 0.0f, 1.0f);
            const float magenta = 1.0f - green;
            return mixed({magenta, green, magenta}, 1.0f);
        }
        case Mask::dots: {
            const float half_row = std::max(std::floor((pitch + 1.0f) / 3.0f), 1.0f);
            const float place = place_in(y, 4.0f * half_row);
            const float a = place_in(x + (place >= 2.0f * half_row ? half_row : 0.0f), pitch);
            const float g = place_in(place, 2.0f * half_row) >= half_row ? 1.0f + (0.85f - 1.0f) * strength : 1.0f;
            return mixed(stripes(a, pitch), g);
        }
        case Mask::slot: {
            const float height = 2.0f * pitch, place = place_in(y, 2.0f * height);
            const float a = place_in(x + (place >= height ? std::floor((pitch + 1.0f) * 0.5f) : 0.0f), pitch);
            const float bridge = std::clamp(place_in(place, height) + 1.0f - (height - pitch / 6.0f), 0.0f, 1.0f);
            return mixed(stripes(a, pitch), 1.0f - bridge * 0.2f * strength);
        }
        case Mask::none: break;
    }
    return {1.0f, 1.0f, 1.0f};
}

}  // namespace detail

// The tile for `mask` at `pitch` whole pixels a triad (the still's maskPitch) and
// `strength` (the still's maskStrength). One period across (`pitch`) and down: one row
// for the stripes, four half-rows for the dots, two slot heights for the slots.
inline MaskTile mask_tile(Mask mask, int pitch, float strength) {
    MaskTile t;
    if (mask == Mask::none) {
        t.rgb = {255, 255, 255};
        return t;
    }
    pitch = std::max(pitch, 1);
    const auto p = static_cast<float>(pitch);
    t.w = pitch;
    if (mask == Mask::dots) t.h = 4 * std::max(static_cast<int>(std::floor((p + 1.0f) / 3.0f)), 1);
    if (mask == Mask::slot) t.h = 4 * pitch;
    t.rgb.resize(static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h) * 3);
    std::size_t i = 0;
    for (int y = 0; y < t.h; ++y)
        for (int x = 0; x < t.w; ++x)
            for (const float c : detail::mask_at(mask, static_cast<float>(x), static_cast<float>(y), p, strength))
                t.rgb[i++] = static_cast<std::uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
    return t;
}

// ---- The brightness limiter --------------------------------------------------

// ABL: the picture's mean light, smoothed on the frames' own clock. Sound Factory's
// readMean (on_mean) and stepBrightnessLimitTo (step).
class AblLimiter {
public:
    // A new reading, linear luma 0..1 (clamped; a NaN is ignored). The first one is
    // where the limiter starts, not a step it travels: the first picture is not news.
    void on_mean(double light) noexcept {
        if (!(light == light)) return;
        light = std::clamp(light, 0.0, 1.0);
        if (std::abs(target_ - level_) <= kSettledWithin) from_rest_ = true;
        target_ = light;
        if (!measured_) {
            measured_ = true;
            level_ = light;
        }
    }

    // One frame at `now`. FROM REST THE JOURNEY STARTS WITH THIS FRAME: with nothing
    // to travel the GPU rests, so the last frame may be long gone, and counting that
    // wait as time travelled would land a new picture's light all at once. Otherwise
    // the time since the last step, at most a second.
    void step(double now) noexcept {
        const double elapsed = from_rest_ ? 1.0 / kSettleFramesPerSecond : std::clamp(now - clock_, 0.0, 1.0);
        from_rest_ = false;
        clock_ = now;
        level_ += (target_ - level_) * (1.0 - std::exp(-elapsed / kAblSeconds));
    }

    double level() const noexcept { return level_; }
    double target() const noexcept { return target_; }
    // Still to travel, signed (target - level): settle_frames() takes it.
    double distance() const noexcept { return target_ - level_; }
    bool measured() const noexcept { return measured_; }

private:
    double target_ = 0.0, level_ = 0.0, clock_ = 0.0;
    bool measured_ = false, from_rest_ = false;
};

// ---- The driver --------------------------------------------------------------

// What the main loop needs before deciding to draw: the frame class (the pacing) and
// whether a frame is owed with nothing else changing.
struct DriverPeek {
    FrameClass cls;
    bool moving;
};

// One frame. `u` is ready to upload; `plan`, `sizes` and `glow_sigma` are the
// still's (rebuilt only when `still_changed`). `glass_changed`: rebake the glass
// (~4 ms). `mask_changed`: upload `mask`, whose pixels are present only then.
// `moving`: another frame is owed even if nothing else changes.
struct DriverFrame {
    TubeUniforms u;
    Resolved resolved;
    PassPlan plan;
    TargetSizes sizes;
    float glow_sigma = 0.0f;
    MaskTile mask;
    bool still_changed = false;
    bool glass_changed = false;
    bool mask_changed = false;
    bool moving = false;
};

class FrameDriver {
public:
    // The tube starts at `now` (the warm-up's clock and the tracking clock), with
    // `degauss_requests` as the baseline: only a rise above it degausses. Everything
    // cached is dropped, so the next frame rebuilds the still, bakes the glass and
    // hands over the mask. frame() without a start() starts at its own `now`.
    void start(double now, std::uint64_t degauss_requests) {
        *this = FrameDriver{};
        started_ = true;
        start_ = last_ = tracking_start_ = now;
        degauss_seen_ = degauss_requests;
    }

    // What frame() would say about class and motion at `now`, changing nothing. A
    // setting or picture not yet drawn, a degauss not yet started or a tube not yet
    // started all owe a frame, so they read as moving.
    DriverPeek peek(const TubeSetting& setting, const Picture& picture, double now,
                    std::uint64_t degauss_requests) const {
        if (!started_ || !have_still_ || still_differs(setting, picture))
            return {plan_passes(resolve(setting), picture).cls, true};
        return {plan_.cls, degauss_requests > degauss_seen_ || moving_at(now)};
    }

    DriverFrame frame(const TubeSetting& setting, const Picture& picture, double now,
                      std::uint64_t degauss_requests) {
        if (!started_) start(now, degauss_requests);
        DriverFrame out;

        // 1. The still: only when the setting or the picture changed.
        out.still_changed = !have_still_ || still_differs(setting, picture);
        if (out.still_changed) rebuild_still(setting, picture, now);

        // 2. The glass: only for its own inputs.
        const GlassKey glass = glass_key(still_);
        out.glass_changed = !have_glass_ || glass != glass_key_;
        glass_key_ = glass;
        have_glass_ = true;

        // 3. The mask tile: only for pattern, pitch or strength.
        const MaskKey mask = mask_key(still_);
        out.mask_changed = !have_mask_ || mask != mask_key_;
        if (out.mask_changed) {
            const Mask kind = picture_.crt_native ? Mask::none : resolved_.mask;   // as tube_uniforms_for
            mask_ = mask_tile(kind, static_cast<int>(std::lround(still_.maskPitch)), still_.maskStrength);
            mask_key_ = mask;
            have_mask_ = true;
        }

        // 4. The phases: this drawn frame's dt (advance_character_phases clamps it).
        advance_character_phases(phases_, resolved_, now - last_);
        last_ = now;

        // 5. Degauss: the op log's counter rose. A counter that went backwards (the log
        //    restarted) is only a new baseline.
        if (degauss_requests != degauss_seen_) {
            if (degauss_requests > degauss_seen_) degauss_start_ = now;
            degauss_seen_ = degauss_requests;
        }

        // 6. The limiter, and the settle debt it and the still owe (measured before this
        //    frame's step, as Sound Factory's owesSettling is, and counting this frame).
        if (out.still_changed || mean_new_) settle_debt_ = settle_frames(still_, abl_.distance());
        mean_new_ = false;
        if (plan_.mean_probe) abl_.step(now);

        // 7. FrameState, as TubeRenderer.mm encodeTubeInto (lines 937-958).
        FrameState fs;
        fs.seconds = now;
        fs.frame = frame_count_++;
        fs.tracking_elapsed = now - tracking_start_;
        fs.motion_amount = (now - start_ - kWarmupMovesSeconds) / 0.5;
        fs.rising_light = plan_.mean_probe ? std::max(0.0, abl_.distance()) : 0.0;
        fs.ablLevel = plan_.mean_probe ? static_cast<float>(abl_.level()) : 0.0f;
        fs.warmupGain = now - start_ < kWarmupSeconds ? static_cast<float>(warmup_gain(now - start_)) : 1.0f;
        fs.degauss = static_cast<float>(std::max(0.0, 1.0 - (now - degauss_start_) / kDegaussSeconds));
        fs.phases = phases_;
        out.u = tube_frame(still_, resolved_, fs);

        // 8. This frame paid one of the debt.
        settle_debt_ = std::max(0, settle_debt_ - 1);

        out.resolved = resolved_;
        out.plan = plan_;
        out.sizes = sizes_;
        out.glow_sigma = glow_sigma_;
        out.mask.w = mask_.w;
        out.mask.h = mask_.h;
        if (out.mask_changed) out.mask.rgb = mask_.rgb;   // the only frame that copies the pixels
        out.moving = moving_at(now);
        return out;
    }

    // A reading of the picture's mean light (linear luma), from the probe the renderer
    // ran. Ignored when the current plan has no probe.
    void on_mean(double light) noexcept {
        if (!have_still_ || !plan_.mean_probe) return;
        abl_.on_mean(light);
        mean_new_ = true;
    }

    // Whether the renderer should run the probe and read a mean this frame: only on a
    // plan with the probe, at most once every kMeanPeriodSeconds.
    bool mean_due(double now) const noexcept {
        return started_ && have_still_ && plan_.mean_probe && now - mean_requested_ >= kMeanPeriodSeconds;
    }
    void note_mean_requested(double now) noexcept { mean_requested_ = now; }

    // The current mask tile, pixels included, for a renderer that must upload it again.
    const MaskTile& mask() const noexcept { return mask_; }

private:
    using GlassKey = std::array<float, 8>;
    using MaskKey = std::array<float, 3>;

    static GlassKey glass_key(const TubeUniforms& u) noexcept {
        return {u.cornerPx, u.marginPx, u.rim, u.vignette, u.glassBlack, u.reflection, u.outW, u.outH};
    }
    static MaskKey mask_key(const TubeUniforms& u) noexcept {
        if (u.maskType == 0.0f) return {0.0f, 0.0f, 0.0f};   // none: pitch and strength draw nothing
        return {u.maskType, u.maskPitch, u.maskStrength};
    }

    bool still_differs(const TubeSetting& setting, const Picture& picture) const noexcept {
        return setting != setting_ || picture.w != picture_.w || picture.h != picture_.h ||
               picture.crt_native != picture_.crt_native;
    }

    void rebuild_still(const TubeSetting& setting, const Picture& picture, double now) {
        const Resolved resolved = resolve(setting);
        if (have_still_) {
            for (const Effect e : {Effect::tracking, Effect::trackingRate, Effect::trackingWidth,
                                   Effect::trackingRecovery})
                if (resolved.effects[index_of(e)] != resolved_.effects[index_of(e)]) {
                    tracking_start_ = now;
                    break;
                }
        }
        const bool had_probe = have_still_ && plan_.mean_probe;
        setting_ = setting;
        picture_ = picture;
        resolved_ = resolved;
        plan_ = plan_passes(resolved_, picture_);
        still_ = tube_uniforms_for(resolved_, picture_);
        apply_plan(still_, plan_);
        sizes_ = target_sizes(plan_, picture_, still_);
        glow_sigma_ = plan_.glow ? glow_sigma_px(resolved_, picture_) : 0.0f;
        have_still_ = true;
        // Without the probe the limiter rests: neutral now, and a probe that returns
        // starts it again from its first reading.
        if (!plan_.mean_probe || !had_probe) {
            abl_ = AblLimiter{};
            mean_new_ = false;
        }
    }

    bool moving_at(double now) const noexcept {
        return moves_on_its_own(resolved_) || tracking_moving(resolved_, now - tracking_start_) ||
               now - start_ < kWarmupMovesSeconds || now - degauss_start_ < kDegaussMovesSeconds ||
               settle_debt_ > 0;
    }

    bool started_ = false, have_still_ = false, have_glass_ = false, have_mask_ = false, mean_new_ = false;
    double start_ = 0.0, last_ = 0.0, tracking_start_ = 0.0;
    double degauss_start_ = -std::numeric_limits<double>::infinity();
    double mean_requested_ = -std::numeric_limits<double>::infinity();
    std::uint64_t degauss_seen_ = 0, frame_count_ = 0;
    TubeSetting setting_{};
    Picture picture_{0.0, 0.0, false};
    Resolved resolved_{};
    TubeUniforms still_{};
    PassPlan plan_{};
    TargetSizes sizes_{};
    float glow_sigma_ = 0.0f;
    GlassKey glass_key_{};
    MaskKey mask_key_{};
    MaskTile mask_{};
    CharacterPhases phases_{};
    AblLimiter abl_{};
    int settle_debt_ = 0;
};

}  // namespace tube
