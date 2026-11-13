#pragma once

// Channel Guide style: "nothing playing" is never a resting state. Every
// path that leaves the main screen with no video — power-on (the intro ends,
// there is no intro file, it fails to load, it times out), the post-game
// ready edge, leaving the Media Browser, closing Settings, a failed item's
// scheduled slot ending, a give-up — is caught by ONE rule evaluated every
// frame, not a hook per path (a hook per path is how one gets missed).
//
// Pure logic, no AppState, no I/O; unit-tested on the Mac
// (tests/app/test_auto_tune.cpp). main.cpp gathers the inputs once per
// frame and, on a result, calls PlaylistPlayback::tune(channel,
// /*keep_overlay=*/false): the video simply comes back, the guide is not
// opened. List style never tunes.
//
// Design: docs/superpowers/specs/2026-11-12-channel-guide-design.md,
// "When nothing would be playing".

#include <optional>
#include <vector>

namespace app {

struct AutoTuneInput {
    bool guide_style = false;        // Menu Style is Channel Guide
    bool intro_complete = false;     // the boot intro is over, whichever way
    bool on_main_screen = false;     // no Media Browser, Settings or keyboard up
    bool video_active = false;       // a video plays, or the next item loads
    bool switch_pending = false;     // a switch or tune has not got its stream up
    bool loading_game = false;       // a game is launching
    bool post_game_pending = false;  // the post-game display restore is not done
    double now_utc = 0.0;            // wall clock, UTC seconds
    double wait_until_utc = 0.0;     // a failed item's slot ends (0 = no wait)
    int last_channel = -1;           // the last channel watched; -1 = none
    std::vector<bool> tunable;       // per channel (= UI row): has a schedule
    std::vector<int> gave_up;        // channels given up on, newest last
};

// The gates alone: guide style, the intro over, on the main screen, no
// video, no switch pending, no game loading, the post-game restore done,
// and now_utc >= wait_until_utc. Reads only those fields, so main.cpp fills
// last_channel / tunable / gave_up (an identity scan) only when it is true —
// never on the frames a video plays.
bool auto_tune_gates_open(const AutoTuneInput& in);

// The channel to tune now, or nullopt. Tunes only when auto_tune_gates_open.
// A channel can be picked when it has a schedule and was not given up on.
// The target, in order:
//   - the last channel watched;
//   - after a give-up, the next channel up from the most recent given-up one,
//     wrapping (re-tuning the channel that just failed repeatedly would loop);
//     when none is left, nothing — every channel that could play failed;
//   - channel 01;
//   - the lowest channel;
//   - else nothing.
std::optional<int> decide_auto_tune(const AutoTuneInput& in);

}  // namespace app
