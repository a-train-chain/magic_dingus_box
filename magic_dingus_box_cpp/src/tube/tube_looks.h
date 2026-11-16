// tube_looks.h - what each of the six CRT looks IS, value by value, plus the two
// whole-pixel sizes.
//
// Ported from Sound Factory's data/CrtLooks.h (commit 4ef74a6), number for number,
// with exactly two columns changed for a TV (lineSize and maskSize): the panel they
// were tuned on is 2048 px wide, a TV is 720p or 1080p, and a 6 px line or a 6 px
// triad is a coarse chunk there.
//   lineSize 10 = 4 px a line at 1080p (270 lines), 3 px at 720p (240 lines).
//   maskSize 10 = a 3 px triad; 40 (arcade only, its dots want to be seen) = 6 px.
//
// The table keeps one row per look, the look's name in a leading comment, so a
// script can read it with a regular expression. Columns follow tube_rows.h's
// kEffects order:
//   brightness contrast saturation white sharpness | scanlines lineSize beam raster |
//   maskStrength maskSize | glow glowSize afterglow | curvature vignette rim
//   glassBlack reflection | convergence chromaBleed noise jitter | flicker hum abl
//   breathing | rollBand rollSpeed rollWidth rollTint tracking trackingRate
//   colorDrift driftSpeed trackingWidth trackingRecovery flagging flagHeight
//   flagSpeed purity puritySize purityHue purityPosition
//
// Pure: standard library only, no I/O.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "tube/tube_rows.h"

namespace tube {

struct LookValues {
    std::array<int, kEffectCount> effects;
    Mask mask;
};

namespace detail {

inline constexpr std::array<LookValues, 6> kLookTable{{
    // clang-format off
    /* pvm */        { { { 55, 60, 58, 50, 63,   35, 10, 80,   0,   60, 10,    8, 20, 10,    70, 20, 45, 30, 15,    0,  0,   0,  0,    0,  0, 20, 15,    0, 35, 35, 50, 0, 15, 0, 25,    35, 43, 0, 35, 25, 0, 40, 75, 0 } }, Mask::grille },
    /* monitor */    { { { 52, 55, 54, 50, 60,   15, 10, 10,   0,   38, 10,    5, 10,  0,    60, 12, 35, 20, 15,    0,  0,   0,  0,    0,  0, 15,  0,    0, 35, 35, 50, 0, 15, 0, 25,    35, 43, 0, 35, 25, 0, 40, 75, 0 } }, Mask::magentaGreen },
    /* trinitron */  { { { 55, 58, 58, 58, 55,   35, 10, 70,  30,   60, 10,   14, 30, 20,    80, 25, 45, 40, 30,   17,  0,  50, 10,   20,  0, 30, 30,    0, 35, 35, 50, 0, 15, 0, 25,    35, 43, 0, 35, 25, 0, 40, 75, 0 } }, Mask::grille },
    /* tv */         { { { 58, 55, 52, 45, 40,   45, 10, 60,  60,   56, 10,   22, 40, 30,   100, 30, 50, 50, 40,   33, 50, 100, 30,   30, 60, 50, 45,    0, 35, 35, 50, 0, 15, 0, 25,    35, 43, 0, 35, 25, 0, 40, 75, 0 } }, Mask::slot },
    /* arcade */     { { { 60, 58, 60, 50, 45,   65, 10, 90, 100,   62, 40,   28, 45, 30,   100, 30, 50, 50, 30,   27, 30, 100, 20,   20, 20, 45, 40,    0, 35, 35, 50, 0, 15, 0, 25,    35, 43, 0, 35, 25, 0, 40, 75, 0 } }, Mask::dots },
    /* everything */ { { { 58, 58, 56, 48, 43,   50, 10, 70,  60,   56, 10,   25, 40, 40,   100, 32, 50, 50, 40,   33, 50, 100, 20,   40, 80, 50, 60,    0, 35, 35, 50, 0, 15, 0, 25,    35, 43, 0, 35, 25, 0, 40, 75, 0 } }, Mask::slot },
    // clang-format on
}};

static_assert(kLookTable.size() == kLooks.size(), "one row per look");

}  // namespace detail

// What `look` is. A value outside the enum can only come from a bad cast and
// reads as the default look: a reference has no "nothing" to return.
constexpr const LookValues& look_values(Look look) noexcept {
    const auto index = static_cast<std::size_t>(look);
    return index < detail::kLookTable.size() ? detail::kLookTable[index]
                                             : detail::kLookTable[static_cast<std::size_t>(kLookDefault)];
}

// THE TWO WHOLE-PIXEL SIZES. The mask is drawn in the display's own pixels, so its
// pitch snaps to a whole number of them (a fractional pitch beats against the grid
// into moire). One detent (ten row units) of MASK SIZE or SCANLINE SIZE is exactly
// one pixel. Sizes are pixels at the 1080-line reference; whole_px scales them to
// the picture actually drawn. A row is never negative (is_row_value refuses it), so
// integer division is the floor.
inline constexpr int kSizeRowStep = 10;

// MASK SIZE: pixels a triad, 2 at row 0 and 12 at row 100.
constexpr int mask_pitch_row_px(int row) noexcept { return 2 + row / kSizeRowStep; }

// SCANLINE SIZE: pixels a line, 3 at row 0 and 13 at row 100.
constexpr int line_pitch_row_px(int row) noexcept { return 3 + row / kSizeRowStep; }

// A reference size scaled to a picture `picture_h` lines tall, whole pixels, never
// under two (a one-pixel mask or line is a dither, not a pattern).
inline int whole_px(int reference_px, double picture_h) noexcept {
    return std::max(2, static_cast<int>(std::lround(reference_px * picture_h / 1080.0)));
}

}  // namespace tube
