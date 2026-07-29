#include "app/playback_stall_watchdog.h"

#include <cmath>

namespace app {

PlaybackStallWatchdog::Action PlaybackStallWatchdog::update(bool expect_playing,
                                                           double position_sec,
                                                           double now_sec) {
    // Not trying to play: a menu, a deliberate pause, or a game running.
    // Position is meant to be still, and "recovering" would start playback
    // nobody asked for. Drop the baseline so that when playback resumes we
    // measure from then, not from whenever it stopped.
    if (!expect_playing) {
        armed_ = false;
        return Action::None;
    }

    // Any movement at all — including backwards, which is a seek or a loop
    // back to the top of a trimmed clip — means the pipeline is alive.
    if (!armed_ || std::fabs(position_sec - last_position_) > kMovedEpsilon) {
        armed_ = true;
        last_position_ = position_sec;
        last_movement_sec_ = now_sec;
        return Action::None;
    }

    if (now_sec - last_movement_sec_ < kStallThresholdSec) {
        return Action::None;  // still within a plausible hitch
    }

    // Genuinely stalled. Rate-limit the attempts: restarting playback takes a
    // moment to have any effect, and firing every poll would stop it ever
    // succeeding.
    if (ever_recovered_ && (now_sec - last_recovery_sec_) < kRetryIntervalSec) {
        return Action::None;
    }

    ever_recovered_ = true;
    last_recovery_sec_ = now_sec;
    return Action::Recover;
}

void PlaybackStallWatchdog::reset() {
    armed_ = false;
    ever_recovered_ = false;
    last_recovery_sec_ = 0.0;
}

}  // namespace app
