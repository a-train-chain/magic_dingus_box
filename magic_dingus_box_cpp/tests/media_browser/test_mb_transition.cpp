// The Media Browser screen-to-screen hand-off table
// (media_browser/ui/mb_transition.h). Each case pins one row of the
// dispatcher's hand-off: a regression here is a screen opening on the
// wrong movie, BTN4 looping back into a sub-screen, Playback starting
// with no file, or the artwork worker left paused for good.

#include <catch2/catch_test_macros.hpp>

#include "media_browser/ui/mb_transition.h"

using media_browser::ui::ArtworkAction;
using media_browser::ui::DetailIdSource;
using media_browser::ui::PlaybackHandoff;
using media_browser::ui::plan_transition;
using media_browser::ui::Screen;
using media_browser::ui::SeriesIdSource;
using media_browser::ui::TransitionPlan;

namespace {

constexpr Screen kAll[] = {
    Screen::Browse,  Screen::Search,  Screen::Detail,
    Screen::SeriesDetail, Screen::ReleasePicker, Screen::Queue,
    Screen::Library, Screen::Playback, Screen::MovieSettings,
};

bool is_noop(const TransitionPlan& p) {
    return p.detail_id == DetailIdSource::None && !p.set_detail_origin &&
           p.series_id == SeriesIdSource::None &&
           !p.take_next_season_intent &&
           p.playback == PlaybackHandoff::None &&
           p.artwork == ArtworkAction::None && !p.flush_watch_state;
}

}  // namespace

TEST_CASE("Detail takes its movie from the screen that opened it",
          "[media_browser][transition]") {
    CHECK(plan_transition(Screen::Browse, Screen::Detail).detail_id ==
          DetailIdSource::Browse);
    CHECK(plan_transition(Screen::Search, Screen::Detail).detail_id ==
          DetailIdSource::Search);
    // Library is mixed-kind: the dispatcher re-checks the ref is a movie.
    CHECK(plan_transition(Screen::Library, Screen::Detail).detail_id ==
          DetailIdSource::LibraryMovie);
    // Coming back up from a sub-screen keeps the movie Detail already has.
    CHECK(plan_transition(Screen::Playback, Screen::Detail).detail_id ==
          DetailIdSource::None);
    CHECK(plan_transition(Screen::ReleasePicker, Screen::Detail).detail_id ==
          DetailIdSource::None);
    CHECK(plan_transition(Screen::Queue, Screen::Detail).detail_id ==
          DetailIdSource::None);
}

TEST_CASE("Detail's origin skips its own sub-screens",
          "[media_browser][transition]") {
    CHECK(plan_transition(Screen::Browse, Screen::Detail).set_detail_origin);
    CHECK(plan_transition(Screen::Search, Screen::Detail).set_detail_origin);
    CHECK(plan_transition(Screen::Library, Screen::Detail).set_detail_origin);
    CHECK(plan_transition(Screen::Queue, Screen::Detail).set_detail_origin);
    // BTN4 on Detail must not loop back into Playback / the picker.
    CHECK_FALSE(plan_transition(Screen::Playback, Screen::Detail).set_detail_origin);
    CHECK_FALSE(
        plan_transition(Screen::ReleasePicker, Screen::Detail).set_detail_origin);
}

TEST_CASE("SeriesDetail takes its show from Browse or Library only",
          "[media_browser][transition]") {
    CHECK(plan_transition(Screen::Browse, Screen::SeriesDetail).series_id ==
          SeriesIdSource::Browse);
    CHECK(plan_transition(Screen::Library, Screen::SeriesDetail).series_id ==
          SeriesIdSource::Library);
    // Playback never changes the show (or its origin).
    const auto from_playback =
        plan_transition(Screen::Playback, Screen::SeriesDetail);
    CHECK(from_playback.series_id == SeriesIdSource::None);
    CHECK(from_playback.take_next_season_intent);
    CHECK_FALSE(plan_transition(Screen::Browse, Screen::SeriesDetail)
                    .take_next_season_intent);
}

TEST_CASE("Playback is loaded only from Detail or SeriesDetail",
          "[media_browser][transition]") {
    CHECK(plan_transition(Screen::Detail, Screen::Playback).playback ==
          PlaybackHandoff::FromDetail);
    CHECK(plan_transition(Screen::SeriesDetail, Screen::Playback).playback ==
          PlaybackHandoff::FromSeriesDetail);
    for (Screen from : kAll) {
        if (from == Screen::Detail || from == Screen::SeriesDetail) continue;
        CHECK(plan_transition(from, Screen::Playback).playback ==
              PlaybackHandoff::None);
    }
}

TEST_CASE("Artwork worker pauses into Playback and resumes out of it",
          "[media_browser][transition]") {
    for (Screen s : kAll) {
        if (s == Screen::Playback) continue;
        CHECK(plan_transition(s, Screen::Playback).artwork ==
              ArtworkAction::PauseAndTrim);
        CHECK(plan_transition(Screen::Playback, s).artwork ==
              ArtworkAction::Resume);
    }
    CHECK(plan_transition(Screen::Browse, Screen::Library).artwork ==
          ArtworkAction::None);
}

TEST_CASE("Leaving Playback flushes the watch position",
          "[media_browser][transition]") {
    for (Screen s : kAll) {
        if (s == Screen::Playback) continue;
        CHECK(plan_transition(Screen::Playback, s).flush_watch_state);
        CHECK_FALSE(plan_transition(s, Screen::Playback).flush_watch_state);
    }
}

TEST_CASE("Tab-to-tab moves hand nothing over",
          "[media_browser][transition]") {
    constexpr Screen kTabs[] = {Screen::Browse, Screen::Search, Screen::Queue,
                                Screen::Library, Screen::MovieSettings};
    for (Screen from : kTabs) {
        for (Screen to : kTabs) {
            if (from == to) continue;
            CHECK(is_noop(plan_transition(from, to)));
        }
    }
}

TEST_CASE("The table is usable at compile time", "[media_browser][transition]") {
    static_assert(plan_transition(Screen::Detail, Screen::Playback).playback ==
                  PlaybackHandoff::FromDetail);
    static_assert(plan_transition(Screen::Playback, Screen::Detail).flush_watch_state);
    SUCCEED();
}
