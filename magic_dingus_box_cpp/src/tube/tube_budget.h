// tube_budget.h - the measured "lean set", the two time budgets and the pass prices
// the Tube renderer's frame class is decided from (design section 3, "Frame class").
//
// A setting draws at 60 fps ("lean") only when every row that needs a pass the lean
// path does not have is at its neutral value; otherwise it draws at 30 fps ("rich").
// The lean path is the richest configuration the Pi 5 held 99 % of frames on in
// Task 1b's round-3 pricing (docs/engineering/measurements/2026-11-15-tube-lean-pricing.txt,
// DECISION block): bend + cheap beam + tone + displace. The motion block (noise, flicker,
// hum, rollBand, purity) and the probe (ABL, breathing) did not hold, so they force
// 30 fps for now; splitting the motion block and a probe that does not stall are
// follow-up measurements that would move rows back to 60 fps.
//
// DRIVER HAZARD (measured, Task 5 builds on it): the Pi 5's shader compiler (Mesa V3D)
// hung, 4 runs of 4, on a signal program that has chroma bleed AND convergence compiled
// in WITHOUT the sharpness path. The program table must therefore never contain that
// combination. Source: docs/engineering/measurements/2026-11-15-tube-hang-isolation.txt.
//
// Pure: standard library only, no I/O.
#pragma once

#include <array>

#include "tube/tube_rows.h"

namespace tube {

// Which ingredients the lean path has. `usable` false means no lean path at all.
struct LeanSet {
    bool usable, bend, beam, tone, displace, motion, probe, cheap_beam;
};

// from Task 1b's table
inline constexpr LeanSet kLeanSet{true, true, true, true, true, false, false, true};

// Frame-time budgets, ms at 1440x1080.
// from Task 1b's table. The budget is the measured total of the shipped lean set
// (12.27 ms) minus a 0.5 ms guard band, by the plan's rule: it is NOT comparable to
// kPassCosts.out_lean (12.3), and a "sum of costs <= budget" check would be wrong.
inline constexpr double kLeanBudgetMs = 11.8;
// THE RICH BUDGET: the largest PREDICTED cost (predicted_ms) a rich plan may have; over
// it, plan_passes() drops afterglow, then glow. Kiosk-measured on the Pi 5
// (docs/engineering/measurements/2026-11-15-tube-budgets-pi5.txt), pacing with the GPU
// timers off, late presents of ~1800 a minute:
//   22.94 ms  Arcade, window 4            6 and 13 late    99.3-99.7 % on time: holds 30 fps
//   22.62 ms  Trinitron, window 4         5 and 11 late    99.4-99.7 %
//   23.47 ms  tv -glow, window 2 (sec. 4) 25 late of 1780  98.6 %
//   24.4-24.5 TV, Everything, window 4    draw at 20 fps (203 and 205 frames in 10 s)
// 23.1 sits between the heaviest setting measured to hold 99 % and the ~23.5 ms one that
// did not, and keeps the looks that held on the right side with the prediction's own
// error (at most ~0.2 ms, see kPassCosts below).
inline constexpr double kRichBudgetMs = 23.1;
inline constexpr bool kRichBeamCheap = false;   // the acceptance ladder may flip it

// Pass prices, ms at 1440x1080 on the Pi 5. Unless a field says otherwise each is the
// KIOSK's own GPU timers (MDB_TUBE_GPU_TIMING=1, 10 s averages) in window 4, binary
// b5f67d2, a 960x720 H.264 video playing: section 7 of
// docs/engineering/measurements/2026-11-15-tube-budgets-pi5.txt. "derived" marks a price
// worked out from measured ones; "earlier round" one this window did not re-measure.
//
// How predicted_ms() composes them: each pass the plan runs, at the width it runs at;
// the output at its afterglow-on price (drawn INTO the history) plus the history copy
// pass, or at its to-screen price without afterglow. The output costs MORE to the screen
// than into the history (10.66 vs 10.09 ms on the signal looks, 14.84 vs 13.97 on the
// raster ones), so dropping afterglow saves the copy pass minus that difference.
struct SignalPrice {
    double full, half;   // the signal program at the picture's width, and at half of it
};
struct OutputPrice {
    double to_screen, into_history;
};
struct PassCosts {
    double quarter;
    double glow_blur;                   // the glow's two blur passes at the quarter's size (a narrow glow)
    double glow_blur_half_base;         // ... on the half-size targets (a wide glow):
    double glow_blur_half_per_radius;   //     base + per_radius * the blur's radius (blur_params)
    SignalPrice sig_sharp, sig_sharp_chroma, sig_guns, sig_guns_chroma;
    double lines;                       // the lines pass (always at the signal's half width: plan_for)
    double out_lean;
    OutputPrice out_rich;               // out_rich_signal (and out_rich_scene, see below)
    OutputPrice out_rich_raster, out_rich_raster_only;
    double history;                     // the afterglow's copy pass (copy_encode)
    double probe;
};

// Signal at half width / at full width, from an earlier round (Task 1b's table: a
// signal program measured at both widths). Used ONLY to derive the widths window 4 did
// not measure.
inline constexpr double kHalfWidthFactor = 0.56;

namespace detail {
// window 4: sharp at full width (PVM 5.24, Monitor 5.23); guns at half width (Trinitron
// 3.90; tv -chroma 3.89, everything -chroma 3.84); guns + chroma at half width (TV 5.28,
// Arcade 5.25, Everything 5.25; the -afterglow rows 5.23, 5.22): 5.26, their mean.
inline constexpr double kSigSharpFull = 5.24, kSigGunsHalf = 3.90, kSigGunsChromaHalf = 5.26;
// What chroma bleed adds to the signal at half width: guns + chroma less guns (derived).
inline constexpr double kChromaHalf = kSigGunsChromaHalf - kSigGunsHalf;
}  // namespace detail

inline constexpr PassCosts kPassCosts{
    0.64,   // quarter: 0.64 on five looks, 0.65 on TV
    1.64,   // glow_blur: window 2 (binary 1f8cdc4, the full-size chain, unchanged since): Monitor at
            // GLOW SIZE 10 and every "glowSize10" row, 1.64-1.66. Sigma 1.92 quarter px, radius 6:
            // the largest a narrow glow (sigma < kGlowHalfSigma) can be, so the upper end for every
            // glow drawn at this size
    0.12,   // glow_blur_half_base, and
    0.062,  // glow_blur_half_per_radius: the least-squares line through window 4's five half-size
            // blurs against their radius (Monitor r3 0.31, PVM r5 0.43, Trinitron r6 0.49, TV and
            // Everything r8 0.62, Arcade r9 0.68; the line is within 0.01 of each). Each blur
            // pass reads 2r+1 taps a pixel, so the time follows the radius; a constant would
            // misprice Monitor's and Arcade's by 0.2-0.3 ms
    {detail::kSigSharpFull, detail::kSigSharpFull * kHalfWidthFactor},   // sig_sharp: full measured; half derived (2.93)
    {(detail::kSigSharpFull * kHalfWidthFactor + detail::kChromaHalf) / kHalfWidthFactor,
     detail::kSigSharpFull * kHalfWidthFactor + detail::kChromaHalf},    // sig_sharp_chroma: derived (half 4.29:
            // sharp's half plus chroma's half-width increment). NOT measured in any window. Its
            // full width is never drawn (chroma always runs at half width: plan_for)
    {detail::kSigGunsHalf / kHalfWidthFactor, detail::kSigGunsHalf},     // sig_guns: half measured; full derived (6.96)
    {detail::kSigGunsChromaHalf / kHalfWidthFactor, detail::kSigGunsChromaHalf},   // sig_guns_chroma: half
            // measured; full derived and never drawn (chroma always runs at half width)
    1.07,   // lines: the mean of window 4's six (Trinitron 1.02, TV 1.13, Arcade 1.16, Everything
            // 1.12, tv -afterglow 0.99, everything -afterglow 1.00)
    12.3,   // out_lean: earlier round (round 3, 2026-11-15-tube-lean-pricing.txt): the shipped lean
            // set (8.85 base + bend 0.47 + cheap beam 1.31 + tone 0.61 + displace 1.03)
    {10.66, 10.09},   // out_rich: to the screen Monitor 10.66; into the history PVM 10.09. The SCENE
            // output (out_rich_scene*, no signal pass) was NOT measured in this window and is
            // priced as the signal one
    {14.84, 13.97},   // out_rich_raster: to the screen tv -afterglow 14.84, everything -afterglow 14.83;
            // into the history the mean of Trinitron 13.90, TV 14.01, Everything 14.00
    {12.54 + 0.44, 12.54},   // out_rich_raster_only: into the history Arcade 12.54. To the screen
            // NOT measured in window 4: DERIVED as 12.54 plus this program's own measured
            // difference in window 2 (binary 1f8cdc4, same output program: arcade -afterglow 13.00
            // against arcade 12.56) = 12.98. Window 4's raster difference (0.87) would give 13.41
    2.49,   // history: the mean of window 4's five (PVM 2.49, Trinitron 2.43, TV 2.54, Arcade 2.45,
            // Everything 2.55)
    0.23,   // probe: 0.23 on five looks, 0.24 on TV
};

// HOW CLOSE THE PREDICTION IS (pinned by a test, tube programs: "the kiosk's totals"):
// within 0.02 ms of PVM 19.11, Monitor 17.08 and Arcade 22.94, and of 0.10-0.14 of the
// two -afterglow totals (TV 22.56, Everything 22.52), but Trinitron predicts 22.79 for
// 22.62 and TV 24.28 for 24.47. No set of per-pass prices does better than about
// 0.18 ms on both: those two run the SAME lines, raster output and history programs,
// and TV's measured 0.11 ms more on each of the three (it drew at 20 fps, Trinitron at
// 30). The errors fall away from the budget on both sides that matter: the four looks
// that hold 30 fps all predict under 23.1, TV and Everything over it, and both of them
// under it once afterglow is dropped.

// The rows that always need a rich pass, whatever the lean set. One per line:
// Task 13's Python check reads this as text.
inline constexpr std::array<Effect, 7> kAlwaysRich{
    Effect::glow,
    Effect::afterglow,
    Effect::sharpness,
    Effect::convergence,
    Effect::chromaBleed,
    Effect::raster,
    Effect::colorDrift,
};

}  // namespace tube
