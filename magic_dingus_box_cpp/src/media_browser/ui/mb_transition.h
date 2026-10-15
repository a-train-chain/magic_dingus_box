#pragma once

// The Media Browser's screen-to-screen hand-off table.
//
// When the active MbScreen's handle_input() returns a sibling Screen, the
// dispatcher (media_browser/mb_host.cpp) has to move state from the screen
// being left to the screen being entered BEFORE the leave()/enter() pair
// runs: which movie Detail should show, where BTN4 on Detail returns to,
// what Playback loads and resumes at, whether the artwork worker pauses.
// These decisions depend only on the (from, to) pair, so they live here as
// a pure table — the dispatcher executes the plan, this header decides it,
// and tests/media_browser/test_mb_transition.cpp pins every row on the Mac
// with no GL, no screens and no services.
//
// The plan says WHAT moves; the dispatcher still owns the ORDER (sources
// read before leave(), watch-state flush before leave()). Neither may be
// reordered without reading the contracts in mb_host.cpp.

#include "media_browser/ui/mb_screen.h"

namespace media_browser::ui {

// Which screen's selection feeds DetailScreen::set_tmdb_id.
enum class DetailIdSource {
    None,
    Browse,        // mb_browse.selected_tmdb_id()
    Search,        // mb_search.selected_tmdb_id()
    LibraryMovie,  // mb_library.selected_ref().id — ONLY when that ref is a
                   // movie. Library is mixed-kind and movie/TV ids collide,
                   // so the dispatcher re-checks the kind at execution time.
};

// Which screen's selection feeds SeriesDetailScreen::set_tmdb_id (and its
// origin).
enum class SeriesIdSource {
    None,
    Browse,   // mb_browse.selected_tmdb_id()
    Library,  // mb_library.selected_ref().id
};

// The play-target copy into PlaybackScreen.
enum class PlaybackHandoff {
    None,
    FromDetail,        // movie: path, title, rich meta, resume, identity
    FromSeriesDetail,  // TV: the above plus the episode context
};

// Artwork I/O contention guard around Playback.
enum class ArtworkAction {
    None,
    PauseAndTrim,  // entering Playback: pause the worker, trim textures
    Resume,        // leaving Playback for a sibling screen
};

struct TransitionPlan {
    DetailIdSource detail_id = DetailIdSource::None;
    // Detail remembers the screen that opened it, so BTN4 returns there.
    // NOT set when coming back up from Playback or ReleasePicker — both are
    // sub-screens of Detail, and pointing origin at them would loop BTN4
    // straight back into the sub-screen just exited.
    bool set_detail_origin = false;
    SeriesIdSource series_id = SeriesIdSource::None;
    // Playback -> SeriesDetail drains the season-end card's one-shot
    // "Start Season N" intent. Must be taken BEFORE leave()/enter():
    // SeriesDetail's enter() clears an unconsumed intent.
    bool take_next_season_intent = false;
    PlaybackHandoff playback = PlaybackHandoff::None;
    ArtworkAction artwork = ArtworkAction::None;
    // Leaving Playback: persist the resume point BEFORE leave() stops the
    // pipeline (see flush_watch_state in mb_host.cpp).
    bool flush_watch_state = false;
};

// Decide the hand-off for a transition from `from` to `to`. Only
// meaningful for a real transition (to != from, to != Exit); the
// dispatcher never consults it otherwise.
constexpr TransitionPlan plan_transition(Screen from, Screen to) {
    TransitionPlan p{};
    if (to == Screen::Detail) {
        if (from == Screen::Browse) {
            p.detail_id = DetailIdSource::Browse;
        } else if (from == Screen::Search) {
            p.detail_id = DetailIdSource::Search;
        } else if (from == Screen::Library) {
            p.detail_id = DetailIdSource::LibraryMovie;
        }
        p.set_detail_origin =
            from != Screen::Playback && from != Screen::ReleasePicker;
    }
    if (to == Screen::SeriesDetail) {
        if (from == Screen::Browse) {
            p.series_id = SeriesIdSource::Browse;
        } else if (from == Screen::Library) {
            p.series_id = SeriesIdSource::Library;
        }
        p.take_next_season_intent = (from == Screen::Playback);
    }
    if (to == Screen::Playback && from == Screen::Detail) {
        p.playback = PlaybackHandoff::FromDetail;
    }
    if (to == Screen::Playback && from == Screen::SeriesDetail) {
        p.playback = PlaybackHandoff::FromSeriesDetail;
    }
    if (to == Screen::Playback && from != Screen::Playback) {
        p.artwork = ArtworkAction::PauseAndTrim;
    } else if (from == Screen::Playback && to != Screen::Playback) {
        p.artwork = ArtworkAction::Resume;
    }
    p.flush_watch_state = (from == Screen::Playback && to != Screen::Playback);
    return p;
}

}  // namespace media_browser::ui
