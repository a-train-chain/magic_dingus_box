// app/intro_sequence.h — the boot intro's pure decisions, moved out of
// main.cpp (which is in no test target).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "app/app_state.h"
#include "app/intro_sequence.h"

using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;

TEST_CASE("intro_video_ended: within 50 ms of the end", "[intro]") {
    CHECK_FALSE(app::intro_video_ended(9.0, 10.0, true));
    CHECK_FALSE(app::intro_video_ended(9.94, 10.0, true));
    CHECK(app::intro_video_ended(9.95, 10.0, true));
    CHECK(app::intro_video_ended(10.0, 10.0, true));
    CHECK(app::intro_video_ended(10.2, 10.0, true));
}

TEST_CASE("intro_video_ended: player stopped after making progress", "[intro]") {
    // The fallback: GStreamer stopped short of the end.
    CHECK(app::intro_video_ended(3.0, 10.0, false));
    // Not before any progress — a pipeline that has not started yet.
    CHECK_FALSE(app::intro_video_ended(0.0, 10.0, false));
    // Still playing mid-way: not ended.
    CHECK_FALSE(app::intro_video_ended(3.0, 10.0, true));
}

TEST_CASE("intro_fade_out_volume: linear ramp to zero over kIntroFadeOut", "[intro]") {
    REQUIRE(app::kIntroFadeOut == 300ms);
    CHECK_THAT(app::intro_fade_out_volume(80.0, 0ms), WithinAbs(80.0, 1e-6));
    CHECK_THAT(app::intro_fade_out_volume(80.0, 150ms), WithinAbs(40.0, 1e-4));
    CHECK_THAT(app::intro_fade_out_volume(80.0, 300ms), WithinAbs(0.0, 1e-6));
    // Clamped: never negative, never above the starting level.
    CHECK_THAT(app::intro_fade_out_volume(80.0, 900ms), WithinAbs(0.0, 1e-6));
    CHECK_THAT(app::intro_fade_out_volume(80.0, -50ms), WithinAbs(80.0, 1e-6));
}

TEST_CASE("hand_intro_to_menu: stopped, de-indexed, menu fading IN", "[intro]") {
    app::AppState state;
    state.video_active = true;
    state.update_playback_state(41.5, 42.0);
    state.current_playlist_index = 3;
    state.current_item_index = 2;
    state.is_fading = false;
    state.fade_target_ui_visible = false;

    const auto now = std::chrono::steady_clock::now();
    app::hand_intro_to_menu(state, now);

    CHECK_FALSE(state.video_active);
    CHECK(state.get_position() == 0.0);
    CHECK(state.get_duration() == 0.0);
    CHECK(state.current_playlist_index == -1);
    CHECK(state.current_item_index == -1);
    // A fade-in, not stop_to_menu's instant full-alpha menu.
    CHECK(state.is_fading);
    CHECK(state.fade_target_ui_visible);
    CHECK(state.fade_start_time == now);
}

TEST_CASE("should_render_video: intro phase", "[intro]") {
    app::AppState state;
    state.intro_complete = false;
    state.showing_intro_video = true;
    state.video_active = false;
    state.intro_fading_out = false;
    // Showing the intro draws the video plane even before video_active.
    CHECK(app::should_render_video(state, false));
    // ...but not during its fade-out: the menu fades in over black.
    state.intro_fading_out = true;
    CHECK_FALSE(app::should_render_video(state, true));
    // Nothing showing and nothing playing: no video.
    state.intro_fading_out = false;
    state.showing_intro_video = false;
    CHECK_FALSE(app::should_render_video(state, false));
    CHECK(app::should_render_video(state, true));
}

TEST_CASE("should_render_video: after the intro", "[intro]") {
    app::AppState state;
    state.intro_complete = true;
    state.video_active = false;
    state.is_switching_playlist = false;
    // A stray playing pipeline alone does not draw after the intro.
    CHECK_FALSE(app::should_render_video(state, true));
    state.video_active = true;
    CHECK(app::should_render_video(state, false));
    // Mid playlist switch: only once the new pipeline is playing.
    state.video_active = false;
    state.is_switching_playlist = true;
    CHECK_FALSE(app::should_render_video(state, false));
    CHECK(app::should_render_video(state, true));
    // The intro's fade flag is irrelevant after the intro.
    state.intro_fading_out = true;
    CHECK(app::should_render_video(state, true));
}
