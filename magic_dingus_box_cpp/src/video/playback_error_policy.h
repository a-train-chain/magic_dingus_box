#pragma once

#include <algorithm>
#include <cstdint>

namespace video {

// What the kiosk does when the video pipeline FAILS on a playlist item —
// a GStreamer error (corrupt/truncated file, unsupported codec, decoder
// failure) or a stall the watchdog could not revive.
//
// Why this exists: an errored pipeline never reaches position >= duration
// (and an error before preroll leaves duration at 0), so the natural-end
// auto-advance could never fire, and the stall watchdog only ever re-called
// play() on the dead pipeline. An unattended kiosk froze forever on one bad
// file. The answer is to skip the item — but "skip" alone turns a playlist
// where EVERY item is broken (unplugged library drive, a batch of files in
// a codec the board cannot decode) into a tight load/fail/load loop. So
// failures are counted per consecutive run, and once the run reaches the
// budget the kiosk gives up and shows the UI instead of spinning.
//
// Keyed on GstPlayer::stream_generation() (bumped by every load_file()), so
// one latched error on one stream is acted on exactly once no matter how
// many frames observe it. Pure logic — tests/video/test_playback_error_policy.cpp.
class PlaybackErrorPolicy {
public:
    enum class Decision {
        None,     // nothing to do
        Advance,  // skip to the next item (the normal advance path)
        GiveUp,   // too many consecutive failures: stop and show the UI
    };

    // Upper bound on consecutive failures before giving up. Each failed item
    // costs roughly a second of load + preroll wait, so this caps the worst
    // case at ~10 s of flicker before the UI comes back.
    static constexpr int kMaxFailureBudget = 10;

    // An item that has played this far without an error is proof the
    // pipeline/content path works, which ends the current failure run.
    static constexpr double kHealthyPlaySec = 3.0;

    // How many consecutive failures to tolerate. A normal playlist gets one
    // attempt per item (a 2-item playlist of broken files gives up after
    // both were tried; a single-item playlist gives up at once rather than
    // reloading itself); large playlists and Master Shuffle (random picks
    // across everything) are capped at kMaxFailureBudget.
    static int failure_budget(int playlist_size, bool master_shuffle) {
        if (master_shuffle) return kMaxFailureBudget;
        return std::clamp(playlist_size, 1, kMaxFailureBudget);
    }

    // Call every frame while a playlist item owns the pipeline.
    //   generation:   the player's stream generation (which stream is this)
    //   has_error:    the player's latched error flag
    //   position_sec: the player's current position
    //   budget:       failure_budget(...)
    Decision on_frame(uint64_t generation, bool has_error,
                      double position_sec, int budget) {
        if (has_error) return on_failure(generation, budget);
        if (position_sec >= kHealthyPlaySec) consecutive_failures_ = 0;
        return Decision::None;
    }

    // A failure that is NOT a latched pipeline error (the stall watchdog
    // giving up on the current stream). Counts toward the same run so a
    // playlist of files that all wedge also stops instead of cycling.
    Decision on_failure(uint64_t generation, int budget) {
        if (has_handled_ && handled_generation_ == generation) {
            return Decision::None;  // already acted on this stream
        }
        has_handled_ = true;
        handled_generation_ = generation;
        ++consecutive_failures_;
        if (consecutive_failures_ >= std::max(budget, 1)) {
            consecutive_failures_ = 0;  // the next attempt is user-initiated
            return Decision::GiveUp;
        }
        return Decision::Advance;
    }

    int consecutive_failures() const { return consecutive_failures_; }

private:
    bool has_handled_ = false;
    uint64_t handled_generation_ = 0;
    int consecutive_failures_ = 0;
};

// Media Browser PlaybackScreen: should a latched pipeline error end the
// session? Only while the session is live — after a natural EOS latched
// (end-of-episode overlay up) or once an exit is already armed, the error
// is irrelevant. Taking this path must NEVER feed the natural-end detector:
// an errored file is not a finished one (no watched mark, no next-episode
// countdown).
inline bool mb_should_abort_on_error(bool has_error, bool eos_latched,
                                     bool exit_pending) {
    return has_error && !eos_latched && !exit_pending;
}

}  // namespace video
