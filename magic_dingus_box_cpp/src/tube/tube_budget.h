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
// did not. The prediction's own error is up to 0.2 ms (kPassCosts below), which is MORE
// than Arcade's predicted margin: Arcade predicts 22.91, 0.19 under. Arcade is the
// fragile one: a rise of more than 0.19 ms in any single price of its plan flips it to
// dropping afterglow (history 2.49 -> 2.69, the raster-only output into the history
// 12.54 -> 12.74, guns + chroma at half width 5.26 -> 5.46, lines 1.07 -> 1.27, quarter
// 0.64 -> 0.84). Its own measured passes total 22.94, under the budget.
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

// Signal at half width / at full width: the one program measured at both widths in one
// run, guns at half width 4.03 against 7.60 at full width (the TV ladder, "+ chroma bleed 0"
// and "+ true raster 0", docs/engineering/measurements/2026-11-15-tube-programs-pi5.txt
// section 6, binary 4ad8223). Used ONLY to derive the widths no window measured. (It
// replaces 0.56, which was the plan's round-2 PREDICTION, never a measurement.)
inline constexpr double kHalfWidthFactor = 4.03 / 7.60;

namespace detail {
// window 4: sharp at full width (PVM 5.24, Monitor 5.23); guns at half width (Trinitron
// 3.90; tv -chroma 3.89, everything -chroma 3.84); guns + chroma at half width (TV 5.28,
// Arcade 5.25, Everything 5.25; the -afterglow rows 5.23, 5.22): 5.26, their mean.
inline constexpr double kSigSharpFull = 5.24, kSigGunsHalf = 3.90, kSigGunsChromaHalf = 5.26;
// Guns at full width: TV with chroma bleed 0 and true raster 0 (convergence 33 and
// sharpness 40 on: sig_guns), section 6 of 2026-11-15-tube-programs-pi5.txt, binary
// 4ad8223. That run read 4.03 at half width where window 4 reads 3.90, so this is likely
// ~3 % high for today's binary; kept as measured.
inline constexpr double kSigGunsFull = 7.60;
// What chroma bleed adds to the signal at half width: guns + chroma less guns (derived).
inline constexpr double kChromaHalf = kSigGunsChromaHalf - kSigGunsHalf;
}  // namespace detail

inline constexpr PassCosts kPassCosts{
    0.64,   // quarter: 0.64 on five looks, 0.65 on TV
    1.64,   // glow_blur: window 2 (binary 1f8cdc4, the full-size chain, unchanged since): Monitor at
            // GLOW SIZE 10 and every "glowSize10" row, 1.64-1.66, at sigma 1.92 quarter px, radius 6.
            // (Sigma 1.92 is itself WIDE at 1440 wide; it was measured on the full-size chain before
            // the half-size one existed.) A narrow glow (sigma < kGlowHalfSigma = 1.9) has a radius
            // of at most ceil(3 x 1.9) = 6, so 1.64 is the upper end for every glow drawn full size
    0.12,   // glow_blur_half_base, and
    0.062,  // glow_blur_half_per_radius: the least-squares line through window 4's five half-size
            // blurs against their radius (Monitor r3 0.31, PVM r5 0.43, Trinitron r6 0.49, TV and
            // Everything r8 0.62, Arcade r9 0.68; the line is within 0.01 of each). Each blur
            // pass reads 2r+1 taps a pixel, so the time follows the radius; a constant would
            // misprice Monitor's and Arcade's by 0.2-0.3 ms
    {detail::kSigSharpFull, detail::kSigSharpFull * kHalfWidthFactor},   // sig_sharp: full measured; half derived (2.78)
    {(detail::kSigSharpFull * kHalfWidthFactor + detail::kChromaHalf) / kHalfWidthFactor,
     detail::kSigSharpFull * kHalfWidthFactor + detail::kChromaHalf},    // sig_sharp_chroma: derived (half 4.14:
            // sharp's half plus chroma's half-width increment). NOT measured in any window. Its
            // full width is never drawn (chroma always runs at half width: plan_for)
    {detail::kSigGunsFull, detail::kSigGunsHalf},     // sig_guns: both measured (full in an earlier binary, above)
    {detail::kSigGunsChromaHalf / kHalfWidthFactor, detail::kSigGunsChromaHalf},   // sig_guns_chroma: half
            // measured; full derived and never drawn (chroma always runs at half width)
    1.07,   // lines: the mean of window 4's six (Trinitron 1.02, TV 1.13, Arcade 1.16, Everything
            // 1.12, tv -afterglow 0.99, everything -afterglow 1.00); its target is 720 x 270 there
            // (kReferenceLinesPixels) and it scales with that target, not the picture
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
    0.23,   // probe: 0.23 on five looks, 0.24 on TV. Its target is a fixed 64x64: never scaled
};

// HOW CLOSE THE PREDICTION IS (pinned by a test, tube programs: "the kiosk's totals"):
// within 0.01 ms of PVM 19.11 and Monitor 17.08, 0.03 of Arcade 22.94, 0.13 of Everything
// 24.41 and 0.10-0.14 of the two -afterglow totals (TV 22.56, Everything 22.52), but
// Trinitron predicts 22.79 for 22.62 (+0.17) and TV 24.28 for 24.47 (-0.19). No set of
// per-pass prices does better than about 0.18 ms on both: those two run the SAME lines,
// raster output and history programs, and TV measured 0.11 ms more on each of the three
// (it drew at 20 fps, Trinitron at 30). Both of those errors point TOWARD the budget
// (Trinitron is predicted heavier, TV lighter), yet every shipped look still lands on
// its side of 23.1: the four that hold 30 fps predict under it (Arcade 22.91 the
// closest, see kRichBudgetMs), TV and Everything over it, and both under it once
// afterglow is dropped (22.66).

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
