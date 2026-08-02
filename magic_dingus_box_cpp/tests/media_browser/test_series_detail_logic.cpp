#include <catch2/catch_test_macros.hpp>

#include "media_browser/ui/series_detail_logic.h"

using namespace media_browser;
using namespace media_browser::ui;

namespace {
TmdbTvSeason tv_season(int n, int eps) {
    TmdbTvSeason s;
    s.season_number = n;
    s.episode_count = eps;
    return s;
}
Season sonarr_season(int n, bool mon, int eps, int files, int64_t bytes) {
    return Season{n, mon, eps, files, bytes};
}
}  // namespace

TEST_CASE("season rows: TMDB-only base, specials excluded, sorted", "[series_detail]") {
    std::vector<TmdbTvSeason> tmdb = {tv_season(2, 13), tv_season(0, 5), tv_season(1, 7)};
    auto rows = merge_season_rows(tmdb, nullptr, {});
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].season_number == 1);
    CHECK(rows[0].episode_count == 7);
    CHECK_FALSE(rows[0].monitored);
    CHECK(rows[0].episode_file_count == 0);
    CHECK(rows[0].state == SeasonState::None);
    CHECK(rows[1].season_number == 2);
}

TEST_CASE("season rows: Sonarr overlay wins on stats; unsettled keeps TMDB base",
          "[series_detail]") {
    std::vector<TmdbTvSeason> tmdb = {tv_season(1, 7), tv_season(2, 13)};

    SECTION("settled series overlays counts, files, monitored; extra season appended") {
        Series s;
        s.seasons = {sonarr_season(0, false, 5, 0, 0),
                     sonarr_season(1, true, 7, 7, 1),
                     sonarr_season(3, false, 10, 0, 0)};
        auto rows = merge_season_rows(tmdb, &s, {});
        REQUIRE(rows.size() == 3);  // 1, 2 (tmdb-only), 3 (sonarr-only); specials dropped
        CHECK(rows[0].monitored);
        CHECK(rows[0].episode_file_count == 7);
        CHECK(rows[0].state == SeasonState::Complete);
        CHECK(rows[1].season_number == 2);
        CHECK_FALSE(rows[1].monitored);
        CHECK(rows[2].season_number == 3);
        CHECK(rows[2].episode_count == 10);
    }

    SECTION("settled==false contract: empty sonarr seasons ⇒ rows are the TMDB base, never empty") {
        Series s;  // add_series with settled==false: seasons ALWAYS empty
        REQUIRE(s.seasons.empty());
        auto rows = merge_season_rows(tmdb, &s, {});
        REQUIRE(rows.size() == 2);  // the "0 seasons" hazard, pinned
        CHECK(rows[0].episode_count == 7);
    }
}

TEST_CASE("season state precedence: downloading > complete > partial > none",
          "[series_detail]") {
    CHECK(decide_season_state(10, 10, true) == SeasonState::Downloading);
    CHECK(decide_season_state(10, 10, false) == SeasonState::Complete);
    CHECK(decide_season_state(10, 3, false) == SeasonState::Partial);
    CHECK(decide_season_state(10, 0, false) == SeasonState::None);
    // Unknown episode_count with files present must not claim Complete.
    CHECK(decide_season_state(0, 3, false) == SeasonState::Partial);
    CHECK(decide_season_state(0, 0, false) == SeasonState::None);
}

TEST_CASE("next_unmonitored_season: first ascending unmonitored, nullopt when all monitored",
          "[series_detail]") {
    std::vector<SeasonRow> rows(3);
    rows[0].season_number = 1; rows[0].monitored = true;
    rows[1].season_number = 2; rows[1].monitored = false;
    rows[2].season_number = 3; rows[2].monitored = false;
    auto next = next_unmonitored_season(rows);
    REQUIRE(next.has_value());
    CHECK(*next == 2);
    rows[1].monitored = rows[2].monitored = true;
    CHECK_FALSE(next_unmonitored_season(rows).has_value());
    CHECK_FALSE(next_unmonitored_season({}).has_value());
}

TEST_CASE("estimate_remaining_bytes: missing episodes only, with defensive fallbacks",
          "[series_detail]") {
    std::vector<SeasonRow> rows(2);
    rows[0].episode_count = 10; rows[0].episode_file_count = 7;   // 3 missing
    rows[1].episode_count = 13; rows[1].episode_file_count = 0;   // 13 missing
    // 16 eps × 45 min × 70 MB/min × 1 MiB = 16*45*70 MiB
    const int64_t expected = 16LL * 45 * 70 * 1024 * 1024;
    CHECK(estimate_remaining_bytes(rows, 45, 70.0) == expected);
    // runtime<=0 falls back to 45; rate<=0 falls back to 70 — a zero estimate
    // would silently ALLOW a whole-series add past the blocking preflight.
    // The runtime<=0 path is also the PRE-ADD path: Sonarr has no record yet,
    // so 45 is an assumption and every label built from it says "(est)".
    CHECK(estimate_remaining_bytes(rows, 0, 70.0) == expected);
    CHECK(estimate_remaining_bytes(rows, 45, 0.0) == expected);
    // files >= count clamps to zero missing, never negative
    rows[0].episode_file_count = 12; rows[1].episode_file_count = 13;
    CHECK(estimate_remaining_bytes(rows, 45, 70.0) == 0);
}

TEST_CASE("pick_preferred_mb_per_min: 1080p family max, 720p fallback, 70 default",
          "[series_detail]") {
    std::vector<QualityDefinition> defs = {
        {4, "HDTV-720p", 40.0, 60.0},
        {9, "HDTV-1080p", 70.0, 100.0},
        {3, "WEBDL-1080p", 65.0, 100.0},
    };
    CHECK(pick_preferred_mb_per_min(defs) == 70.0);
    defs.erase(defs.begin() + 1);  // drop HDTV-1080p → WEBDL-1080p (65) wins
    CHECK(pick_preferred_mb_per_min(defs) == 65.0);
    defs.erase(defs.begin() + 1);  // only 720p left
    CHECK(pick_preferred_mb_per_min(defs) == 40.0);
    CHECK(pick_preferred_mb_per_min({}) == 70.0);
    // preferred<=0 rows (unlimited/null upstream) are skipped, not returned
    CHECK(pick_preferred_mb_per_min({{9, "HDTV-1080p", 0.0, 0.0}}) == 70.0);
}

TEST_CASE("whole_series_verdict: block at free minus 20 GiB floor; only a FAILED stat warns",
          "[series_detail]") {
    const int64_t GiB = 1024LL * 1024 * 1024;
    // free 100 GiB, floor 20 → budget 80
    CHECK(whole_series_verdict(80 * GiB, 100 * GiB) == DiskVerdict::Allow);
    CHECK(whole_series_verdict(80 * GiB + 1, 100 * GiB) == DiskVerdict::Block);
    // nullopt == the stat FAILED (nothing answered) — warn, never wedge.
    CHECK(whole_series_verdict(1, std::nullopt) == DiskVerdict::WarnOnly);
    // A READING of zero is the full-disk case, which is exactly what the
    // blocking preflight exists for. Failing open here would let the one
    // situation the feature was built to catch sail straight through.
    CHECK(whole_series_verdict(1, int64_t{0}) == DiskVerdict::Block);
}

// ---------- Remove: cancel-id selection ----------
//
// The decision that decides how many cancels the orphan-proof remove issues.
// It lived inline in the kiosk-only screen TU, where its most important
// property — a 13-row season pack yields ONE cancel, not 13 — could not be
// asserted at all. Over-cancelling is not cosmetic: sibling rows 404 by
// design after the first cancel, and counting those 404s as failures aborted
// the remove AFTER the torrent was gone.

namespace {
SonarrQueueItem q_row(int id, int series_id, std::string download_id,
                      std::string title = {}) {
    SonarrQueueItem q;
    q.id = id;
    q.series_id = series_id;
    q.download_id = std::move(download_id);
    q.title = std::move(title);
    return q;
}
}  // namespace

TEST_CASE("cancel ids: a 13-row season pack yields exactly ONE cancel",
          "[series_detail]") {
    std::vector<SonarrQueueItem> queue;
    for (int i = 0; i < 13; ++i) {
        queue.push_back(q_row(100 + i, 7, "ABCDEF", "Breaking.Bad.S02.1080p"));
    }
    const auto ids = cancel_ids_for_series(queue, 7);
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == 100);  // the FIRST row of the pack, in queue order
}

TEST_CASE("cancel ids: an empty download_id dedupes on the release title",
          "[series_detail]") {
    // Mixed rows: a populated-id pack, plus rows whose download_id never made
    // it into the queue payload but which share one release title (the field
    // is documented identical across a pack's rows). Fanning those out
    // per-row would fire N cancels at ONE download and count N-1 by-design
    // 404s as failures.
    std::vector<SonarrQueueItem> queue = {
        q_row(1, 7, "HASH1", "Show.S01.1080p"),
        q_row(2, 7, "HASH1", "Show.S01.1080p"),
        q_row(3, 7, "", "Show.S03.720p"),
        q_row(4, 7, "", "Show.S03.720p"),
        q_row(5, 7, "", "Show.S03.720p"),
    };
    const auto ids = cancel_ids_for_series(queue, 7);
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == 1);
    CHECK(ids[1] == 3);
}

TEST_CASE("cancel ids: rows for OTHER series are excluded", "[series_detail]") {
    // The queue is global. Cancelling another series' download while removing
    // this one is destructive and silent — the user asked to delete one show.
    std::vector<SonarrQueueItem> queue = {
        q_row(1, 7, "MINE", "Mine.S01"),
        q_row(2, 9, "THEIRS", "Theirs.S01"),
        q_row(3, 9, "", "Theirs.S02"),
        q_row(4, 7, "MINE", "Mine.S01"),
    };
    const auto ids = cancel_ids_for_series(queue, 7);
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == 1);
    CHECK(cancel_ids_for_series(queue, 9).size() == 2);
    CHECK(cancel_ids_for_series(queue, 404).empty());
    CHECK(cancel_ids_for_series({}, 7).empty());
}

TEST_CASE("cancel ids: empty download_ids with DISTINCT titles stay separate",
          "[series_detail]") {
    // The title fallback must not over-collapse: these are three genuinely
    // different downloads, and skipping two of them would leave live torrents
    // running for a series the user just deleted.
    std::vector<SonarrQueueItem> queue = {
        q_row(1, 7, "", "Show.S01E01.720p"),
        q_row(2, 7, "", "Show.S01E02.720p"),
        q_row(3, 7, "", "Show.S01E03.720p"),
    };
    const auto ids = cancel_ids_for_series(queue, 7);
    REQUIRE(ids.size() == 3);
    CHECK(ids[0] == 1);
    CHECK(ids[2] == 3);
    // Degenerate: no download_id AND no title is unkeyed, so each row is
    // taken as-is rather than collapsing onto one empty key.
    std::vector<SonarrQueueItem> unkeyed = {q_row(8, 7, "", ""),
                                            q_row(9, 7, "", "")};
    CHECK(cancel_ids_for_series(unkeyed, 7).size() == 2);
}

TEST_CASE("series detail state resolver: precedence and copy", "[series_detail]") {
    SeriesDetailInputs in;
    in.tmdb_done = false;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::Loading);
    in.tmdb_done = true; in.tmdb_ok = false;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::TmdbError);
    in.tmdb_ok = true; in.sonarr_configured = false;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::NotConfigured);
    in.sonarr_configured = true; in.sonarr_done = false;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::Loading);
    in.sonarr_done = true; in.sonarr_ok = false;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::SonarrUnreachable);
    in.sonarr_ok = true; in.in_library = false;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::NotInLibrary);
    in.in_library = true;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::InLibrary);

    // Copy: the NotConfigured string is the SAME literal browse ships —
    // "TV library not set up on this box" — so the two screens never disagree.
    CHECK(std::string(series_detail_state_message(SeriesDetailState::NotConfigured))
          == "TV library not set up on this box");
    CHECK(std::string(series_detail_state_message(SeriesDetailState::SonarrUnreachable))
          == "Sonarr service offline");
    CHECK(series_detail_state_message(SeriesDetailState::NotInLibrary) == nullptr);
    CHECK(series_detail_state_message(SeriesDetailState::InLibrary) == nullptr);
}

// ---------- Action row ----------
//
// decide_action_row is the function whose focus algebra was a plan-time
// Critical (the whole-series confirm was never the focused button, so SELECT
// inside the 4 s window fired "Add Season 1"). It lived untested inside the
// kiosk-only screen TU until the Task-5 review; these cases pin the four
// rules it encodes — WHICH buttons, their LABELS, keep-focus-by-identity,
// and the bias away from Remove.

namespace {
ActionRowInputs row_inputs(SeriesDetailState st) {
    ActionRowInputs in;
    in.state = st;
    return in;
}
}  // namespace

TEST_CASE("action row: not-in-library offers the add pair, focused on add",
          "[series_detail]") {
    auto in = row_inputs(SeriesDetailState::NotInLibrary);
    const auto row = decide_action_row(in);
    REQUIRE(row.buttons.size() == 2);
    CHECK(row.buttons[0].action == Action::AddSeason1);
    CHECK(row.buttons[0].label == "Add Season 1");
    CHECK(row.buttons[1].action == Action::WholeSeries);
    CHECK(row.buttons[1].label == "Whole series\xE2\x80\xA6");
    CHECK(row.focus == 0);
}

TEST_CASE("action row: an UNSETTLED in-library record is [Remove] only",
          "[series_detail]") {
    // The window where Sonarr holds the record but has never refreshed it:
    // every season reads unmonitored, so next_unmonitored answers the first
    // season we JUST added. Offering "Download Season 1" one second after
    // adding season 1 is the bug this rule exists for.
    auto in = row_inputs(SeriesDetailState::InLibrary);
    in.series_settled = false;
    in.next_unmonitored = 1;
    const auto row = decide_action_row(in);
    REQUIRE(row.buttons.size() == 1);
    CHECK(row.buttons[0].action == Action::Remove);
    CHECK(row.focus == 0);
}

TEST_CASE("action row: a settled in-library record offers next season + whole + remove",
          "[series_detail]") {
    auto in = row_inputs(SeriesDetailState::InLibrary);
    in.series_settled = true;
    in.next_unmonitored = 2;
    const auto row = decide_action_row(in);
    REQUIRE(row.buttons.size() == 3);
    CHECK(row.buttons[0].action == Action::NextSeason);
    CHECK(row.buttons[0].label == "Download Season 2");
    CHECK(row.buttons[1].action == Action::WholeSeries);
    CHECK(row.buttons[2].action == Action::Remove);
    CHECK(row.focus == 0);  // never the destructive one

    // Everything monitored: the add controls hide, Remove stays.
    in.next_unmonitored = std::nullopt;
    const auto only_remove = decide_action_row(in);
    REQUIRE(only_remove.buttons.size() == 1);
    CHECK(only_remove.buttons[0].action == Action::Remove);
}

TEST_CASE("action row: arming a confirm keeps focus on the SAME canonical button",
          "[series_detail]") {
    auto in = row_inputs(SeriesDetailState::InLibrary);
    in.series_settled = true;
    in.next_unmonitored = 2;  // row is [NextSeason, WholeSeries, Remove]

    // Remove -> ConfirmRemove: one button in two states, focus must not move.
    in.prev_focus_action = Action::Remove;
    in.remove_pending = true;
    const auto armed = decide_action_row(in);
    REQUIRE(armed.buttons.size() == 3);
    CHECK(armed.buttons[2].action == Action::ConfirmRemove);
    CHECK(armed.buttons[2].label == "Confirm Remove");
    CHECK(armed.focus == 2);

    // ...and back again when the confirm expires.
    in.prev_focus_action = Action::ConfirmRemove;
    in.remove_pending = false;
    const auto disarmed = decide_action_row(in);
    CHECK(disarmed.buttons[2].action == Action::Remove);
    CHECK(disarmed.focus == 2);

    // The whole-series arm is a relabel of the button the user pressed; the
    // 4 s confirm is unreachable if focus slides off it.
    in.prev_focus_action = Action::WholeSeries;
    in.whole_armed = true;
    in.whole_estimate_bytes = 12LL * 1024 * 1024 * 1024;
    const auto whole = decide_action_row(in);
    CHECK(whole.buttons[1].action == Action::WholeSeries);
    CHECK(whole.focus == 1);
}

TEST_CASE("action row: focus biases AWAY from Remove when the kept action is gone",
          "[series_detail]") {
    // The user pressed Add on a not-in-library page; the add landed and the
    // row became the in-library set. AddSeason1 no longer exists.
    auto in = row_inputs(SeriesDetailState::InLibrary);
    in.series_settled = true;
    in.next_unmonitored = 2;
    in.prev_focus_action = Action::AddSeason1;
    const auto row = decide_action_row(in);
    REQUIRE(row.buttons.size() == 3);
    CHECK(row.focus == 0);
    CHECK(row.buttons[row.focus].action != Action::Remove);
}

TEST_CASE("action row: [Remove]-only falls back onto Remove itself",
          "[series_detail]") {
    // The bias has to yield when the destructive button is genuinely the
    // only thing on offer — otherwise focus points at nothing.
    auto in = row_inputs(SeriesDetailState::InLibrary);
    in.series_settled = false;
    in.next_unmonitored = 1;
    in.prev_focus_action = Action::AddSeason1;  // no match in the new row
    const auto row = decide_action_row(in);
    REQUIRE(row.buttons.size() == 1);
    CHECK(row.buttons[0].action == Action::Remove);
    CHECK(row.focus == 0);
}

TEST_CASE("action row: a FORCED [Remove]-only focus is not preserved when the row re-expands",
          "[series_detail]") {
    // The three-step sequence that put the ring on the delete button:
    //
    //   1. NotInLibrary, focus on "Add Season 1" — the user presses it.
    //   2. The unsettled drain collapses the row to [Remove]. Focus is FORCED
    //      there: it is the only button, not a choice.
    //   3. ~9 s later the poll settles and the row re-expands to
    //      [NextSeason, WholeSeries, Remove]. Preserving the identity from
    //      step 2 leaves Remove focused right as the waiting user taps SELECT
    //      — which arms ConfirmRemove, and a second tap within 2 s deletes the
    //      series WITH ITS FILES.
    //
    // Step 3 must land on NextSeason (index 0) instead.

    // --- step 1 ---
    auto s1 = row_inputs(SeriesDetailState::NotInLibrary);
    const auto add_row = decide_action_row(s1);
    REQUIRE(add_row.buttons.size() == 2);
    REQUIRE(add_row.buttons[add_row.focus].action == Action::AddSeason1);

    // --- step 2: the unsettled drain ---
    auto s2 = row_inputs(SeriesDetailState::InLibrary);
    s2.series_settled = false;
    s2.next_unmonitored = 1;
    s2.prev_focus_action = add_row.buttons[add_row.focus].action;
    s2.prev_row_remove_only = false;  // the previous row was the add pair
    const auto forced = decide_action_row(s2);
    REQUIRE(forced.buttons.size() == 1);
    REQUIRE(forced.buttons[0].action == Action::Remove);
    REQUIRE(forced.focus == 0);  // forced, not chosen

    // --- step 3: the poll settles and the row re-expands ---
    auto s3 = row_inputs(SeriesDetailState::InLibrary);
    s3.series_settled = true;
    s3.next_unmonitored = 2;
    s3.prev_focus_action = forced.buttons[forced.focus].action;  // Remove
    s3.prev_row_remove_only = true;  // ...and it was the ONLY button
    const auto settled = decide_action_row(s3);
    REQUIRE(settled.buttons.size() == 3);
    CHECK(settled.focus == 0);
    CHECK(settled.buttons[settled.focus].action == Action::NextSeason);

    // The counterfactual: without the flag the identity loop preserves Remove
    // and the ring lands on the delete button. This is the bug, pinned.
    s3.prev_row_remove_only = false;
    const auto unguarded = decide_action_row(s3);
    CHECK(unguarded.focus == 2);
    CHECK(unguarded.buttons[unguarded.focus].action == Action::Remove);
}

TEST_CASE("action row: the non-actionable states have an EMPTY row",
          "[series_detail]") {
    // Loading / TmdbError / NotConfigured / SonarrUnreachable: no buttons at
    // all, so a SELECT is a structural no-op rather than a silent add.
    for (auto st : {SeriesDetailState::Loading, SeriesDetailState::TmdbError,
                    SeriesDetailState::NotConfigured,
                    SeriesDetailState::SonarrUnreachable}) {
        auto in = row_inputs(st);
        in.next_unmonitored = 1;
        in.prev_focus_action = Action::Remove;
        const auto row = decide_action_row(in);
        CHECK(row.buttons.empty());
        CHECK(row.focus == 0);
    }
}

TEST_CASE("action row: the armed whole-series label carries the GiB estimate",
          "[series_detail]") {
    CHECK(whole_series_label(false, 0) == "Whole series\xE2\x80\xA6");
    // Binary GB, and "(est)" because pre-add the runtime is an assumption.
    CHECK(whole_series_label(true, 42LL * 1024 * 1024 * 1024) ==
          "Confirm ~42 GB (est)");
    // Truncation, not rounding: 41.9 GiB must never read as "42 GB".
    CHECK(whole_series_label(true, 42LL * 1024 * 1024 * 1024 - 1) ==
          "Confirm ~41 GB (est)");
    // The label the row actually carries when armed.
    auto in = row_inputs(SeriesDetailState::NotInLibrary);
    in.whole_armed = true;
    in.whole_estimate_bytes = 7LL * 1024 * 1024 * 1024;
    const auto row = decide_action_row(in);
    REQUIRE(row.buttons.size() == 2);
    CHECK(row.buttons[1].label == "Confirm ~7 GB (est)");
}
