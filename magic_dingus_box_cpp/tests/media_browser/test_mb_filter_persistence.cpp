#include <catch2/catch_test_macros.hpp>

#include <json/json.h>

#include "app/app_state.h"
#include "app/mb_filter_persistence.h"

using DS = app::AppState::DisplaySettings;

TEST_CASE("DisplaySettings: mb_filter() gives four independent slots",
          "[mb_filters]") {
    DS s;
    s.mb_filter(DS::MbMode::Movies, DS::MbChartTab::Popular).genre_mask  = 1u;
    s.mb_filter(DS::MbMode::Movies, DS::MbChartTab::TopRated).genre_mask = 2u;
    s.mb_filter(DS::MbMode::Tv,     DS::MbChartTab::Popular).genre_mask  = 4u;
    s.mb_filter(DS::MbMode::Tv,     DS::MbChartTab::TopRated).genre_mask = 8u;
    CHECK(s.mb_filter(DS::MbMode::Movies, DS::MbChartTab::Popular).genre_mask  == 1u);
    CHECK(s.mb_filter(DS::MbMode::Movies, DS::MbChartTab::TopRated).genre_mask == 2u);
    CHECK(s.mb_filter(DS::MbMode::Tv,     DS::MbChartTab::Popular).genre_mask  == 4u);
    CHECK(s.mb_filter(DS::MbMode::Tv,     DS::MbChartTab::TopRated).genre_mask == 8u);
}

TEST_CASE("DisplaySettings: per-tab sort defaults survive the restructure",
          "[mb_filters]") {
    const DS s;
    CHECK(s.mb_mode == DS::MbMode::Movies);
    CHECK(s.mb_filter(DS::MbMode::Movies, DS::MbChartTab::Popular).sort
          == DS::MbDiscoverSort::Popularity);
    CHECK(s.mb_filter(DS::MbMode::Movies, DS::MbChartTab::TopRated).sort
          == DS::MbDiscoverSort::TopRated);
    CHECK(s.mb_filter(DS::MbMode::Tv, DS::MbChartTab::Popular).sort
          == DS::MbDiscoverSort::Popularity);
    CHECK(s.mb_filter(DS::MbMode::Tv, DS::MbChartTab::TopRated).sort
          == DS::MbDiscoverSort::TopRated);
    CHECK(s.mb_filter(DS::MbMode::Movies, DS::MbChartTab::Popular).decade
          == DS::MbDecade::Any);
    CHECK(s.mb_filter(DS::MbMode::Tv, DS::MbChartTab::TopRated).language
          == DS::MbLanguage::Any);
}

TEST_CASE("persistence: a legacy movie-only settings.json loads into the "
          "Movies slots", "[mb_filters]") {
    // Exactly the keys a box provisioned before Phase 2c-1 has on disk.
    Json::Value display;
    display["mb_popular_genre_mask"]  = 4u;
    display["mb_popular_decade"]      = "1990s";
    display["mb_popular_min_rating"]  = "7";
    display["mb_popular_runtime"]     = "90to120";
    display["mb_popular_language"]    = "ja";
    display["mb_popular_sort"]        = "most_voted";
    display["mb_toprated_genre_mask"] = 8u;
    display["mb_toprated_decade"]     = "classic";
    display["mb_toprated_sort"]       = "recent_release";

    DS s;
    app::mb_filters_from_json(display, s);

    const auto& mp = s.mb_filter(DS::MbMode::Movies, DS::MbChartTab::Popular);
    CHECK(mp.genre_mask == 4u);
    CHECK(mp.decade     == DS::MbDecade::D1990s);
    CHECK(mp.min_rating == DS::MbMinRating::Seven);
    CHECK(mp.runtime    == DS::MbRuntime::Range90To120);
    CHECK(mp.language   == DS::MbLanguage::Japanese);
    CHECK(mp.sort       == DS::MbDiscoverSort::MostVoted);

    const auto& mt = s.mb_filter(DS::MbMode::Movies, DS::MbChartTab::TopRated);
    CHECK(mt.genre_mask == 8u);
    CHECK(mt.decade     == DS::MbDecade::Classic);
    CHECK(mt.sort       == DS::MbDiscoverSort::RecentRelease);
    CHECK(mt.min_rating == DS::MbMinRating::Any);   // key absent → default kept

    // Nothing leaked into TV, and mode stayed Movies.
    CHECK(s.mb_mode == DS::MbMode::Movies);
    CHECK(s.mb_filter(DS::MbMode::Tv, DS::MbChartTab::Popular).genre_mask == 0u);
    CHECK(s.mb_filter(DS::MbMode::Tv, DS::MbChartTab::TopRated).genre_mask == 0u);
    CHECK(s.mb_filter(DS::MbMode::Tv, DS::MbChartTab::TopRated).sort
          == DS::MbDiscoverSort::TopRated);
}

TEST_CASE("persistence: round-trip preserves all four sets and the mode",
          "[mb_filters]") {
    DS in;
    in.mb_mode = DS::MbMode::Tv;
    in.mb_filter(DS::MbMode::Movies, DS::MbChartTab::Popular)  =
        {1u, DS::MbDecade::D2020s, DS::MbMinRating::Six,   DS::MbRuntime::Under90,
         DS::MbLanguage::French,  DS::MbDiscoverSort::MostVoted};
    in.mb_filter(DS::MbMode::Movies, DS::MbChartTab::TopRated) =
        {2u, DS::MbDecade::D1970s, DS::MbMinRating::Eight, DS::MbRuntime::Over3Hr,
         DS::MbLanguage::German,  DS::MbDiscoverSort::RecentRelease};
    in.mb_filter(DS::MbMode::Tv, DS::MbChartTab::Popular)      =
        {4u, DS::MbDecade::D2010s, DS::MbMinRating::Seven, DS::MbRuntime::Range2To3Hr,
         DS::MbLanguage::Korean,  DS::MbDiscoverSort::Popularity};
    in.mb_filter(DS::MbMode::Tv, DS::MbChartTab::TopRated)     =
        {8u, DS::MbDecade::Classic, DS::MbMinRating::Any,  DS::MbRuntime::Range90To120,
         DS::MbLanguage::Mandarin, DS::MbDiscoverSort::TopRated};

    Json::Value display;
    app::mb_filters_to_json(in, display, /*include_tv=*/true);
    DS out;
    app::mb_filters_from_json(display, out);

    CHECK(out.mb_mode == DS::MbMode::Tv);
    for (auto mode : {DS::MbMode::Movies, DS::MbMode::Tv}) {
        for (auto tab : {DS::MbChartTab::Popular, DS::MbChartTab::TopRated}) {
            const auto& a = in.mb_filter(mode, tab);
            const auto& b = out.mb_filter(mode, tab);
            CHECK(a.genre_mask == b.genre_mask);
            CHECK(a.decade     == b.decade);
            CHECK(a.min_rating == b.min_rating);
            CHECK(a.runtime    == b.runtime);
            CHECK(a.language   == b.language);
            CHECK(a.sort       == b.sort);
        }
    }
}

TEST_CASE("persistence: absent mb_mode defaults to Movies", "[mb_filters]") {
    DS s;
    s.mb_mode = DS::MbMode::Tv;
    Json::Value display;              // no mb_mode key at all
    app::mb_filters_from_json(display, s);
    CHECK(s.mb_mode == DS::MbMode::Movies);
}

TEST_CASE("persistence: include_tv=false writes exactly the legacy key set",
          "[mb_filters]") {
    // The ENABLE_MEDIA_BROWSER=OFF build must produce byte-identical
    // settings.json output — no mb_mode, no mb_tv_* keys.
    DS s;
    s.mb_mode = DS::MbMode::Tv;
    Json::Value display;
    app::mb_filters_to_json(s, display, /*include_tv=*/false);
    CHECK(display.isMember("mb_popular_genre_mask"));
    CHECK(display.isMember("mb_toprated_sort"));
    CHECK_FALSE(display.isMember("mb_mode"));
    CHECK_FALSE(display.isMember("mb_tv_popular_genre_mask"));
    CHECK_FALSE(display.isMember("mb_tv_toprated_sort"));
    CHECK(display.getMemberNames().size() == 12);
}
