#pragma once

// The boot intro video — kiosk half (GStreamer, EGL, GL, GPIO LEDs).
// Moved out of main.cpp verbatim: same order of side effects, same log
// lines. The pure decisions it uses (end detection, fade volume, the
// hand-off to the menu, the render-video decision) are app/intro_sequence.h,
// unit-tested on the Mac. main() calls:
//
//   boot     start_intro(find_intro_video(), ...)   — before READY=1
//   loop     tick_intro(...)                         — once per iteration,
//            after the playlist switch timeout and before the pipeline-
//            error / auto-advance ticks, as the inline code ran

#include <string>

#include "app_state.h"

namespace platform {
struct DisplayMode;
class EglContext;
class GpioManager;
}  // namespace platform
namespace video {
class GstPlayer;
class GstRenderer;
}  // namespace video

namespace app {

class Controller;

// Walk config::get_intro_search_paths() and return the first existing intro
// video as a canonical (or, failing that, absolute) path; "" when none.
// Logs every location checked to stdout.
std::string find_intro_video();

// Load and play the intro at boot, then block (up to 10 s, 50 ms steps)
// until it is playing AND its first frame is ready to draw — so the first
// thing on screen is the video, not the menu or a blank screen. The main
// loop does the actual drawing. A load failure, a timeout or an empty
// `intro_video_path` (no intro installed) skips the intro: the menu shows
// at once (intro_complete=true).
void start_intro(const std::string& intro_video_path, AppState& state,
                 Controller& controller, video::GstPlayer& player,
                 video::GstRenderer& gst_renderer,
                 const std::string& playlist_directory);

// One main-loop step of the intro: LED dance + end detection while it
// plays, then the 300 ms audio fade-out, then stop the video, clear the
// screen, stop the LEDs, wait (bounded) for the pipeline to stop and start
// the menu fade-in. No-op once the intro is over. `mode` is read live.
void tick_intro(AppState& state, Controller& controller,
                platform::GpioManager& gpio, platform::EglContext& egl,
                const platform::DisplayMode& mode);

}  // namespace app
