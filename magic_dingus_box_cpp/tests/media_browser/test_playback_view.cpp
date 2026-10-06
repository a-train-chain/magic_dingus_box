// Table tests for playback_view.h — PlaybackScreen's now-playing triple,
// fades, seek curve, finished-episode lookup, end-overlay table, card
// height and quick-add copy, moved out of playback_screen.cpp so they run on
// the Mac.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "media_browser/ui/playback_view.h"

namespace mb = media_browser;
using namespace media_browser::ui;

namespace {

const std::string kDash = "\xE2\x80\x94";

mb::EpisodeInfo ep(int s, int e, const std::string& title = "") {
    mb::EpisodeInfo x;
    x.season_number = s;
    x.episode_number = e;
    x.title = title;
    return x;
}

}  // namespace

TEST_CASE("playback view: now-playing status", "[playback_view]") {
    const std::vector<mb::EpisodeInfo> eps{ep(2, 4, "Four"), ep(2, 5, "Five")};
    auto s = now_playing_status(true, 2, 5, eps, "Show", "Show " + kDash + " S2E5", 0);
    CHECK(s.kind == "tv");
    CHECK(s.title == "Show");
    CHECK(s.subtitle == "S2E5 \xC2\xB7 Five");
    s = now_playing_status(true, 3, 1, eps, "Show", "x", 0);
    CHECK(s.subtitle == "S3E1");  // not in the vector: bare code
    // Identity-less TV (no series title) degrades to the movie shape.
    s = now_playing_status(true, 2, 5, eps, "", "Display Title", 2001);
    CHECK(s.kind == "movie");
    CHECK(s.title == "Display Title");
    CHECK(s.subtitle == "2001");
    s = now_playing_status(false, 0, 0, {}, "", "Heat", 0);
    CHECK(s.kind == "movie");
    CHECK(s.subtitle.empty());
}

TEST_CASE("playback view: fades and heading", "[playback_view]") {
    CHECK(hud_alpha_for(true, true, 0, 300) == 1.0f);
    CHECK(hud_alpha_for(false, true, 0, 300) == 0.0f);
    CHECK(hud_alpha_for(false, false, 2000, 300) == 1.0f);
    CHECK(hud_alpha_for(false, false, 150, 300) == 0.5f);
    CHECK(hud_alpha_for(false, false, 300, 300) == 1.0f);

    CHECK(title_marquee_alpha(2000) == 1.0f);
    CHECK(title_marquee_alpha(500) == 1.0f);
    CHECK(title_marquee_alpha(250) == 0.5f);

    CHECK(now_playing_heading("") == "NOW PLAYING");
    CHECK(now_playing_heading("Heat") == "NOW PLAYING " + kDash + " Heat");
}

TEST_CASE("playback view: rotary seek curve", "[playback_view]") {
    CHECK(rotary_seek_seconds(0.0) == 5.0);
    CHECK(rotary_seek_seconds(1.0) == 120.0);
    CHECK(rotary_seek_seconds(0.5) == 5.0 + 115.0 * 0.25);
}

TEST_CASE("playback view: finished-episode lookup", "[playback_view]") {
    const std::vector<mb::EpisodeInfo> eps{ep(1, 1), ep(1, 2), ep(2, 1)};
    CHECK(finished_episode_index(eps, 1, 1, 2) == 1);   // cache trusted
    CHECK(finished_episode_index(eps, 0, 2, 1) == 2);   // stale cache: search
    CHECK(finished_episode_index(eps, -1, 1, 1) == 0);
    CHECK(finished_episode_index(eps, 9, 1, 1) == 0);   // out-of-range cache
    CHECK(finished_episode_index(eps, 0, 3, 3) == -1);
    CHECK(finished_episode_index({}, -1, 1, 1) == -1);
}

TEST_CASE("playback view: end-overlay SELECT and countdown", "[playback_view]") {
    using K = EndOverlayKind;
    using S = EndOverlaySelect;
    CHECK(decide_end_overlay_select(K::Countdown, true) == S::PlayNext);
    // The prompt carries has_primary for its chrome — still Continue.
    CHECK(decide_end_overlay_select(K::StillWatching, true) == S::Continue);
    CHECK(decide_end_overlay_select(K::Card, true) == S::StartSeason);
    CHECK(decide_end_overlay_select(K::Card, false) == S::Done);

    CHECK(countdown_remaining_seconds(8, 0) == 8);
    CHECK(countdown_remaining_seconds(8, 999) == 8);
    CHECK(countdown_remaining_seconds(8, 1000) == 7);
    CHECK(countdown_remaining_seconds(8, 7999) == 1);
    CHECK(countdown_remaining_seconds(8, 60000) == 1);  // never below 1

    CHECK(end_card_height(K::Card, false, 26) == 26 + 30 + 16 + 44 + 26);
    CHECK(end_card_height(K::Card, true, 26) == 26 + 30 + 28 + 16 + 44 + 26);
    CHECK(end_card_height(K::StillWatching, true, 26) ==
          26 + 30 + 28 + 28 + 16 + 44 + 26);
    CHECK(end_card_height(K::Countdown, true, 26) == 26 + 30 + 28 + 16 + 24 + 26);
}

TEST_CASE("playback view: quick-add profile and outcome copy", "[playback_view]") {
    std::vector<mb::QualityProfile> ps;
    CHECK(pick_quick_add_profile_id(ps) == 0);
    ps.push_back({3, "HD-1080p", 0, {}});
    CHECK(pick_quick_add_profile_id(ps) == 3);
    ps.push_back({4, "HD - 720p/1080p", 0, {}});
    CHECK(pick_quick_add_profile_id(ps) == 4);
    ps.push_back({5, "Any", 0, {}});
    CHECK(pick_quick_add_profile_id(ps) == 5);

    CHECK(quick_add_failure_toast("HTTP 400: This movie has already been added") ==
          "Already in library");
    CHECK(quick_add_failure_toast("Already exists") == "Already in library");
    CHECK(quick_add_failure_toast("timeout") ==
          "Couldn\xe2\x80\x99t add " + kDash + " try again");

    const auto hints = playback_footer_hints();
    REQUIRE(hints.size() == 6);
    CHECK(hints[0].action == "\xE2\x88\x92" "10s");
    CHECK(hints[5].action == "Open Menu");
}

TEST_CASE("playback view: episode display title is shared", "[playback_view]") {
    CHECK(series_episode_display_title("Show", ep(2, 6, "Six")) ==
          "Show " + kDash + " S2E6 \xC2\xB7 Six");
}
