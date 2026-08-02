#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

#include "app/app_state.h"
#include "media_browser/ui/mb_filter_state.h"

using DS = app::AppState::DisplaySettings;
using media_browser::ui::FilterState;
using media_browser::ui::FilterTabKind;
using media_browser::ui::MbMode;
using media_browser::ui::any_filter_active;
using media_browser::ui::apply_mode_toggle;
using media_browser::ui::build_discover_filter;
using media_browser::ui::build_tv_discover_filter;
using media_browser::ui::filter_genre_count;
using media_browser::ui::filter_genre_display;
using media_browser::ui::filter_genre_ids;
using media_browser::ui::filter_runtime_labels;
using media_browser::ui::mode_row_value_label;
using media_browser::ui::read_filter_state;
using media_browser::ui::write_filter_state;

namespace {
FilterState fs_with(uint32_t mask, int decade, int rating, int runtime,
                    int language, int sort) {
    FilterState fs;
    fs.genre_mask = mask;
    fs.decade = decade;
    fs.min_rating = rating;
    fs.runtime = runtime;
    fs.language = language;
    fs.sort = sort;
    return fs;
}
bool has_id(const std::vector<int>& v, int id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}
}  // namespace

TEST_CASE("filter state: the four (mode,tab) slots never cross-contaminate",
          "[filter_state]") {
    DS s;
    write_filter_state(s, MbMode::Movies, FilterTabKind::Popular,
                       fs_with(1u, 1, 1, 1, 1, 1));
    write_filter_state(s, MbMode::Tv, FilterTabKind::Popular,
                       fs_with(2u, 2, 2, 2, 2, 2));
    write_filter_state(s, MbMode::Movies, FilterTabKind::TopRated,
                       fs_with(4u, 3, 3, 3, 3, 3));
    write_filter_state(s, MbMode::Tv, FilterTabKind::TopRated,
                       fs_with(8u, 4, 0, 4, 4, 0));

    CHECK(read_filter_state(s, MbMode::Movies, FilterTabKind::Popular).genre_mask == 1u);
    CHECK(read_filter_state(s, MbMode::Tv, FilterTabKind::Popular).genre_mask == 2u);
    CHECK(read_filter_state(s, MbMode::Movies, FilterTabKind::TopRated).genre_mask == 4u);
    CHECK(read_filter_state(s, MbMode::Tv, FilterTabKind::TopRated).genre_mask == 8u);
    CHECK(read_filter_state(s, MbMode::Movies, FilterTabKind::Popular).decade == 1);
    CHECK(read_filter_state(s, MbMode::Tv, FilterTabKind::Popular).decade == 2);
}

TEST_CASE("filter state: ForYou reads default and never writes",
          "[filter_state]") {
    DS s;
    write_filter_state(s, MbMode::Movies, FilterTabKind::Popular, fs_with(1u, 1, 1, 1, 1, 1));
    write_filter_state(s, MbMode::Movies, FilterTabKind::ForYou, fs_with(9u, 5, 3, 4, 8, 2));
    // The ForYou write is a no-op — neither chart tab moved.
    CHECK(read_filter_state(s, MbMode::Movies, FilterTabKind::Popular).genre_mask == 1u);
    CHECK(read_filter_state(s, MbMode::Movies, FilterTabKind::TopRated).genre_mask == 0u);
    // And reading ForYou yields a default state in both modes.
    CHECK(read_filter_state(s, MbMode::Movies, FilterTabKind::ForYou) == FilterState{});
    CHECK(read_filter_state(s, MbMode::Tv, FilterTabKind::ForYou) == FilterState{});
}

TEST_CASE("genre catalogs: movie and TV id spaces are separate",
          "[filter_state]") {
    const auto& movie_ids = filter_genre_ids(MbMode::Movies);
    const auto& tv_ids    = filter_genre_ids(MbMode::Tv);
    CHECK(filter_genre_count(MbMode::Movies) == 18);
    CHECK(filter_genre_count(MbMode::Tv) == 16);
    // TV-only ids.
    CHECK(has_id(tv_ids, 10759));      // Action & Adventure
    CHECK(has_id(tv_ids, 10765));      // Sci-Fi & Fantasy
    CHECK_FALSE(has_id(movie_ids, 10759));
    CHECK_FALSE(has_id(movie_ids, 10765));
    // Movie ids that are invalid for TV must not appear in the TV catalog.
    for (int invalid : {28, 12, 14, 27, 36, 53, 878, 10402, 10749, 10752, 10770}) {
        CHECK_FALSE(has_id(tv_ids, invalid));
    }
    // The movie catalog is byte-for-byte what shipped.
    CHECK(movie_ids.front() == 28);    // Action
    CHECK(movie_ids.back() == 10752);  // War
}

TEST_CASE("genre display: a mask bit names the active mode's genre",
          "[filter_state]") {
    CHECK(std::string(filter_genre_display(MbMode::Movies, 0u)) == "All");
    CHECK(std::string(filter_genre_display(MbMode::Tv, 0u)) == "All");
    CHECK(std::string(filter_genre_display(MbMode::Movies, 1u)) == "Action");
    CHECK(std::string(filter_genre_display(MbMode::Tv, 1u)) == "Action & Adv");
    // Bit past the end of the shorter TV catalog does not read out of bounds.
    CHECK(std::string(filter_genre_display(MbMode::Tv, 1u << 17)) == "?");
}

TEST_CASE("mode row value label", "[filter_state]") {
    CHECK(std::string(mode_row_value_label(MbMode::Movies)) == "MOVIES");
    CHECK(std::string(mode_row_value_label(MbMode::Tv)) == "TV");
}

TEST_CASE("runtime labels differ per mode (TV runtime is per-episode)",
          "[filter_state]") {
    CHECK(std::string(filter_runtime_labels(MbMode::Movies)[1]) == "<90m");
    CHECK(std::string(filter_runtime_labels(MbMode::Tv)[1]) == "<30m");
    CHECK(std::string(filter_runtime_labels(MbMode::Movies)[0]) == "Any");
    CHECK(std::string(filter_runtime_labels(MbMode::Tv)[0]) == "Any");
    CHECK(std::string(filter_runtime_labels(MbMode::Tv)[4]) == "60m+");
}

TEST_CASE("any_filter_active: per-tab sort default", "[filter_state]") {
    CHECK_FALSE(any_filter_active(FilterState{}, FilterTabKind::Popular));
    FilterState top_default;
    top_default.sort = static_cast<int>(DS::MbDiscoverSort::TopRated);
    CHECK_FALSE(any_filter_active(top_default, FilterTabKind::TopRated));
    CHECK(any_filter_active(top_default, FilterTabKind::Popular));
    FilterState genre_only;
    genre_only.genre_mask = 1u;
    CHECK(any_filter_active(genre_only, FilterTabKind::Popular));
}

TEST_CASE("build_discover_filter: movie behavior is unchanged",
          "[filter_state]") {
    FilterState fs = fs_with(/*mask=*/1u,
                             static_cast<int>(DS::MbDecade::D1990s),
                             static_cast<int>(DS::MbMinRating::Seven),
                             static_cast<int>(DS::MbRuntime::Range90To120),
                             static_cast<int>(DS::MbLanguage::Japanese),
                             static_cast<int>(DS::MbDiscoverSort::RecentRelease));
    const auto df = build_discover_filter(fs, FilterTabKind::Popular);
    REQUIRE(df.genre_ids.size() == 1);
    CHECK(df.genre_ids[0] == 28);
    CHECK(df.primary_release_year_gte.value() == 1990);
    CHECK(df.primary_release_year_lte.value() == 1999);
    CHECK(df.vote_average_gte.value() == 7.0f);
    CHECK(df.with_runtime_gte.value() == 90);
    CHECK(df.with_runtime_lte.value() == 120);
    CHECK(df.with_original_language.value() == "ja");
    CHECK(df.sort_by == "primary_release_date.desc");
    CHECK(df.vote_count_gte.value() == 200);
    CHECK(build_discover_filter(fs, FilterTabKind::TopRated).vote_count_gte.value() == 300);
}

TEST_CASE("build_tv_discover_filter: decade maps to first_air_date",
          "[filter_state]") {
    FilterState fs;
    fs.decade = static_cast<int>(DS::MbDecade::D2010s);
    const auto tf = build_tv_discover_filter(fs, FilterTabKind::Popular);
    CHECK(tf.first_air_date_year_gte.value() == 2010);
    CHECK(tf.first_air_date_year_lte.value() == 2019);
    FilterState classic;
    classic.decade = static_cast<int>(DS::MbDecade::Classic);
    const auto ct = build_tv_discover_filter(classic, FilterTabKind::Popular);
    CHECK_FALSE(ct.first_air_date_year_gte.has_value());
    CHECK(ct.first_air_date_year_lte.value() == 1969);
}

TEST_CASE("build_tv_discover_filter: RecentRelease sorts by first_air_date",
          "[filter_state]") {
    FilterState fs;
    fs.sort = static_cast<int>(DS::MbDiscoverSort::RecentRelease);
    CHECK(build_tv_discover_filter(fs, FilterTabKind::Popular).sort_by
          == "first_air_date.desc");
    fs.sort = static_cast<int>(DS::MbDiscoverSort::TopRated);
    CHECK(build_tv_discover_filter(fs, FilterTabKind::Popular).sort_by
          == "vote_average.desc");
    fs.sort = static_cast<int>(DS::MbDiscoverSort::MostVoted);
    CHECK(build_tv_discover_filter(fs, FilterTabKind::Popular).sort_by
          == "vote_count.desc");
    fs.sort = static_cast<int>(DS::MbDiscoverSort::Popularity);
    CHECK(build_tv_discover_filter(fs, FilterTabKind::Popular).sort_by
          == "popularity.desc");
}

TEST_CASE("build_tv_discover_filter: runtime bands are per-episode",
          "[filter_state]") {
    auto band = [](DS::MbRuntime r) {
        FilterState fs;
        fs.runtime = static_cast<int>(r);
        return build_tv_discover_filter(fs, FilterTabKind::Popular);
    };
    const auto under = band(DS::MbRuntime::Under90);
    CHECK_FALSE(under.with_runtime_gte.has_value());
    CHECK(under.with_runtime_lte.value() == 29);
    const auto mid = band(DS::MbRuntime::Range90To120);
    CHECK(mid.with_runtime_gte.value() == 30);
    CHECK(mid.with_runtime_lte.value() == 45);
    const auto hour = band(DS::MbRuntime::Range2To3Hr);
    CHECK(hour.with_runtime_gte.value() == 46);
    CHECK(hour.with_runtime_lte.value() == 60);
    const auto over = band(DS::MbRuntime::Over3Hr);
    CHECK(over.with_runtime_gte.value() == 61);
    CHECK_FALSE(over.with_runtime_lte.has_value());
    const auto any = band(DS::MbRuntime::Any);
    CHECK_FALSE(any.with_runtime_gte.has_value());
    CHECK_FALSE(any.with_runtime_lte.has_value());
}

TEST_CASE("build_tv_discover_filter: TV vote-count gates are lower than movies",
          "[filter_state]") {
    const FilterState fs;
    CHECK(build_tv_discover_filter(fs, FilterTabKind::Popular).vote_count_gte.value()
          == media_browser::ui::kTvVoteCountPopular);
    CHECK(build_tv_discover_filter(fs, FilterTabKind::TopRated).vote_count_gte.value()
          == media_browser::ui::kTvVoteCountTopRated);
    CHECK(media_browser::ui::kTvVoteCountPopular < 200);
    CHECK(media_browser::ui::kTvVoteCountTopRated < 300);
}

TEST_CASE("build_tv_discover_filter: the genre mask uses TV ids",
          "[filter_state]") {
    FilterState fs;
    fs.genre_mask = 1u;                       // bit 0 of the TV catalog
    const auto tf = build_tv_discover_filter(fs, FilterTabKind::Popular);
    REQUIRE(tf.genre_ids.size() == 1);
    CHECK(tf.genre_ids[0] == 10759);          // Action & Adventure, not 28
}

// --- apply_mode_toggle -----------------------------------------------------
// The single most intricate rule in the mode work: "the staged edits belong to
// the mode being LEFT; the incoming mode's stored state is what gets
// re-staged." It touches only DisplaySettings, so it is asserted here rather
// than trusted inside a Renderer-bound screen.

TEST_CASE("apply_mode_toggle: staged edits land in the outgoing slot only",
          "[filter_state]") {
    DS s;
    // TV already has Drama-ish state persisted; Movies has none.
    write_filter_state(s, MbMode::Tv, FilterTabKind::Popular, fs_with(4u, 2, 0, 0, 0, 0));
    // The user was editing MOVIES/Popular and pressed MODE.
    const FilterState staged = fs_with(1u, 5, 3, 2, 4, 1);
    const FilterState restaged = apply_mode_toggle(
        s, /*outgoing=*/MbMode::Movies, /*incoming=*/MbMode::Tv,
        FilterTabKind::Popular, staged);

    // 1. the staged edits went to Movies, the mode they were made against.
    CHECK(read_filter_state(s, MbMode::Movies, FilterTabKind::Popular) == staged);
    // 2. TV's own persisted state is untouched...
    CHECK(read_filter_state(s, MbMode::Tv, FilterTabKind::Popular).genre_mask == 4u);
    CHECK(read_filter_state(s, MbMode::Tv, FilterTabKind::Popular).decade == 2);
    // 3. ...and is exactly what came back for the overlay to re-stage.
    CHECK(restaged == read_filter_state(s, MbMode::Tv, FilterTabKind::Popular));
    // 4. the mode itself moved.
    CHECK(s.mb_mode == DS::MbMode::Tv);
    // 5. the OTHER tab was not touched in either mode.
    CHECK(read_filter_state(s, MbMode::Movies, FilterTabKind::TopRated) ==
          read_filter_state(DS{}, MbMode::Movies, FilterTabKind::TopRated));
}

TEST_CASE("apply_mode_toggle: a round trip restores each mode's own edits",
          "[filter_state]") {
    DS s;
    const FilterState movie_edits = fs_with(1u, 1, 1, 1, 1, 1);
    const FilterState tv_edits    = fs_with(8u, 3, 2, 3, 5, 2);

    // Movies -> TV, carrying the movie edits.
    const FilterState after_first = apply_mode_toggle(
        s, MbMode::Movies, MbMode::Tv, FilterTabKind::Popular, movie_edits);
    CHECK(s.mb_mode == DS::MbMode::Tv);
    CHECK(after_first == FilterState{});          // TV had nothing persisted yet

    // Edit in TV, then TV -> Movies.
    const FilterState after_second = apply_mode_toggle(
        s, MbMode::Tv, MbMode::Movies, FilterTabKind::Popular, tv_edits);
    CHECK(s.mb_mode == DS::MbMode::Movies);
    CHECK(after_second == movie_edits);           // the movie edits came back
    CHECK(read_filter_state(s, MbMode::Tv, FilterTabKind::Popular) == tv_edits);
}

TEST_CASE("apply_mode_toggle: ForYou writes nothing and re-stages a default",
          "[filter_state]") {
    DS s;
    write_filter_state(s, MbMode::Movies, FilterTabKind::Popular, fs_with(1u, 1, 1, 1, 1, 1));
    const FilterState restaged = apply_mode_toggle(
        s, MbMode::Movies, MbMode::Tv, FilterTabKind::ForYou, fs_with(9u, 5, 3, 4, 8, 2));
    CHECK(restaged == FilterState{});
    CHECK(s.mb_mode == DS::MbMode::Tv);
    // The chart tabs are untouched — For You has no slot to spill into.
    CHECK(read_filter_state(s, MbMode::Movies, FilterTabKind::Popular).genre_mask == 1u);
    CHECK(read_filter_state(s, MbMode::Tv, FilterTabKind::Popular).genre_mask == 0u);
    CHECK(read_filter_state(s, MbMode::Tv, FilterTabKind::TopRated).genre_mask == 0u);
}
