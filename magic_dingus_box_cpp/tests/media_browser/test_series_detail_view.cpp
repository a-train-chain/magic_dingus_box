// Table tests for series_detail_view.h — SeriesDetailScreen's view-side
// decisions (row text and tones, the two paged lists, the footer, and the
// rotary / paging / SELECT input mapping), moved out of
// series_detail_screen.cpp so they run on the Mac.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "media_browser/ui/series_detail_view.h"

namespace mb = media_browser;
using namespace media_browser::ui;

namespace {

const std::string kDash = "\xE2\x80\x94";
const std::string kDot = " \xC2\xB7 ";

SeasonRow row(int n, int count, int files, bool monitored, SeasonState st) {
    SeasonRow r;
    r.season_number = n;
    r.episode_count = count;
    r.episode_file_count = files;
    r.monitored = monitored;
    r.state = st;
    return r;
}

mb::EpisodeInfo ep(int season, int number, const std::string& title,
                   bool has_file, int runtime = 0) {
    mb::EpisodeInfo e;
    e.season_number = season;
    e.episode_number = number;
    e.title = title;
    e.has_file = has_file;
    e.runtime_minutes = runtime;
    return e;
}

}  // namespace

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------

TEST_CASE("series view: lookups", "[series_detail_view]") {
    const std::vector<SeasonRow> rows{row(1, 10, 10, true, SeasonState::Complete),
                                      row(2, 8, 0, false, SeasonState::None),
                                      row(3, 8, 2, false, SeasonState::Partial)};
    REQUIRE(find_season_row(rows, 3) != nullptr);
    CHECK(find_season_row(rows, 3)->episode_file_count == 2);
    CHECK(find_season_row(rows, 9) == nullptr);
    CHECK(unmonitored_seasons(rows) == std::vector<int>{2, 3});

    const std::vector<mb::EpisodeInfo> eps{ep(1, 1, "a", true), ep(2, 1, "b", false),
                                           ep(1, 2, "c", true)};
    CHECK(season_episode_indices(eps, 1) == std::vector<int>{0, 2});
    CHECK(season_episode_indices(eps, 5).empty());

    // ONE season's estimate == estimate_remaining_bytes over that row alone.
    CHECK(season_estimate_bytes(rows, 2, 45, 70.0) ==
          estimate_remaining_bytes({rows[1]}, 45, 70.0));
    CHECK(season_estimate_bytes(rows, 9, 45, 70.0) == 0);
}

TEST_CASE("series view: runtime fallback, display title, genres",
          "[series_detail_view]") {
    CHECK(episode_runtime_minutes(ep(1, 1, "x", true, 52), 45) == 52);
    CHECK(episode_runtime_minutes(ep(1, 1, "x", true, 0), 45) == 45);
    CHECK(episode_runtime_minutes(ep(1, 1, "x", true, 0), 0) == 0);

    CHECK(series_episode_display_title("Show", ep(2, 5, "Pilot", true)) ==
          "Show " + kDash + " S2E5" + kDot + "Pilot");

    CHECK(join_genres({}) == "");
    CHECK(join_genres({"Drama"}) == "Drama");
    CHECK(join_genres({"A", "B", "C", "D"}) == "A" + kDot + "B" + kDot + "C");
}

TEST_CASE("series view: watch-map helpers", "[series_detail_view]") {
    watch_map w;
    CHECK(next_up_is_first(w));
    w[WatchKey{1, 1}] = WatchRowLite{30.0, 3000.0, false};  // not resumable (<60 s)
    CHECK(next_up_is_first(w));
    CHECK(episode_resume_position(w, 1, 1) == 0.0);
    w[WatchKey{1, 2}] = WatchRowLite{600.0, 3000.0, false};
    CHECK_FALSE(next_up_is_first(w));
    CHECK(episode_resume_position(w, 1, 2) == 600.0);
    CHECK(episode_resume_position(w, 4, 4) == 0.0);
    watch_map watched;
    watched[WatchKey{1, 1}] = WatchRowLite{0.0, 0.0, true};
    CHECK_FALSE(next_up_is_first(watched));
}

// ---------------------------------------------------------------------------
// Action row
// ---------------------------------------------------------------------------

TEST_CASE("series view: primary action, remove-only row, button kind",
          "[series_detail_view]") {
    CHECK(is_primary_season_action(Action::AddSeason));
    CHECK(is_primary_season_action(Action::NextSeason));
    CHECK_FALSE(is_primary_season_action(Action::WholeSeries));
    CHECK_FALSE(is_primary_season_action(Action::PlayNextUp));

    CHECK_FALSE(row_is_remove_only({}));
    CHECK(row_is_remove_only({{Action::Remove, "Remove"}}));
    CHECK(row_is_remove_only({{Action::ConfirmRemove, "Confirm Remove"}}));
    CHECK_FALSE(row_is_remove_only({{Action::NextSeason, "x"}, {Action::Remove, "y"}}));

    using K = chrome::ButtonKind;
    CHECK(series_button_kind(Action::Remove, false, false) == K::Warn);
    CHECK(series_button_kind(Action::ConfirmRemove, false, false) == K::Warn);
    CHECK(series_button_kind(Action::WholeSeries, true, false) == K::Warn);
    CHECK(series_button_kind(Action::WholeSeries, false, false) == K::Action);
    CHECK(series_button_kind(Action::NextSeason, false, true) == K::Action);
    CHECK(series_button_kind(Action::AddSeason, false, true) == K::Action);
    CHECK(series_button_kind(Action::NextSeason, false, false) == K::Ok);
    CHECK(series_button_kind(Action::PlayNextUp, true, true) == K::Ok);
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

TEST_CASE("series view: meta line", "[series_detail_view]") {
    mb::TmdbTvDetail d;
    d.year = 2008;
    d.number_of_seasons = 5;
    d.number_of_episodes = 62;
    d.status = "Ended";
    CHECK(series_meta_line(d, false, true) ==
          "2008" + kDot + "5 seasons" + kDot + "62 episodes" + kDot + "Ended");
    CHECK(series_meta_line(d, true, false) ==
          "2008" + kDot + "5 seasons" + kDot + "62 episodes" + kDot + "Ended" +
              kDot + "syncing\xE2\x80\xA6");
    // Not in the library: an unsettled flag says nothing.
    CHECK(series_meta_line(d, false, false).find("syncing") == std::string::npos);
    d.number_of_seasons = 1;
    d.status.clear();
    CHECK(series_meta_line(d, true, true) ==
          "2008" + kDot + "1 season" + kDot + "62 episodes");
}

TEST_CASE("series view: season row text and tone", "[series_detail_view]") {
    auto v = season_row_view(row(3, 8, 2, false, SeasonState::Partial));
    CHECK(v.label == "Season 3");
    CHECK(v.counts == "2/8 eps");
    CHECK(v.state == "partial");
    CHECK(v.state_tone == MbTone::Accent);

    v = season_row_view(row(1, 8, 0, true, SeasonState::None));
    CHECK(v.state == "monitored");
    CHECK(v.state_tone == MbTone::Dim);
    v = season_row_view(row(1, 8, 0, false, SeasonState::None));
    CHECK(v.state == kDash);
    v = season_row_view(row(1, 8, 0, true, SeasonState::Downloading));
    CHECK(v.state == "downloading");
    CHECK(v.state_tone == MbTone::Highlight2);
    v = season_row_view(row(1, 8, 8, true, SeasonState::Complete));
    CHECK(v.state == "complete");
    CHECK(v.state_tone == MbTone::Highlight1);
}

TEST_CASE("series view: episode glyph", "[series_detail_view]") {
    watch_map w;
    // Fileless: no glyph at all, whatever the watch map says.
    w[WatchKey{1, 1}] = WatchRowLite{0, 0, true};
    CHECK(episode_glyph_view(ep(1, 1, "a", false), w).kind == EpisodeGlyph::None);

    auto g = episode_glyph_view(ep(1, 1, "a", true), w);
    CHECK(g.kind == EpisodeGlyph::Watched);
    CHECK(g.text == "\xE2\x9C\x93");
    CHECK(g.tone == MbTone::Highlight1);

    // Watched by POSITION (>= 92%) without the flag.
    w[WatchKey{1, 2}] = WatchRowLite{2900.0, 3000.0, false};
    CHECK(episode_glyph_view(ep(1, 2, "b", true), w).kind == EpisodeGlyph::Watched);

    w[WatchKey{1, 3}] = WatchRowLite{3723.0, 7200.0, false};
    g = episode_glyph_view(ep(1, 3, "c", true), w);
    CHECK(g.kind == EpisodeGlyph::Resume);
    CHECK(g.text == "\xE2\x96\xB6 1:02:03");
    CHECK(g.tone == MbTone::Accent);

    g = episode_glyph_view(ep(1, 4, "d", true), w);
    CHECK(g.kind == EpisodeGlyph::Unwatched);
    CHECK(g.text == "\xC2\xB7");
    CHECK(g.tone == MbTone::Dim);
}

TEST_CASE("series view: episode row text", "[series_detail_view]") {
    CHECK(episode_row_text(ep(1, 4, "Cat", true, 47), 45, true) ==
          "E4" + kDot + "Cat" + kDot + "47m");
    CHECK(episode_row_text(ep(1, 4, "Cat", true, 0), 45, false) ==
          "E4" + kDot + "Cat" + kDot + "45m");
    CHECK(episode_row_text(ep(1, 4, "Cat", true, 0), 0, false) == "E4" + kDot + "Cat");
    // The downloading suffix is for FILELESS rows of a downloading season.
    CHECK(episode_row_text(ep(1, 4, "Cat", false, 0), 0, true) ==
          "E4" + kDot + "Cat" + kDot + "downloading");
    CHECK(episode_row_text(ep(1, 4, "Cat", false, 0), 0, false) == "E4" + kDot + "Cat");
}

// ---------------------------------------------------------------------------
// Paging
// ---------------------------------------------------------------------------

TEST_CASE("series view: season list paging", "[series_detail_view]") {
    // 21 seasons, 8 per page: 3 pages; page 2 shows 16..20.
    auto p = season_list_paging(21, 8, 2, -1);
    CHECK(p.page_count == 3);
    CHECK(p.page == 2);
    CHECK(p.first == 16);
    CHECK(p.last == 21);
    CHECK(p.overflow);
    CHECK(p.focus == -1);  // the action-row ring passes through

    // A stale page clamps into range; a stranded season ring snaps in.
    p = season_list_paging(10, 8, 5, 2);
    CHECK(p.page == 1);
    CHECK(p.first == 8);
    CHECK(p.last == 10);
    CHECK(p.focus == 8);
    p = season_list_paging(10, 8, 0, 9);
    CHECK(p.focus == 7);

    // per_page 0 (CRT_NATIVE): one page, no rows, nothing divided, ring kept.
    p = season_list_paging(10, 0, 3, 4);
    CHECK(p.page_count == 1);
    CHECK(p.page == 0);
    CHECK(p.first == 0);
    CHECK(p.last == 0);
    CHECK_FALSE(p.overflow);
    CHECK(p.focus == 4);

    // Fits on one page: no overflow; an empty list is one page.
    p = season_list_paging(5, 8, 0, -1);
    CHECK_FALSE(p.overflow);
    CHECK(p.last == 5);
    p = season_list_paging(0, 8, 0, -1);
    CHECK(p.page_count == 1);
    CHECK(p.last == 0);
}

TEST_CASE("series view: episode list paging follows focus", "[series_detail_view]") {
    // 10 episodes + delete row = 11 nav rows, 4 per page.
    auto p = episode_list_paging(11, 4, 9);
    CHECK(p.page_count == 3);
    CHECK(p.page == 2);
    CHECK(p.first == 8);
    CHECK(p.last == 11);
    CHECK(p.overflow);
    CHECK(p.focus == 9);
    // Focus clamps into the chain first.
    p = episode_list_paging(11, 4, 40);
    CHECK(p.focus == 10);
    CHECK(p.page == 2);
    p = episode_list_paging(11, 4, -3);
    CHECK(p.focus == 0);
    CHECK(p.page == 0);
    // per_page 0: nothing drawn.
    p = episode_list_paging(11, 0, 5);
    CHECK(p.page_count == 1);
    CHECK(p.page == 0);
    CHECK(p.last == 0);
    CHECK_FALSE(p.overflow);
}

TEST_CASE("series view: page indicators", "[series_detail_view]") {
    CHECK(season_page_indicator(8, 16, 21) ==
          "Seasons 9\xE2\x80\x93" "16 of 21" + kDot + "[BTN1/BTN3]");
    CHECK(episode_page_indicator(0, 4, 10) ==
          "Episodes 1\xE2\x80\x93" "4 of 10" + kDot + "[BTN1/BTN3]");
    // A last page holding only the delete row never reads "11-10 of 10".
    CHECK(episode_page_indicator(10, 11, 10) ==
          "Episodes 10\xE2\x80\x93" "10 of 10" + kDot + "[BTN1/BTN3]");
}

// ---------------------------------------------------------------------------
// Delete row
// ---------------------------------------------------------------------------

TEST_CASE("series view: delete row presence and state", "[series_detail_view]") {
    CHECK(season_delete_row_present(true, true, true, 10, 3, false));
    CHECK(season_delete_row_present(true, true, true, 10, 0, true));
    CHECK_FALSE(season_delete_row_present(true, true, true, 10, 0, false));
    CHECK_FALSE(season_delete_row_present(false, true, true, 10, 3, false));
    CHECK_FALSE(season_delete_row_present(true, false, true, 10, 3, false));
    CHECK_FALSE(season_delete_row_present(true, true, false, 10, 3, false));
    CHECK_FALSE(season_delete_row_present(true, true, true, 0, 3, false));

    CHECK(season_delete_row_state(false, false) == SeasonDeleteState::Idle);
    CHECK(season_delete_row_state(false, true) == SeasonDeleteState::Armed);
    CHECK(season_delete_row_state(true, true) == SeasonDeleteState::Removing);
}

// ---------------------------------------------------------------------------
// Input mapping
// ---------------------------------------------------------------------------

TEST_CASE("series view: rotary chain over season rows then buttons",
          "[series_detail_view]") {
    // 3 rows, 2 per page, 3 buttons. Ring on button 0, rotate back one: the
    // LAST season row, and the page follows.
    auto c = rotate_season_chain(-1, 0, 0, 3, 2, 3, -1);
    REQUIRE(c.has_value());
    CHECK(c->season_focus == 2);
    CHECK(c->season_page == 1);
    CHECK(c->focus == 0);

    // From the last row forward: back onto button 0.
    c = rotate_season_chain(2, 1, 1, 3, 2, 3, +1);
    REQUIRE(c.has_value());
    CHECK(c->season_focus == -1);
    CHECK(c->focus == 0);
    CHECK(c->season_page == 1);  // unchanged when leaving the list

    // Clamp, never wrap.
    c = rotate_season_chain(0, 0, 0, 3, 2, 3, -5);
    CHECK(c->season_focus == 0);
    c = rotate_season_chain(-1, 2, 0, 3, 2, 3, +5);
    CHECK(c->season_focus == -1);
    CHECK(c->focus == 2);

    // per_page 0: rows are not in the chain; a stale season ring restarts
    // from the button row.
    c = rotate_season_chain(1, 1, 0, 3, 0, 3, -1);
    REQUIRE(c.has_value());
    CHECK(c->season_focus == -1);
    CHECK(c->focus == 0);

    // Nothing to navigate.
    CHECK_FALSE(rotate_season_chain(-1, 0, 0, 0, 2, 0, 1).has_value());
    CHECK_FALSE(rotate_season_chain(-1, 0, 0, 5, 0, 0, 1).has_value());
}

TEST_CASE("series view: season page step", "[series_detail_view]") {
    // Ring in the list moves to the page's first row.
    auto s = season_page_step(1, 3, 3, 2, 5, -1);
    CHECK(s.season_page == 0);
    CHECK(s.season_focus == 0);
    s = season_page_step(1, 3, 3, 2, 5, +1);
    CHECK(s.season_page == 2);
    CHECK(s.season_focus == 4);
    // ...clamped to the last row.
    s = season_page_step(0, 3, 0, 4, 5, +1);
    CHECK(s.season_page == 1);
    CHECK(s.season_focus == 4);
    // Ring on the action row: pages browse freely underneath.
    s = season_page_step(0, 3, -1, 2, 5, +1);
    CHECK(s.season_page == 1);
    CHECK(s.season_focus == -1);
    // Ends are inert.
    s = season_page_step(0, 3, 1, 2, 5, -1);
    CHECK(s.season_page == 0);
    CHECK(s.season_focus == 1);
    s = season_page_step(2, 3, 4, 2, 5, +1);
    CHECK(s.season_page == 2);
    CHECK(s.season_focus == 4);
}

TEST_CASE("series view: episode focus stepping", "[series_detail_view]") {
    CHECK(episode_focus_after_rotate(3, 2, 11) == 5);
    CHECK(episode_focus_after_rotate(3, 20, 11) == 10);
    CHECK(episode_focus_after_rotate(3, -20, 11) == 0);
    CHECK(episode_focus_after_rotate(3, 1, 0) == 3);

    CHECK(episode_focus_after_page(5, 4, 11, -1) == 1);
    CHECK(episode_focus_after_page(2, 4, 11, -1) == 0);
    CHECK(episode_focus_after_page(5, 4, 11, +1) == 9);
    CHECK(episode_focus_after_page(9, 4, 11, +1) == 10);
    CHECK(episode_focus_after_page(5, 0, 11, +1) == 5);
    CHECK(episode_focus_after_page(5, 4, 0, +1) == 5);
}

TEST_CASE("series view: season row SELECT", "[series_detail_view]") {
    const SeasonRow files = row(1, 8, 3, true, SeasonState::Partial);
    const SeasonRow dl = row(2, 8, 0, true, SeasonState::Downloading);
    const SeasonRow empty = row(3, 8, 0, false, SeasonState::None);
    CHECK(decide_season_row_select(0, false, files) == SeasonRowSelect::RingToButtons);
    CHECK(decide_season_row_select(0, true, files) == SeasonRowSelect::RingToButtons);
    CHECK(decide_season_row_select(4, true, files) == SeasonRowSelect::Busy);
    CHECK(decide_season_row_select(4, false, files) == SeasonRowSelect::OpenPicker);
    CHECK(decide_season_row_select(4, false, dl) == SeasonRowSelect::OpenPicker);
    CHECK(decide_season_row_select(4, false, empty) == SeasonRowSelect::StartDownload);
}

TEST_CASE("series view: episode SELECT", "[series_detail_view]") {
    EpisodeSelectInputs in;
    in.nav_count = 5;
    in.per_page = 4;
    CHECK(decide_episode_select(in) == EpisodeSelect::Play);

    auto t = in;
    t.nav_count = 0;
    CHECK(decide_episode_select(t) == EpisodeSelect::Ignore);
    t = in;
    t.per_page = 0;
    t.mut_in_flight = true;
    CHECK(decide_episode_select(t) == EpisodeSelect::Ignore);  // before Busy
    t = in;
    t.mut_in_flight = true;
    t.delete_focused = true;
    CHECK(decide_episode_select(t) == EpisodeSelect::Busy);

    t = in;
    t.delete_focused = true;
    CHECK(decide_episode_select(t) == EpisodeSelect::ArmDelete);
    t.season_del_armed = true;
    CHECK(decide_episode_select(t) == EpisodeSelect::ConfirmDelete);
    t.remove_pending = true;
    CHECK(decide_episode_select(t) == EpisodeSelect::FinishRemoveFirst);
    t.season_del_inflight = true;
    CHECK(decide_episode_select(t) == EpisodeSelect::Busy);  // before Remove
    // The delete-row gates never touch an episode row.
    t.delete_focused = false;
    CHECK(decide_episode_select(t) == EpisodeSelect::Play);
}

TEST_CASE("series view: single-season start guards", "[series_detail_view]") {
    CHECK(decide_season_start_guard(true, false, false, false) == SeasonStartGuard::Busy);
    CHECK(decide_season_start_guard(false, false, true, true) ==
          SeasonStartGuard::NotInLibrary);
    CHECK(decide_season_start_guard(false, true, false, true) ==
          SeasonStartGuard::NotInLibrary);
    CHECK(decide_season_start_guard(false, true, true, false) ==
          SeasonStartGuard::Syncing);
    CHECK(decide_season_start_guard(false, true, true, true) == SeasonStartGuard::Go);
}

// ---------------------------------------------------------------------------
// Footer + deferred start
// ---------------------------------------------------------------------------

TEST_CASE("series view: footer hints", "[series_detail_view]") {
    auto h = series_footer_hints(false, true, false, false, false);
    REQUIRE(h.size() == 6);
    CHECK(h[0].action == "Seasons \xE2\x86\x90");
    CHECK(h[1].action == "Exit");
    CHECK(h[2].action == "Seasons \xE2\x86\x92");
    CHECK(h[3].action == "Back");
    CHECK(h[4].action == "Choose");
    CHECK(h[5].action == "Select");

    h = series_footer_hints(false, false, true, false, true);
    CHECK(h[0].action == kDash);
    CHECK(h[2].action == kDash);
    CHECK(h[4].action == kDash);
    CHECK(h[5].action == kDash);

    h = series_footer_hints(true, false, true, false, true);
    CHECK(h[0].action == "Episodes \xE2\x86\x90");
    CHECK(h[2].action == "Episodes \xE2\x86\x92");
    CHECK(h[3].action == "Seasons");
    CHECK(h[4].action == "Choose");
    CHECK(h[5].action == "Play");
    h = series_footer_hints(true, true, false, true, false);
    CHECK(h[0].action == kDash);
    CHECK(h[5].action == "Select");
    CHECK(h[3].icon == chrome::HintIcon::Btn4Black);
}

TEST_CASE("series view: deferred season-start copy", "[series_detail_view]") {
    CHECK(deferred_gate_from_code(0) == DeferredGate::Pending);
    CHECK(deferred_gate_from_code(1) == DeferredGate::Ready);
    CHECK(deferred_gate_from_code(2) == DeferredGate::TimedOut);
    CHECK(deferred_gate_from_code(7) == DeferredGate::Pending);

    CHECK(std::string(season_update_didnt_apply_toast()) ==
          "Season update didn't apply " + kDash + " try from this screen");
    CHECK(deferred_waiting_toast("Show", 6) ==
          "Show: starting Season 6 once services are back\xE2\x80\xA6");
    CHECK(deferred_services_down_toast("Show", 6) ==
          "Show: Sonarr didn't come back " + kDash +
              " Season 6 not started; try from this screen");
    CHECK(deferred_not_started_toast("Show", 6) ==
          "Show: Season 6 not started " + kDash + " open the show again to start it");
    CHECK(added_start_dropped_toast("Show", 4) ==
          "Show: added " + kDash + " open the show again to start Season 4");
    CHECK(added_start_dropped_toast("", 4) ==
          "This series: added " + kDash + " open the show again to start Season 4");
}
