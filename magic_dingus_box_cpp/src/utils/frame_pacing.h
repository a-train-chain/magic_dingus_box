#pragma once

// Main-loop frame-rate cap. Pure logic, header-only, unit-tested on the Mac
// (tests/utils/test_frame_pacing.cpp).
//
// The kiosk's present_frame() blocks until the DRM page flip completes, so
// every iteration already ends ON a vblank. The cap therefore has to think
// in vblanks, not just milliseconds.
//
// The bug this replaces (30 fps Media Browser movie mode): the sleep was
// computed from the PREVIOUS iteration's start-to-start period. An
// iteration that had just slept 17 ms measured 33 ms -> no sleep -> the
// next one flipped one vblank later (16 ms) -> measured 16 ms -> slept
// 17 ms -> the flip after that slipped a further vblank (50 ms). Frames
// alternated 16/33/50 ms: visible stutter on movie playback, the one
// screen that is supposed to be smooth.
//
// The fix anchors the sleep to THIS iteration's present completion, which
// is a vblank: sleep just past the next (vblanks_per_frame - 1) vblanks,
// so the following flip lands exactly vblanks_per_frame vblanks after this
// one. Rendering of the next frame then has ~one vblank minus a margin of
// budget — the same budget the 60 fps path has always had, and the same
// input/video latency. A floor on the whole iteration keeps the cap
// working when the flip does NOT block (SetCrtc fallback after a failed
// page flip), where the vblank anchor does not exist.

#include <algorithm>
#include <chrono>
#include <cmath>

namespace utils {

struct FramePacing {
    // How many display refreshes each kiosk frame spans (>= 1).
    int vblanks_per_frame = 1;
    // Sleep after a present that completed on a vblank: past the
    // intermediate vblanks plus a margin, so the next flip cannot be
    // submitted early enough to catch one of them. 0 when every vblank
    // is presented.
    std::chrono::microseconds post_present_gap{0};
    // Floor on one whole iteration (start of loop -> after the sleep).
    // Only binds when the flip did not block.
    std::chrono::microseconds min_iteration{0};
};

// target_fps: the cap (60 normally, 30 during MB movie playback).
// refresh_hz: the CRTC mode's vrefresh; <= 0 (unknown) is treated as 60.
inline FramePacing frame_pacing_for(int target_fps, int refresh_hz) {
    if (refresh_hz <= 0) refresh_hz = 60;
    if (target_fps <= 0) target_fps = refresh_hz;
    FramePacing p;
    // 60/30 -> 2; 50/30 -> 2 (25 fps — 50 Hz cannot do 30 evenly);
    // 30/30, 24/30 -> 1; 60/60 -> 1; 120/60 -> 2.
    p.vblanks_per_frame = std::max(
        1, static_cast<int>(std::lround(static_cast<double>(refresh_hz) /
                                        static_cast<double>(target_fps))));
    const std::chrono::microseconds period{1000000 / refresh_hz};
    // An eighth of a refresh (~2 ms at 60 Hz) absorbs wakeup jitter on
    // the far side of the intermediate vblank.
    const std::chrono::microseconds margin = period / 8;
    if (p.vblanks_per_frame > 1) {
        p.post_present_gap = period * (p.vblanks_per_frame - 1) + margin;
    }
    p.min_iteration = period * p.vblanks_per_frame - margin;
    return p;
}

// How long to sleep at the bottom of the loop, right after present_frame()
// returned. iteration_elapsed = start of this iteration -> now.
inline std::chrono::microseconds frame_cap_sleep(
        const FramePacing& p, std::chrono::microseconds iteration_elapsed) {
    const auto floor_sleep = p.min_iteration - iteration_elapsed;
    return std::max({std::chrono::microseconds{0}, p.post_present_gap,
                     floor_sleep});
}

}  // namespace utils
