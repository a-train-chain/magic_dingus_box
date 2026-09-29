#pragma once

namespace app {

// Detects the video pipeline silently stalling while the kiosk believes it is
// playing, and asks the caller to restart playback.
//
// This exists because of a live wedge on 2026-07-29. A burst of navigation
// input tipped a playlist switch into its timeout path; the recovery restored
// the kiosk's "I am playing" flags but left GStreamer PAUSED. Confirmed in
// PulseAudio — the kiosk's own sink-input read `Corked: yes` while the kiosk
// reported `is_paused = false`. Position sat at 0.00 for seven hours and
// nothing noticed. See tests/app/test_playback_stall.cpp.
//
// The discriminating signal is position: a video the kiosk believes is playing
// MUST advance. Everything else (corked streams, suspended sinks, paused
// pipelines) is a downstream symptom of the same thing.
//
// Kept as pure logic with an injected clock so it is testable at all — the
// real failure is only reproducible by corking an audio stream.
class PlaybackStallWatchdog {
public:
    enum class Action {
        None,
        Recover,
        // kMaxRecoveriesBeforeAdvance consecutive Recover attempts on the
        // same item never got position moving: restarting this pipeline is
        // not going to work, so the caller should move on (skip to the next
        // playlist item). The watchdog re-arms from scratch after returning
        // this, so a caller that CANNOT advance (e.g. Media Browser
        // playback) may treat it as one more Recover.
        Advance,
    };

    // expect_playing: the kiosk's own belief — video active and not paused.
    // position_sec:   whatever the player currently reports.
    // now_sec:        monotonic wall clock, seconds.
    Action update(bool expect_playing, double position_sec, double now_sec);

    // Called when a new item is loaded, so a fresh 0.0 is not mistaken for a
    // position that has stopped moving.
    void reset();

    // How long position must stand still before this is treated as a stall.
    // Above the longest legitimate hitch (loading, seeking, a decoder hiccup)
    // and far below anything a viewer would sit through.
    static constexpr double kStallThresholdSec = 3.0;

    // Minimum gap between recovery attempts. Restarting playback is not
    // instantaneous, so firing every poll would hammer play() and guarantee it
    // never takes. Long enough that a wedged box retries ~12 times a minute
    // rather than 240.
    static constexpr double kRetryIntervalSec = 8.0;

    // Recover attempts on a frozen position before escalating to Advance.
    // Before this, a wedged item was retried every 8 s forever — an
    // unattended kiosk never left it. 3 attempts = ~27 s of frozen video.
    static constexpr int kMaxRecoveriesBeforeAdvance = 3;

private:
    // Position is a double from GStreamer; compare with a tolerance rather
    // than for exact equality.
    static constexpr double kMovedEpsilon = 1e-3;

    bool armed_ = false;           // have we got a baseline to compare against?
    double last_position_ = 0.0;
    double last_movement_sec_ = 0.0;
    double last_recovery_sec_ = 0.0;
    bool ever_recovered_ = false;
    int recoveries_without_progress_ = 0;
};

}  // namespace app
