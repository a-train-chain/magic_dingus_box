// utils::frame_pacing_for / frame_cap_sleep — the main loop's frame cap.
//
// The simulations model what the kiosk actually does: present_frame()
// blocks until the page flip, which lands on the first vblank after the
// frame finished rendering. They pin the 30 fps movie-mode bug (frames
// alternating 16/33/50 ms) and its fix (every flip exactly 2 vblanks).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

#include "utils/frame_pacing.h"

using std::chrono::microseconds;
using utils::frame_cap_sleep;
using utils::field_rate_skip_sleep;
using utils::frame_pacing_for;

namespace {

// First vblank strictly after t (vblanks at multiples of period).
std::int64_t next_vblank(std::int64_t t_us, std::int64_t period_us) {
    return (t_us / period_us + 1) * period_us;
}

// Render cost that jitters between 2 and 12 ms — a busy Pi 4 overlay frame.
std::int64_t render_us(int i) { return 2000 + (i * 3700) % 10000; }

// Runs the FIXED loop; returns the intervals between consecutive flips.
std::vector<std::int64_t> simulate_fixed(int target_fps, int refresh_hz,
                                         bool flip_blocks, int frames) {
    const auto p = frame_pacing_for(target_fps, refresh_hz);
    const std::int64_t period = 1000000 / refresh_hz;
    std::vector<std::int64_t> intervals;
    std::int64_t t = 1234;  // arbitrary phase
    std::int64_t last_flip = -1;
    for (int i = 0; i < frames; ++i) {
        const std::int64_t start = t;
        t += render_us(i);
        if (flip_blocks) t = next_vblank(t, period);
        if (last_flip >= 0) intervals.push_back(t - last_flip);
        last_flip = t;
        t += frame_cap_sleep(p, microseconds{t - start}).count();
    }
    return intervals;
}

// Runs the OLD loop: sleep(target - previous start-to-start period), in
// truncated ms.
std::vector<std::int64_t> simulate_old(int target_ms, int refresh_hz, int frames) {
    const std::int64_t period = 1000000 / refresh_hz;
    std::vector<std::int64_t> intervals;
    std::int64_t t = 1234;
    std::int64_t last_start = t;
    std::int64_t last_flip = -1;
    for (int i = 0; i < frames; ++i) {
        const std::int64_t delta_ms = (t - last_start) / 1000;
        last_start = t;
        t += render_us(i);
        t = next_vblank(t, period);
        if (last_flip >= 0) intervals.push_back(t - last_flip);
        last_flip = t;
        if (delta_ms < target_ms) t += (target_ms - delta_ms) * 1000;
    }
    return intervals;
}

}  // namespace

TEST_CASE("frame_pacing_for: vblanks per frame", "[frame_pacing]") {
    CHECK(frame_pacing_for(30, 60).vblanks_per_frame == 2);
    CHECK(frame_pacing_for(60, 60).vblanks_per_frame == 1);
    CHECK(frame_pacing_for(30, 50).vblanks_per_frame == 2);   // 25 fps
    CHECK(frame_pacing_for(30, 30).vblanks_per_frame == 1);
    CHECK(frame_pacing_for(30, 24).vblanks_per_frame == 1);
    CHECK(frame_pacing_for(60, 120).vblanks_per_frame == 2);
    // Unknown refresh (vrefresh 0) is treated as 60 Hz.
    CHECK(frame_pacing_for(30, 0).vblanks_per_frame == 2);
}

TEST_CASE("frame_pacing_for: 60 fps on 60 Hz never sleeps after a blocking "
          "flip", "[frame_pacing]") {
    const auto p = frame_pacing_for(60, 60);
    CHECK(p.post_present_gap.count() == 0);
    // A full vblank-locked iteration (16.67 ms) needs no extra sleep.
    CHECK(frame_cap_sleep(p, microseconds{16666}).count() == 0);
}

TEST_CASE("old 30 fps cap alternated flip intervals (regression pin)",
          "[frame_pacing]") {
    const auto iv = simulate_old(33, 60, 120);
    bool saw_one_vblank = false;
    for (auto d : iv) {
        if (d < 20000) saw_one_vblank = true;  // a 16.7 ms frame
    }
    CHECK(saw_one_vblank);
}

TEST_CASE("fixed 30 fps cap flips exactly every second vblank at 60 Hz",
          "[frame_pacing]") {
    const std::int64_t two_vblanks = 2 * (1000000 / 60);
    for (auto d : simulate_fixed(30, 60, /*flip_blocks=*/true, 600)) {
        REQUIRE(d == two_vblanks);
    }
}

TEST_CASE("fixed 60 fps cap flips every vblank at 60 Hz", "[frame_pacing]") {
    const std::int64_t one_vblank = 1000000 / 60;
    for (auto d : simulate_fixed(60, 60, /*flip_blocks=*/true, 600)) {
        REQUIRE(d == one_vblank);
    }
}

TEST_CASE("30 fps on a 50 Hz mode paces evenly at 25 fps", "[frame_pacing]") {
    const std::int64_t two_vblanks = 2 * (1000000 / 50);
    for (auto d : simulate_fixed(30, 50, /*flip_blocks=*/true, 600)) {
        REQUIRE(d == two_vblanks);
    }
}

TEST_CASE("cap still binds when the flip does not block (SetCrtc fallback)",
          "[frame_pacing]") {
    // No vblank anchor: the iteration floor must keep the loop near the
    // target instead of spinning (60) or running ~45 fps (30).
    for (int fps : {30, 60}) {
        const auto p = frame_pacing_for(fps, 60);
        const std::int64_t floor_us = p.min_iteration.count();
        std::int64_t t = 0;
        for (int i = 0; i < 200; ++i) {
            const std::int64_t start = t;
            t += render_us(i);  // present returns immediately
            t += frame_cap_sleep(p, microseconds{t - start}).count();
            REQUIRE(t - start >= floor_us);
            REQUIRE(t - start <= floor_us + 1000000 / 60);
        }
        // Floor is within an eighth of a refresh of the target period.
        CHECK(floor_us >= (1000000 / fps) - (1000000 / 60) / 8 - 1);
    }
}

namespace {

// The CRT field-rate loop: 60 fps pacing, the redraw gate alternating
// draw / skip, skipped iterations paced by field_rate_skip_sleep.
std::vector<std::int64_t> simulate_field_rate(int refresh_hz, int frames,
                                              std::int64_t skip_work_us) {
    const auto p = frame_pacing_for(60, refresh_hz);
    const std::int64_t period = 1000000 / refresh_hz;
    std::vector<std::int64_t> intervals;
    std::int64_t t = 1234;
    std::int64_t last_flip = -1;
    bool draw = true;
    int drawn = 0;
    while (drawn < frames) {
        const std::int64_t start = t;
        if (draw) {
            t += render_us(drawn);
            t = next_vblank(t, period);
            if (last_flip >= 0) intervals.push_back(t - last_flip);
            last_flip = t;
            ++drawn;
            t += frame_cap_sleep(p, microseconds{t - start}).count();
        } else {
            t += skip_work_us;  // input poll, status file, ...
            t += field_rate_skip_sleep(p, refresh_hz, microseconds{t - last_flip},
                                       microseconds{t - start})
                     .count();
        }
        draw = !draw;
    }
    return intervals;
}

}  // namespace

TEST_CASE("CRT field rate flips exactly every second vblank", "[frame_pacing]") {
    // Render costs up to 12 ms (render_us): the drawing iteration starts
    // just past the skipped vblank, so it has ~14.6 ms of budget.
    for (std::int64_t skip_work : {0, 300, 3000}) {
        const std::int64_t two_vblanks = 2 * (1000000 / 60);
        for (auto d : simulate_field_rate(60, 600, skip_work)) {
            REQUIRE(d == two_vblanks);
        }
    }
    const std::int64_t two_at_50 = 2 * (1000000 / 50);
    for (auto d : simulate_field_rate(50, 300, 0)) {
        REQUIRE(d == two_at_50);
    }
}

TEST_CASE("field_rate_skip_sleep: never shorter than the normal cap",
          "[frame_pacing]") {
    const auto p = frame_pacing_for(60, 60);
    // Present long ago: falls back to the ordinary floor.
    CHECK(utils::field_rate_skip_sleep(p, 60, microseconds{100000},
                                       microseconds{1000}) ==
          frame_cap_sleep(p, microseconds{1000}));
    // Right after a present: sleeps past the next vblank (+ margin).
    CHECK(utils::field_rate_skip_sleep(p, 60, microseconds{0}, microseconds{0})
              .count() == 16666 + 16666 / 8);
}
