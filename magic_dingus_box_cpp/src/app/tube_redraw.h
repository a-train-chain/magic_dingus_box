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
//   hold_present       Rich30 at a refresh fast enough to halve: each flip
//                      is held to every second vblank (utils::present_hold,
//                      utils::held_frame_sleep). Below 48 Hz a rich frame
//                      already spans a vblank, so nothing is held.
//
// A tube that is not drawing (another engine, the Media Browser, a game)
// asks for nothing.

#include "tube/tube_frame_class.h"

namespace app {

struct TubeRedraw {
    bool field_rate_motion = false;
    bool continuous_motion = false;
    bool hold_present = false;
};

inline TubeRedraw tube_redraw(bool tube_drawing, tube::FrameClass frame_class, bool tube_moving,
                              int vrefresh_hz) noexcept {
    TubeRedraw r;
    if (!tube_drawing) return r;
    if (frame_class == tube::FrameClass::Lean60) {
        r.field_rate_motion = tube_moving;
    } else {
        r.continuous_motion = tube_moving;
        r.hold_present = vrefresh_hz >= 48;
    }
    return r;
}

}  // namespace app
