// reset_main_ui_for_media_browser() / clear_now_playing(): the siblings of
// stop_to_menu() used on both sides of a Media Browser session.
//
// Why a sibling and not stop_to_menu itself: the Media Browser hands the
// screen to its own surfaces, so it parks the playlist UI HIDDEN
// (ui_visible_when_playing = false) where stop_to_menu shows it, and it
// leaves the playback position and the auto-advance guard alone — the MB
// PlaybackScreen drives the shared pipeline and owns those. What both
// must share with stop_to_menu is the anti-blank-menu part: playing-item
// indexes and any UI fade cleared, or the main menu draws nothing after
// the Media Browser closes.

#include <catch2/catch_test_macros.hpp>

#include "app/app_state.h"
#include "app/playback_reset.h"

namespace {

// Mirror of Renderer::render's early-out (see test_stop_to_menu.cpp).
bool renderer_would_skip_ui(const app::AppState& s) {
    const bool is_transitioning =
        s.current_playlist_index >= 0 && s.current_item_index >= 0;
    return is_transitioning && !s.video_active;
}

void put_mid_playback(app::AppState& s) {
    s.current_playlist_index = 3;
    s.current_item_index = 8;
    s.video_active = true;
    s.ui_visible_when_playing = true;
    s.is_fading = true;
    s.fade_target_ui_visible = false;
    s.is_switching_playlist = true;
    s.last_advanced_item_index = 8;
    s.last_advanced_duration = 120.0;
    s.update_playback_state(42.0, 120.0);
    s.now_playing_title = "Episode 9";
    s.now_playing_subtitle = "Cartoons";
    s.now_playing_kind = "video";
    s.current_playlist_name = "Saturday Morning";
    s.current_item_count = 9;
}

}  // namespace

TEST_CASE("reset_main_ui_for_media_browser leaves a drawable, hidden menu",
          "[mb_ui_reset]") {
    app::AppState s;
    put_mid_playback(s);

    app::reset_main_ui_for_media_browser(s);

    CHECK_FALSE(renderer_would_skip_ui(s));
    CHECK(s.current_playlist_index == -1);
    CHECK(s.current_item_index == -1);
    CHECK_FALSE(s.video_active);
    CHECK_FALSE(s.is_switching_playlist);
    CHECK_FALSE(s.is_fading);
    CHECK_FALSE(s.ui_visible_when_playing);
}

TEST_CASE("reset_main_ui_for_media_browser leaves MB-owned fields alone",
          "[mb_ui_reset]") {
    // Called on MB EXIT after PlaybackScreen::leave(): the position the
    // watch flush already consumed, the phone-remote fields and the
    // auto-advance guard are not this function's to touch.
    app::AppState s;
    put_mid_playback(s);

    app::reset_main_ui_for_media_browser(s);

    CHECK(s.get_position() == 42.0);
    CHECK(s.get_duration() == 120.0);
    CHECK(s.last_advanced_item_index == 8);
    CHECK(s.now_playing_title == "Episode 9");
    CHECK(s.current_item_count == 9);
}

TEST_CASE("clear_now_playing clears exactly the phone remote's fields",
          "[mb_ui_reset]") {
    app::AppState s;
    put_mid_playback(s);

    app::clear_now_playing(s);

    CHECK(s.now_playing_title.empty());
    CHECK(s.now_playing_subtitle.empty());
    CHECK(s.now_playing_kind.empty());
    CHECK(s.current_playlist_name.empty());
    CHECK(s.current_item_count == 0);
    // Nothing else.
    CHECK(s.video_active);
    CHECK(s.current_item_index == 8);
}
