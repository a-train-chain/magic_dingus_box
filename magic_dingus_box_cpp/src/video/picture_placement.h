#pragma once

#include <algorithm>
#include <cmath>

namespace video {

// Where playlist video is drawn and which part of the frame is sampled.
//
// Playlist video goes into "the 4:3 area": a centred 4:3 rect in Modern TV
// mode, the whole framebuffer in CRT Native mode — where a 1280x720
// framebuffer is presented as 4:3, so its pixels are NOT square. Every
// aspect here is therefore a DISPLAY aspect (width / height as seen on the
// screen), never a pixel count, and the conversion to pixels happens once,
// from the target rect and the aspect it is shown at.
//
//   crop: cover the target, centred; the overflow is not sampled. The
//         opening is ignored — a bezel covers the edges, as it always has.
//   fit:  the whole frame, as large as fits inside the opening (the bezel's
//         transparent window, or the target itself without a bezel),
//         centred; black elsewhere.
//
// Pure and header-only: test_video_unit links no src/*.cpp.

struct IRect {
    int x, y, w, h;
};

inline bool operator==(const IRect& a, const IRect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// Normalised frame coordinates of the sampled region (0..1 on each axis).
struct TexRect {
    float u0, v0, u1, v1;
};

struct Placement {
    IRect viewport;
    TexRect tex;
};

// A frame's display aspect: its pixel grid times its pixel aspect ratio
// (an anamorphic 720x480 DVD frame with PAR 32:27 shows as 16:9, not 3:2).
// 0 when anything is missing or nonsense — "unknown" to place_picture.
inline double display_aspect(int w, int h, int par_n, int par_d) {
    if (w <= 0 || h <= 0 || par_n <= 0 || par_d <= 0) return 0.0;
    return (static_cast<double>(w) * par_n) /
           (static_cast<double>(h) * par_d);
}

// True when a display aspect is within 2% of 4:3 — close enough that crop
// and fit draw nearly the same picture, so the toggle would be invisible.
inline bool is_four_by_three(double display_aspect) {
    constexpr double kFourByThree = 4.0 / 3.0;
    constexpr double kTolerance = 0.02;
    if (!(display_aspect > 0.0)) return false;
    return std::fabs(display_aspect / kFourByThree - 1.0) <= kTolerance;
}

// The crop a playlist picture is drawn with: the video's stored setting,
// except that a source KNOWN to be 4:3 is always cropped. For one, fit
// would only shrink the picture inside a bezel's opening (thin bars inside
// the frame), and neither the overlay nor the Content Manager offers a
// control for a 4:3 video — so a stored "not cropped" (a "Show everything"
// upload, a restored backup, a package) could never be undone. An unknown
// aspect (0) keeps the stored setting.
inline bool picture_crop(bool stored_crop, double display_aspect) {
    return stored_crop || is_four_by_three(display_aspect);
}

// The centred 4:3 rect inside w x h: pillarboxed when w x h is wider than
// 4:3, letterboxed when taller. Integer-exact: 1280x720 -> 960x720,
// 1920x1080 -> 1440x1080.
inline IRect four_by_three_rect(int w, int h) {
    int cw = h * 4 / 3;
    int ch = h;
    if (cw > w) {
        cw = w;
        ch = w * 3 / 4;
    }
    return IRect{(w - cw) / 2, (h - ch) / 2, cw, ch};
}

// A rect measured in an image of img_w x img_h (the 1920x1080 bezel art)
// mapped to a framebuffer of fb_w x fb_h. Edges are rounded to the nearest
// pixel, so the rect's right/bottom edges land where the scaled ones do.
inline IRect scale_image_rect(IRect r, int img_w, int img_h, int fb_w,
                              int fb_h) {
    if (img_w <= 0 || img_h <= 0) return IRect{0, 0, 0, 0};
    const double sx = static_cast<double>(fb_w) / img_w;
    const double sy = static_cast<double>(fb_h) / img_h;
    const int x0 = static_cast<int>(std::lround(r.x * sx));
    const int y0 = static_cast<int>(std::lround(r.y * sy));
    const int x1 = static_cast<int>(std::lround((r.x + r.w) * sx));
    const int y1 = static_cast<int>(std::lround((r.y + r.h) * sy));
    return IRect{x0, y0, x1 - x0, y1 - y0};
}

// The part of `a` inside `b`; an empty rect (w or h 0) when they do not
// overlap. Clamps a bezel's declared opening to the 4:3 area: fit must never
// place the picture outside the target, whatever bezels.json says.
inline IRect intersect(IRect a, IRect b) {
    const int x0 = std::max(a.x, b.x);
    const int y0 = std::max(a.y, b.y);
    const int x1 = std::min(a.x + a.w, b.x + b.w);
    const int y1 = std::min(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return IRect{0, 0, 0, 0};
    return IRect{x0, y0, x1 - x0, y1 - y0};
}

// src_aspect:    the video's display aspect (pixel aspect ratio included).
// target:        the 4:3 area in framebuffer pixels.
// target_aspect: the display aspect target is shown at (4:3).
// opening:       the visible part of target (pass target with no bezel).
// An unusable aspect (not yet known, zero, infinite) draws the whole frame
// over the whole target — the picture before any of this existed.
inline Placement place_picture(double src_aspect, IRect target,
                               double target_aspect, IRect opening,
                               bool crop) {
    constexpr TexRect kFullFrame{0.0f, 0.0f, 1.0f, 1.0f};
    const auto usable = [](double a) { return std::isfinite(a) && a > 0.0; };
    if (!usable(src_aspect) || !usable(target_aspect) || target.w <= 0 ||
        target.h <= 0) {
        return Placement{target, kFullFrame};
    }

    if (crop) {
        // Display-aspect ratio alone decides how much of the frame is kept;
        // the target's pixel shape does not enter into it.
        TexRect tex = kFullFrame;
        if (src_aspect > target_aspect) {
            const double kept = target_aspect / src_aspect;
            tex.u0 = static_cast<float>((1.0 - kept) / 2.0);
            tex.u1 = static_cast<float>(1.0 - (1.0 - kept) / 2.0);
        } else {
            const double kept = src_aspect / target_aspect;
            tex.v0 = static_cast<float>((1.0 - kept) / 2.0);
            tex.v1 = static_cast<float>(1.0 - (1.0 - kept) / 2.0);
        }
        return Placement{target, tex};
    }

    // An empty opening is no opening: fit inside the target instead.
    if (opening.w <= 0 || opening.h <= 0) opening = target;

    // Fit: convert the source's display aspect into framebuffer-pixel
    // aspect via the target's pixel shape, then fit inside the opening.
    const double px_per_display =
        (static_cast<double>(target.w) / target.h) / target_aspect;
    const double src_px_aspect = src_aspect * px_per_display;
    const double opening_aspect = static_cast<double>(opening.w) / opening.h;

    int w = opening.w;
    int h = opening.h;
    if (src_px_aspect > opening_aspect) {
        h = static_cast<int>(std::lround(opening.w / src_px_aspect));
    } else {
        w = static_cast<int>(std::lround(opening.h * src_px_aspect));
    }
    const IRect viewport{opening.x + (opening.w - w) / 2,
                         opening.y + (opening.h - h) / 2, w, h};
    return Placement{viewport, kFullFrame};
}

}  // namespace video
