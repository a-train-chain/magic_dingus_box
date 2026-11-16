#pragma once

// What a drawing tube asks of the kiosk's main loop (design section 4).
// Pure, header-only, unit-tested on the Mac (tests/app/test_tube_redraw.cpp).
//
//   field_rate_motion  Lean60 motion: the menu is drawn at field rate, the
//                      same path the CRT time effects use
//                      (MainMenuActivity::crt_time_effects).
//   continuous_motion  Rich30 motion: the screen is not static
//                      (MainMenuActivity::tube_rich_motion) — every
//                      iteration draws, and the flip is held instead.
//   hold_present       The frame is rendered right after the last flip and
//                      its flip is held so it lands
//                      utils::held_vblanks_at(hold_fps, hz) vblanks after
//                      the last (utils::capped_present_hold_at,
//                      utils::held_frame_sleep_at).
//                      Rich30 at >= 48 Hz: 2 vblanks at 50/60 Hz, 4 at 120
//                      Hz (below 48 Hz a rich frame already spans a vblank).
//                      Lean60 only where the loop's 60 fps cap spans two or
//                      more vblanks (utils::frame_pacing_for(60, hz) >= 2:
//                      100, 120, 144 Hz): 2 vblanks there. Without the hold
//                      the cap's post-flip sleep left a lean frame less than
//                      one refresh to render and it landed a vblank late. At
//                      50/60 Hz a lean frame is one vblank: never held.
//   hold_fps           The target rate the hold paces to (30 or 60); 0 when
//                      nothing is held.
//
// A tube that is not drawing (another engine, the Media Browser, a game)
// asks for nothing.

#include "tube/tube_frame_class.h"
#include "utils/frame_pacing.h"

namespace app {

struct TubeRedraw {
    bool field_rate_motion = false;
    bool continuous_motion = false;
    bool hold_present = false;
    int hold_fps = 0;
};

inline TubeRedraw tube_redraw(bool tube_drawing, tube::FrameClass frame_class, bool tube_moving,
                              int vrefresh_hz) noexcept {
    TubeRedraw r;
    if (!tube_drawing) return r;
    if (frame_class == tube::FrameClass::Lean60) {
        r.field_rate_motion = tube_moving;
        r.hold_present = utils::held_vblanks_at(60, vrefresh_hz) >= 2;
    } else {
        r.continuous_motion = tube_moving;
        r.hold_present = vrefresh_hz >= 48;
    }
    if (r.hold_present) r.hold_fps = tube::frames_per_second(frame_class);
    return r;
}

}  // namespace app
