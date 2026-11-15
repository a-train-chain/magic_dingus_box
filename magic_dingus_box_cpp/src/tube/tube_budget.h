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
inline constexpr double kRichBudgetMs = 23.1;   // from Task 1b's table
inline constexpr bool kRichBeamCheap = false;   // the acceptance ladder may flip it

// Pass prices, ms at 1440x1080 on the Pi 5 (from Task 1b's table unless noted).
struct PassCosts {
    double quarter, glow_blur, sig_sharp, sig_sharp_chroma, sig_guns, sig_guns_chroma,
        half_width_factor, lines_half, lines_full, out_lean, out_rich, out_rich_raster,
        out_rich_raster_only, history, probe;
};

inline constexpr PassCosts kPassCosts{
    0.7,   // quarter: an earlier round, not re-measured in round 3
    3.5,   // glow_blur: quarter + glow together measured 2.3-4.2; this is the upper end
    4.5,   // sig_sharp
    7.1,   // sig_sharp_chroma (provisional): NOT measured
    6.8,   // sig_guns: measured 3.8 at half width, stored divided by half_width_factor
    9.1,   // sig_guns_chroma: measured 5.1 at half width, stored divided by half_width_factor
    0.56,  // half_width_factor
    1.0,   // lines_half
    2.1,   // lines_full
    12.3,  // out_lean: the shipped lean set (8.85 base + bend 0.47 + cheap beam 1.31 + tone 0.61 + displace 1.03)
    9.0,   // out_rich: drawn to the screen
    11.2,  // out_rich_raster
    10.1,  // out_rich_raster_only (provisional): NOT measured
    2.4,   // history: the NET cost of afterglow = output into the history 7.1 + history pass 4.3 - 9.0
    0.02,  // probe
};

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
