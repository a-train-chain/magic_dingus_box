#include <catch2/catch_test_macros.hpp>

#include <unordered_set>

#include "media_browser/media_ref.h"
#include "media_browser/ui/mb_recs.h"

using media_browser::MediaKind;
using media_browser::MediaRef;
using media_browser::TmdbSearchHit;
using media_browser::ui::merge_recommendations;

namespace {
TmdbSearchHit mk(int id) {
    TmdbSearchHit h;
    h.tmdb_id = id;
    h.title = "Movie " + std::to_string(id);
    return h;
}
TmdbSearchHit mk_tv(int id) {
    TmdbSearchHit h;
    h.tmdb_id = id;
    h.kind = MediaKind::Tv;
    h.title = "Show " + std::to_string(id);
    return h;
}
const std::unordered_set<MediaRef> kNoExclude;
}  // namespace

TEST_CASE("merge: seed-count score dominates", "[mb_recs]") {
    std::vector<std::vector<TmdbSearchHit>> per_seed = {
        {mk(9), mk(7)},
        {mk(7)},
    };
    auto out = merge_recommendations(per_seed, kNoExclude);
    REQUIRE(out.size() == 2);
    CHECK(out[0].tmdb_id == 7);
    CHECK(out[1].tmdb_id == 9);
}

TEST_CASE("merge: min-index breaks equal seed counts", "[mb_recs]") {
    std::vector<std::vector<TmdbSearchHit>> per_seed = {
        {mk(5), mk(6), mk(20)},
        {mk(21)},
    };
    auto out = merge_recommendations(per_seed, kNoExclude);
    REQUIRE(out.size() == 4);
    CHECK(out[0].tmdb_id == 5);
    CHECK(out[1].tmdb_id == 21);
    CHECK(out[2].tmdb_id == 6);
    CHECK(out[3].tmdb_id == 20);
}

TEST_CASE("merge: ascending tmdb_id breaks full ties", "[mb_recs]") {
    std::vector<std::vector<TmdbSearchHit>> per_seed = {{mk(300), mk(100)},
                                                        {mk(100), mk(300)}};
    auto out = merge_recommendations(per_seed, kNoExclude);
    REQUIRE(out.size() == 2);
    CHECK(out[0].tmdb_id == 100);
    CHECK(out[1].tmdb_id == 300);
}

TEST_CASE("merge: excludes library ids and non-positive ids", "[mb_recs]") {
    std::vector<std::vector<TmdbSearchHit>> per_seed = {{mk(1), mk(2), mk(0), mk(-3)}};
    auto out = merge_recommendations(per_seed, {MediaRef{MediaKind::Movie, 2}});
    REQUIRE(out.size() == 1);
    CHECK(out[0].tmdb_id == 1);
}

TEST_CASE("merge: duplicate within one seed list counts once", "[mb_recs]") {
    std::vector<std::vector<TmdbSearchHit>> per_seed = {
        {mk(4), mk(4), mk(4)},
        {mk(8)}, {mk(8)},
    };
    auto out = merge_recommendations(per_seed, kNoExclude);
    REQUIRE(out.size() == 2);
    CHECK(out[0].tmdb_id == 8);
    CHECK(out[1].tmdb_id == 4);
}

TEST_CASE("merge: caps at exactly cap", "[mb_recs]") {
    std::vector<std::vector<TmdbSearchHit>> per_seed(1);
    for (int i = 1; i <= 150; ++i) per_seed[0].push_back(mk(i));
    auto out = merge_recommendations(per_seed, kNoExclude);
    CHECK(out.size() == 100);
    auto out3 = merge_recommendations(per_seed, kNoExclude, 3);
    REQUIRE(out3.size() == 3);
    CHECK(out3[0].tmdb_id == 1);
}

TEST_CASE("merge: exclude is kind-aware", "[mb_recs]") {
    // The library holds SHOW 1396. The movie with the same id must survive.
    std::vector<std::vector<TmdbSearchHit>> per_seed = {{mk(1396), mk_tv(1396)}};
    auto out = merge_recommendations(per_seed, {MediaRef{MediaKind::Tv, 1396}});
    REQUIRE(out.size() == 1);
    CHECK(out[0].tmdb_id == 1396);
    CHECK(out[0].kind == MediaKind::Movie);
}

TEST_CASE("merge: same id in both kinds yields two entries", "[mb_recs]") {
    std::vector<std::vector<TmdbSearchHit>> per_seed = {{mk(1396)}, {mk_tv(1396)}};
    auto out = merge_recommendations(per_seed, kNoExclude);
    REQUIRE(out.size() == 2);
}

TEST_CASE("merge: kind breaks the final tie deterministically", "[mb_recs]") {
    // Identical seed count (1) and min index (0) and id — only kind differs.
    // Movie sorts before Tv, so the order is stable across runs.
    std::vector<std::vector<TmdbSearchHit>> per_seed = {{mk_tv(500)}, {mk(500)}};
    auto out = merge_recommendations(per_seed, kNoExclude);
    REQUIRE(out.size() == 2);
    CHECK(out[0].kind == MediaKind::Movie);
    CHECK(out[1].kind == MediaKind::Tv);
}
