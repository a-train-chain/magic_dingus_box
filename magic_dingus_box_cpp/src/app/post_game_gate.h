#pragma once

// The window between "the game is gone" and "the kiosk can take input".
// Pure logic, unit-tested on the Mac (tests/app/test_post_game_gate.cpp).
//
// WHY (Pi 5, 2026-10-03): the game-session END hook runs inside
// Controller::load_playlist_item, i.e. while the main loop is still
// blocked in the dispatch of the press that launched the game. It used to
// publish screen=playlist right there, from a status snapshot whose
// Settings fields were the PRE-LAUNCH copy (menu open, game list showing)
// — while the very next line of main.cpp force-closes that menu and the
// loop has yet to run its post-game reset (DRM master, EGL, GL resources,
// GStreamer, audio). A client trusting that status ("Settings is open,
// cursor on the game list") pressed SELECT, and the press landed on the
// MAIN menu — Master Shuffle — because the menu it was aimed at no longer
// existed. A second leak of the same class: the rest of the input batch
// that launched the game (presses made BEFORE the launch) was dispatched
// after the return, against whatever screen the kiosk came back to.
//
// The contract this gate enforces: a press is applied to the screen the
// user sees, or it is ignored — never to a different screen. So:
//   - from session end until the kiosk is ready, the published screen
//     stays "retroarch" (nothing claims a menu the kiosk cannot drive);
//   - events already polled before/while the game ran are dropped;
//   - at the ready edge, the caller drains and discards whatever input
//     queued during the reset (it was aimed at a black or frozen screen),
//     and only THEN does the live screen get published.
// "Ready" = first main-loop iteration after the display-reset block has
// run. A launch that failed before the display handover has no reset
// pending, so it becomes ready on the very next iteration.

#include "app_state.h"

namespace app {

class PostGameGate {
public:
    // Game-session END hook — every exit path, including launch failures.
    void session_ended() { settling_ = true; }

    // Between session end and the ready edge.
    bool settling() const { return settling_; }

    // Dispatch loop: may an already-polled event still be dispatched?
    // False while settling — those events predate the return.
    bool accepts_input() const { return !settling_; }

    // Once per main-loop iteration, AFTER the display-reset block. True
    // exactly once per session end: the iteration the kiosk can take input
    // again. The caller then discards queued input and clears the
    // RetroArch status fields.
    bool take_ready(bool display_reset_pending);

    // What the status file should say: the live screen, except while
    // settling, when it keeps saying RetroArch.
    ScreenMode published_screen(ScreenMode live) const {
        return settling_ ? ScreenMode::RetroArch : live;
    }

private:
    bool settling_ = false;
};

}  // namespace app
