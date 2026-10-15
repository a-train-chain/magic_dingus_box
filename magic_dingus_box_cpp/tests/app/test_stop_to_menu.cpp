// stop_to_menu(): the ONE "playback is over, show the menu" state reset.
//
// The black-screen defect: a non-looping playlist ending, every item failing
// to load, and the playback-error policy giving up all stopped the pipeline
// but left current_playlist_index/current_item_index >= 0. Renderer::render
// reads "indexes set + no video" as a between-items transition and returns
// before drawing anything — so the menu, the error banner AND the settings
// menu were all invisible until a reboot. 9 of 13 playlists on the owner's
// box are `loop: false`, so this hit at the end of most of them.
//
// The renderer predicate is mirrored here (renderer_would_skip_ui) rather
// than linked, because renderer.cpp needs GL. If that predicate changes,
// update the mirror — the test exists to pin the CONTRACT between the two.

#include <catch2/catch_test_macros.hpp>

#include "app/app_state.h"
#include "app/playback_reset.h"

namespace {

// Mirror of Renderer::render's early-out: is_transitioning && !video_active.
bool renderer_would_skip_ui(const app::AppState& s) {
    const bool is_transitioning =
        s.current_playlist_index >= 0 && s.current_item_index >= 0;
    return is_transitioning && !s.video_active;
}

// A box mid-playlist: item 8 of playlist 3 playing, menu hidden, a UI fade
// toward hidden still in flight, and the phone remote showing the track.
void put_mid_playback(app::AppState& s) {
    s.current_playlist_index = 3;
    s.current_item_index = 8;
    s.video_active = true;
    s.ui_visible_when_playing = false;
    s.is_fading = true;
    s.fade_target_ui_visible = false;
    s.is_switching_playlist = true;
    s.last_advanced_item_index = 8;
    s.last_advanced_duration = 120.0;
    s.update_playback_state(119.8, 120.0);
    s.now_playing_title = "Episode 9";
    s.now_playing_subtitle = "Cartoons";
    s.now_playing_kind = "video";
    s.current_playlist_name = "Saturday Morning";
    s.current_item_count = 9;
}

}  // namespace

TEST_CASE("the old end-of-playlist reset leaves the renderer blank",
          "[stop_to_menu][regression]") {
    // Documents the defect this function replaces: what load_next_item's
    // non-loop branch used to do (cursor back to 0, video off).
    app::AppState s;
    put_mid_playback(s);
    s.current_item_index = 0;
    s.video_active = false;
    s.ui_visible_when_playing = true;
    CHECK(renderer_would_skip_ui(s));
}

TEST_CASE("stop_to_menu leaves a state the renderer draws",
          "[stop_to_menu]") {
    app::AppState s;
    put_mid_playback(s);

    app::stop_to_menu(s);

    CHECK_FALSE(renderer_would_skip_ui(s));
    CHECK(s.current_playlist_index == -1);
    CHECK(s.current_item_index == -1);
    CHECK_FALSE(s.video_active);
    CHECK_FALSE(s.is_switching_playlist);
    // A stale fade toward hidden would zero the menu's alpha even with the
    // indexes cleared — the second half of the blank-menu failure.
    CHECK_FALSE(s.is_fading);
    CHECK(s.ui_visible_when_playing);
}

TEST_CASE("stop_to_menu clears the stopped item's playback bookkeeping",
          "[stop_to_menu]") {
    app::AppState s;
    put_mid_playback(s);

    app::stop_to_menu(s);

    CHECK(s.get_position() == 0.0);
    CHECK(s.get_duration() == 0.0);
    CHECK(s.last_advanced_item_index == -1);
    CHECK(s.last_advanced_duration == 0.0);
}

TEST_CASE("stop_to_menu clears the phone remote's now-playing fields",
          "[stop_to_menu]") {
    // update_state's own stop-clear never fires on these paths: they force
    // video_active false themselves, so update_state sees no true->false edge.
    app::AppState s;
    put_mid_playback(s);

    app::stop_to_menu(s);

    CHECK(s.now_playing_title.empty());
    CHECK(s.now_playing_subtitle.empty());
    CHECK(s.now_playing_kind.empty());
    CHECK(s.current_playlist_name.empty());
    CHECK(s.current_item_count == 0);
}

TEST_CASE("stop_to_menu with a message raises the error banner",
          "[stop_to_menu]") {
    app::AppState s;
    put_mid_playback(s);

    app::stop_to_menu(s, "Couldn't play these videos");

    CHECK(s.has_error_message());
    CHECK(s.error_message == "Couldn't play these videos");
    CHECK_FALSE(renderer_would_skip_ui(s));
}

TEST_CASE("stop_to_menu without a message leaves an existing banner alone",
          "[stop_to_menu]") {
    app::AppState s;
    put_mid_playback(s);
    s.set_error("earlier problem");

    app::stop_to_menu(s);

    CHECK(s.error_message == "earlier problem");
}

TEST_CASE("stop_to_menu is idempotent", "[stop_to_menu]") {
    app::AppState s;
    put_mid_playback(s);
    app::stop_to_menu(s);
    app::stop_to_menu(s);
    CHECK_FALSE(renderer_would_skip_ui(s));
    CHECK(s.current_item_index == -1);
}
