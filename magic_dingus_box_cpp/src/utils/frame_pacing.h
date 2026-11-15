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

// CRT field rate (app::RedrawGate draws every other iteration on the
// static CRT menu): the sleep for an iteration that SKIPPED drawing.
//
// Without it a skipped iteration lasts min_iteration (just under one
// refresh) and the drawing iteration after it starts ~2 ms before the
// vblank it is meant to skip — a frame rendered in under 2 ms catches
// that vblank (16 ms frame), a slower one the next (33 ms): uneven 30 fps.
// Sleeping until just past the skipped vblank instead gives the drawing
// iteration a full refresh minus the margin to render, and its flip lands
// exactly two vblanks after the previous one — the same anchor the 30 fps
// movie cap uses, while input is still polled twice per drawn frame.
//
// since_last_present: time since the last present returned (on a vblank).
// iteration_elapsed: start of this iteration -> now.
inline std::chrono::microseconds field_rate_skip_sleep(
        const FramePacing& p, int refresh_hz,
        std::chrono::microseconds since_last_present,
        std::chrono::microseconds iteration_elapsed) {
    if (refresh_hz <= 0) refresh_hz = 60;
    const std::chrono::microseconds period{1000000 / refresh_hz};
    const auto past_skipped_vblank = period + period / 8 - since_last_present;
    return std::max(frame_cap_sleep(p, iteration_elapsed), past_skipped_vblank);
}

// Rich30 tube frames (app::TubeRedraw::hold_present): each flip is held so
// it lands round(refresh / 30) vblanks after the previous one — 2 at 50/60
// Hz, 4 at 120 Hz, 1 at 30 Hz or below (nothing to hold). <= 0 reads as 60.
inline int held_vblanks(int refresh_hz) noexcept {
    return frame_pacing_for(30, refresh_hz).vblanks_per_frame;
}

// How long to wait before the page flip of a held frame. Called AFTER
// eglSwapBuffers and BEFORE the flip is submitted, with a FRESH
// since_last_present = steady_clock::now() - the last present's completion
// (a vblank); a value read earlier in the iteration would let a slow frame
// wait for a vblank it has already passed.
//
// Why hold the flip instead of skipping an iteration (design section 4): a
// rich frame costs up to ~23 ms, more than one refresh. Skipping iterations
// the way the CRT field-rate path does gives it at most one refresh minus a
// margin to render, so a 20 ms frame would miss and the cadence would
// wobble between 2 and 3 vblanks. Rendering straight after the flip and
// holding only the FLIP gives the frame the whole two-refresh budget: the
// wait ends just past the intermediate vblank(s) plus the margin, so the
// flip lands exactly held_vblanks after the last one. A frame slower than
// that waits for nothing and flips at the next vblank (counted late).
inline std::chrono::microseconds present_hold(int refresh_hz,
                                              std::chrono::microseconds since_last_present) {
    const FramePacing p = frame_pacing_for(30, refresh_hz);
    if (p.vblanks_per_frame == 1) return std::chrono::microseconds{0};
    // post_present_gap = period * (held_vblanks - 1) + period / 8.
    return std::max(std::chrono::microseconds{0}, p.post_present_gap - since_last_present);
}

// The bottom-of-loop sleep for a held frame: ONLY the floor on the whole
// iteration (period * held_vblanks - period / 8), which binds when the flip
// did not block (SetCrtc fallback). Never a post-present gap — the hold
// already happened before the flip, so the next frame starts rendering
// right after it.
inline std::chrono::microseconds held_frame_sleep(int refresh_hz,
                                                  std::chrono::microseconds iteration_elapsed) {
    const FramePacing p = frame_pacing_for(30, refresh_hz);
    return std::max(std::chrono::microseconds{0}, p.min_iteration - iteration_elapsed);
}

}  // namespace utils
