#include <catch2/catch_test_macros.hpp>

#include "media_browser/ui/still_watching.h"

using namespace media_browser;
using namespace media_browser::ui;

namespace {

// Same minimal fake episode contract as test_episode_logic.cpp — the
// overlay-conversion test builds a real Countdown model through
// decide_end_overlay to prove make_still_watching_overlay preserves it.
struct FakeEp {
    int season_number;
    int episode_number;
    bool has_file;
    std::string title;
};

SeasonRow season_row(int n, SeasonState st, bool monitored, int files) {
    SeasonRow r;
    r.season_number = n;
    r.state = st;
    r.monitored = monitored;
    r.episode_file_count = files;
    return r;
}

}  // namespace

TEST_CASE("streak limit is three auto-started episodes", "[still_watching]") {
    // The whole feature: after THREE consecutively auto-started episodes
    // with no interaction, the box stops and asks. The prompt gives one
    // minute to answer.
    CHECK(kAutoAdvanceStreakLimit == 3);
    CHECK(kStillWatchingTimeoutSeconds == 60);
}

TEST_CASE("manual start, three silent auto-advances, then the prompt",
          "[still_watching]") {
    StillWatchingGuard g;
    // Ep 1 starts from the picker — manual, streak 0.
    g.note_session_start();
    CHECK(g.streak == 0);

    // Ep 1 ends untouched: countdown runs (no prompt), expiry auto-starts
    // ep 2.
    CHECK(g.decide() == EosAdvance::AutoAdvance);
    g.note_auto_advance();  // ep 2 auto-started (streak 1)

    CHECK(g.decide() == EosAdvance::AutoAdvance);
    g.note_auto_advance();  // ep 3 auto-started (streak 2)

    CHECK(g.decide() == EosAdvance::AutoAdvance);
    g.note_auto_advance();  // ep 4 auto-started (streak 3)

    // Ep 4 ends: three episodes played with no sign of life — countdown #4
    // is replaced by the prompt.
    CHECK(g.streak == 3);
    CHECK(g.decide() == EosAdvance::Prompt);
}

TEST_CASE("interaction mid-episode resets the streak", "[still_watching]") {
    StillWatchingGuard g;
    g.note_session_start();
    g.note_auto_advance();  // ep 2 auto-started
    g.note_auto_advance();  // ep 3 auto-started (streak 2)

    // A seek during ep 3 proves presence — streak restarts.
    g.note_user_interaction();
    CHECK(g.streak == 0);

    // No prompt until THREE MORE silent auto-advances.
    CHECK(g.decide() == EosAdvance::AutoAdvance);
    g.note_auto_advance();
    CHECK(g.decide() == EosAdvance::AutoAdvance);
    g.note_auto_advance();
    CHECK(g.decide() == EosAdvance::AutoAdvance);
    g.note_auto_advance();
    CHECK(g.decide() == EosAdvance::Prompt);
}

TEST_CASE("prompt Continue resets — they're awake", "[still_watching]") {
    StillWatchingGuard g;
    g.note_session_start();
    g.note_auto_advance();
    g.note_auto_advance();
    g.note_auto_advance();
    REQUIRE(g.decide() == EosAdvance::Prompt);

    // Continue is a press — the same reset as any other interaction. The
    // next three episode-ends auto-advance again before the next prompt.
    g.note_user_interaction();
    CHECK(g.streak == 0);
    CHECK(g.decide() == EosAdvance::AutoAdvance);
}

TEST_CASE("countdown 'Play now' press resets, expiry increments",
          "[still_watching]") {
    // The distinction the screen enforces: only the SILENT countdown
    // expiry counts as an auto-start. A "Play now" press is a manual
    // start and maps to note_user_interaction().
    StillWatchingGuard g;
    g.note_session_start();
    g.note_auto_advance();       // expiry: streak 1
    g.note_auto_advance();       // expiry: streak 2
    g.note_user_interaction();   // "Play now" pressed on countdown #3
    CHECK(g.streak == 0);
    CHECK(g.decide() == EosAdvance::AutoAdvance);
}

TEST_CASE("a fresh session never inherits a streak", "[still_watching]") {
    StillWatchingGuard g;
    g.note_auto_advance();
    g.note_auto_advance();
    g.note_auto_advance();
    REQUIRE(g.decide() == EosAdvance::Prompt);
    // Leaving playback and starting again from the picker is a manual
    // start (PlaybackScreen::enter() fires this).
    g.note_session_start();
    CHECK(g.decide() == EosAdvance::AutoAdvance);
}

TEST_CASE("prompt timeout predicate", "[still_watching]") {
    // Timeout means STOP playback (via the screen's normal exit path);
    // one shared number between screen and test.
    CHECK_FALSE(StillWatchingGuard::prompt_timed_out(0.0));
    CHECK_FALSE(StillWatchingGuard::prompt_timed_out(59.9));
    CHECK(StillWatchingGuard::prompt_timed_out(60.0));
    CHECK(StillWatchingGuard::prompt_timed_out(61.5));
}

TEST_CASE("still-watching title copy is pinned", "[still_watching]") {
    CHECK(still_watching_title("Game of Thrones") ==
          "Still watching Game of Thrones?");
    // Empty series title degrades cleanly — no dangling space.
    CHECK(still_watching_title("") == "Still watching?");
}

TEST_CASE("make_still_watching_overlay preserves the countdown's target",
          "[still_watching]") {
    // Build a REAL countdown model through decide_end_overlay, then
    // convert — the prompt must keep playing exactly what the countdown
    // would have played.
    std::vector<FakeEp> eps = {
        FakeEp{1, 4, true, "The Shadow"},
        FakeEp{1, 5, true, "The Wolf and the Lion"}};
    std::vector<SeasonRow> rows = {season_row(1, SeasonState::Complete, true, 5)};
    auto countdown =
        decide_end_overlay(rows, eps, watch_map{}, eps[0], "Game of Thrones");
    REQUIRE(countdown.kind == EndOverlayKind::Countdown);

    auto m = make_still_watching_overlay(countdown, "Game of Thrones");
    CHECK(m.kind == EndOverlayKind::StillWatching);
    CHECK(m.title_line == "Still watching Game of Thrones?");
    // The countdown's next-episode line becomes the body — the card still
    // says what a Continue press starts.
    CHECK(m.body_line == "Next: S1E5 \xC2\xB7 The Wolf and the Lion");
    CHECK(m.primary_label == "Continue");
    CHECK(m.has_primary);
    // next_index survives the conversion — Continue advances to the same
    // episode the countdown targeted.
    CHECK(m.next_index == countdown.next_index);
    CHECK(m.next_index == 1);
}
