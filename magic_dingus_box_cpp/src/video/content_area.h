#pragma once

// The content rectangle a kiosk frame draws into, in two coordinate spaces:
//
//   content          PHYSICAL framebuffer pixels: glViewport and the
//                    video's area.
//   logical_content  the UI's LOGICAL canvas: Renderer::set_content_viewport,
//                    which overwrites the renderer's projection size.
//
// Modern TV draws the playlist/guide UI into a centred 4:3 area of the
// output (the deliberate CRT look); a full-screen film and the Media
// Browser (modern 16:9 content) use the whole output, and CRT Native always
// uses the whole framebuffer (shown as 4:3 by the set). The two rects
// diverge at 1920x1080: physical 1440x1080, logical 960x720 — feeding the
// physical size to set_content_viewport would draw every menu element at
// 2/3 of its intended size. Both rects use a TOP-LEFT origin; glViewport's
// bottom-left y is derived where it is called.
//
// main.cpp's render loop and the Tube engine must BOTH take the rectangle
// from here: it was computed inline in main.cpp and moved, unchanged, so the
// tube's picture cannot land somewhere the video and UI do not.
//
// Pure and header-only (tests/video/test_content_area.cpp).

#include "video/picture_placement.h"

namespace video {

struct ContentRects {
    IRect content;
    IRect logical_content;
    bool letterbox_active;
};

// modern_tv: DisplayMode::MODERN_TV. full_screen_film: app::movie_full_screen.
// media_browser: the Media Browser owns the screen (always false in a build
// without it). fb_*: the framebuffer (the DRM mode). canvas_*: the UI's
// logical canvas (config::display::logical_canvas).
inline ContentRects content_rects(bool modern_tv, bool full_screen_film, bool media_browser,
                                  int fb_w, int fb_h, int canvas_w, int canvas_h) {
    const bool letterbox_active = modern_tv && !full_screen_film && !media_browser;
    if (!letterbox_active) {
        return ContentRects{IRect{0, 0, fb_w, fb_h}, IRect{0, 0, canvas_w, canvas_h}, false};
    }
    // Integer-exact at both resolutions: 720 -> 960x720, 1080 -> 1440x1080
    // (physical and logical alike).
    return ContentRects{four_by_three_rect(fb_w, fb_h), four_by_three_rect(canvas_w, canvas_h),
                        true};
}

}  // namespace video
