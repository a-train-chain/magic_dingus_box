#include <catch2/catch_test_macros.hpp>
#include "media_browser/ui/mb_recs.h"

using media_browser::TmdbSearchHit;
using media_browser::ui::merge_recommendations;

namespace {
TmdbSearchHit mk(int id) {
    TmdbSearchHit h;
    h.tmdb_id = id;
    h.title = "Movie " + std::to_string(id);
    return h;
}
}  // namespace

TEST_CASE("merge: seed-count score dominates", "[mb_recs]") {
    // id 7 recommended by 2 seeds; id 9 by 1 seed at index 0.
    std::vector<std::vector<TmdbSearchHit>> per_seed = {
        {mk(9), mk(7)},
        {mk(7)},
    };
    auto out = merge_recommendations(per_seed, {});
    REQUIRE(out.size() == 2);
    CHECK(out[0].tmdb_id == 7);
    CHECK(out[1].tmdb_id == 9);
}

TEST_CASE("merge: min-index breaks equal seed counts", "[mb_recs]") {
    // Both ids appear in exactly one seed list; 21 at index 0 beats 20 at index 2.
    std::vector<std::vector<TmdbSearchHit>> per_seed = {
        {mk(5), mk(6), mk(20)},
        {mk(21)},
    };
    auto out = merge_recommendations(per_seed, {});
    REQUIRE(out.size() == 4);
    CHECK(out[0].tmdb_id == 5);    // count 1, min-index 0, id 5
    CHECK(out[1].tmdb_id == 21);   // count 1, min-index 0, id 21
    CHECK(out[2].tmdb_id == 6);    // count 1, min-index 1
    CHECK(out[3].tmdb_id == 20);   // count 1, min-index 2
}

TEST_CASE("merge: ascending tmdb_id breaks full ties", "[mb_recs]") {
    std::vector<std::vector<TmdbSearchHit>> per_seed = {{mk(300), mk(100)}, {mk(100), mk(300)}};
    auto out = merge_recommendations(per_seed, {});
    REQUIRE(out.size() == 2);
    // Both: count 2, min-index 0 → lower id first.
    CHECK(out[0].tmdb_id == 100);
    CHECK(out[1].tmdb_id == 300);
}

TEST_CASE("merge: excludes library ids and non-positive ids", "[mb_recs]") {
    std::vector<std::vector<TmdbSearchHit>> per_seed = {{mk(1), mk(2), mk(0), mk(-3)}};
    auto out = merge_recommendations(per_seed, {2});
    REQUIRE(out.size() == 1);
    CHECK(out[0].tmdb_id == 1);
}

TEST_CASE("merge: duplicate within one seed list counts once", "[mb_recs]") {
    std::vector<std::vector<TmdbSearchHit>> per_seed = {
        {mk(4), mk(4), mk(4)},   // one seed, repeated — count must stay 1
        {mk(8)}, {mk(8)},        // two distinct seeds — count 2
    };
    auto out = merge_recommendations(per_seed, {});
    REQUIRE(out.size() == 2);
    CHECK(out[0].tmdb_id == 8);
    CHECK(out[1].tmdb_id == 4);
}

TEST_CASE("merge: caps at exactly cap", "[mb_recs]") {
    std::vector<std::vector<TmdbSearchHit>> per_seed(1);
    for (int i = 1; i <= 150; ++i) per_seed[0].push_back(mk(i));
    auto out = merge_recommendations(per_seed, {});
    CHECK(out.size() == 100);          // default cap (spec: exactly 100)
    auto out3 = merge_recommendations(per_seed, {}, 3);
    REQUIRE(out3.size() == 3);
    CHECK(out3[0].tmdb_id == 1);       // count 1, min-index 0
}
