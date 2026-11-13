#pragma once

// Where the overlay's CROP checkbox (the Picture control) goes in the
// bottom-right now-playing block. Renderer::render_footer draws it; this is
// the placement rule alone, so it is unit-tested on the Mac
// (tests/ui/test_picture_control_layout.cpp).
//
// The control sits to the LEFT of the video title, on the title's line,
// 16 px clear of it. render_footer right-aligns the title 80 px from the
// canvas edge and never truncates it, so a long title pushes the control
// left; once the control would start left of the canvas centre — the left
// half is the playlist list's — it goes on its own line instead, above the
// title, right-aligned with the block (80 px from the edge). That keeps it
// off both the title and the playlist text on either logical canvas (640
// CRT Native, 960 Modern TV). Design:
// docs/superpowers/specs/2026-11-12-video-crop-toggle-design.md, "Layout".
//
// Pure, header-only: no GL, no fonts — the caller measures.

namespace ui {

struct PictureControlPos {
    float x = 0.0f;         // left edge of the control (box, then label)
    float y = 0.0f;         // text baseline of the control's line
    bool own_line = false;  // true = on its own line above the title
};

constexpr float kPictureControlTitleGap = 16.0f;   // control's right edge → title
constexpr float kPictureControlRightInset = 80.0f; // render_footer's right inset
constexpr float kPictureControlLineGap = 6.0f;     // extra space above the title line

// canvas_w: the logical canvas width. title_x / title_baseline: where
// render_footer draws the title. control_w: the whole control's width (box,
// gap, label). line_h: the footer's line advance.
inline PictureControlPos place_picture_control(float canvas_w, float title_x,
                                               float title_baseline, float control_w,
                                               float line_h) {
    const float beside_x = title_x - kPictureControlTitleGap - control_w;
    if (beside_x >= canvas_w / 2.0f) {
        return {beside_x, title_baseline, false};
    }
    return {canvas_w - kPictureControlRightInset - control_w,
            title_baseline - line_h - kPictureControlLineGap, true};
}

}  // namespace ui
