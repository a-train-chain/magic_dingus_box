#pragma once

// The boot intro video — pure decision half. The kiosk-only half (finding
// the file, the blocking first-frame wait at boot, the per-frame end
// detection / fade-out / hand-off to the menu, with its GL clears and LED
// animation) is app/intro_sequence_kiosk.h. Both were moved out of
// main.cpp verbatim; this half is unit-tested on the Mac
// (tests/app/test_intro_sequence.cpp).
//
// Lifecycle, as AppState records it:
//   boot     showing_intro_video=true, intro_ready=false — the menu is not
//            drawn until the first intro frame is ready (or the wait times
//            out, which skips the intro: intro_complete=true)
//   playing  intro_video_ended() watches position/duration each frame
//   ending   intro_fading_out=true: the intro's VOLUME ramps to zero over
//            kIntroFadeOut while the menu is drawn over the last frame
//   done     controller.stop(), then hand_intro_to_menu(): the menu FADES
//            in (is_fading) from a stopped, de-indexed playback state.

#include <chrono>

#include "app_state.h"

namespace app {

// Length of the intro's audio fade-out once its end is detected.
inline constexpr std::chrono::milliseconds kIntroFadeOut{300};

// The intro has ended when the position is within 50 ms of the duration
// (tight margin so the video plays fully), or — the fallback — the player
// stopped playing after having made progress. Only meaningful once a
// duration is known (the caller gates on duration > 0).
bool intro_video_ended(double position, double duration, bool is_playing);

// Intro stream volume `elapsed` into the fade-out: `original_volume`
// scaled down linearly to 0 at kIntroFadeOut (progress clamped to [0, 1]).
double intro_fade_out_volume(double original_volume,
                             std::chrono::milliseconds elapsed);

// The playback/UI reset once the intro is stopped: video inactive, position
// and indexes cleared, and the menu fade-IN started at `now`. Deliberately
// NOT app::stop_to_menu() — that cancels fades and shows the menu at full
// alpha at once. The intro never publishes now-playing fields and never
// sets is_switching_playlist, so this subset is the complete reset for
// this path.
void hand_intro_to_menu(AppState& state,
                        std::chrono::steady_clock::time_point now);

// Whether this frame draws the video plane. During the intro phase
// (!intro_complete): while the intro (or anything) is playing, except
// during its fade-out, when the menu fades in over a black clear. After
// the intro: whenever video is active, or a playlist switch is mid-load
// and the pipeline is already playing.
bool should_render_video(const AppState& state, bool player_is_playing);

}  // namespace app
