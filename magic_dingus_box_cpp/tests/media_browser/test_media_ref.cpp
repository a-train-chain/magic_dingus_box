#include <catch2/catch_test_macros.hpp>

#include <unordered_map>
#include <unordered_set>

#include "media_browser/media_ref.h"
#include "media_browser/tmdb_client.h"

using media_browser::MediaKind;
using media_browser::MediaRef;
using media_browser::media_ref_of;
using media_browser::TmdbSearchHit;

TEST_CASE("MediaRef: equality is kind-aware", "[media_ref]") {
    CHECK(MediaRef{MediaKind::Movie, 1396} == MediaRef{MediaKind::Movie, 1396});
    CHECK(MediaRef{MediaKind::Movie, 1396} != MediaRef{MediaKind::Tv, 1396});
    CHECK(MediaRef{MediaKind::Tv, 1396} != MediaRef{MediaKind::Tv, 1397});
}

TEST_CASE("MediaRef: the 1396 collision — movie and show coexist in one set",
          "[media_ref]") {
    // tmdb_id 1396 is Breaking Bad (TV) AND an unrelated movie. A bare
    // unordered_set<int> collapses them into one entry; this must not.
    std::unordered_set<MediaRef> refs;
    CHECK(refs.insert(MediaRef{MediaKind::Movie, 1396}).second);
    CHECK(refs.insert(MediaRef{MediaKind::Tv, 1396}).second);
    REQUIRE(refs.size() == 2);
    CHECK(refs.count(MediaRef{MediaKind::Movie, 1396}) == 1);
    CHECK(refs.count(MediaRef{MediaKind::Tv, 1396}) == 1);
    CHECK(refs.count(MediaRef{MediaKind::Tv, 1395}) == 0);
}

TEST_CASE("MediaRef: hash separates kinds", "[media_ref]") {
    const std::hash<MediaRef> h;
    CHECK(h(MediaRef{MediaKind::Movie, 1396}) != h(MediaRef{MediaKind::Tv, 1396}));
    CHECK(h(MediaRef{MediaKind::Movie, 1396}) == h(MediaRef{MediaKind::Movie, 1396}));
    // Injective over adjacent ids of the same kind — no accidental aliasing
    // between (Movie, n) and (Tv, n-1).
    CHECK(h(MediaRef{MediaKind::Tv, 1396}) != h(MediaRef{MediaKind::Movie, 1397}));
}

TEST_CASE("MediaRef: map lookups are kind-scoped", "[media_ref]") {
    std::unordered_map<MediaRef, int> by_ref;
    by_ref[MediaRef{MediaKind::Movie, 42}] = 1;
    by_ref[MediaRef{MediaKind::Tv, 42}]    = 2;
    REQUIRE(by_ref.size() == 2);
    CHECK(by_ref.at(MediaRef{MediaKind::Movie, 42}) == 1);
    CHECK(by_ref.at(MediaRef{MediaKind::Tv, 42}) == 2);
}

TEST_CASE("media_ref_of copies kind and id from the hit", "[media_ref]") {
    TmdbSearchHit movie;
    movie.tmdb_id = 603;
    CHECK(media_ref_of(movie) == MediaRef{MediaKind::Movie, 603});  // default kind
    TmdbSearchHit show;
    show.tmdb_id = 1396;
    show.kind = MediaKind::Tv;
    CHECK(media_ref_of(show) == MediaRef{MediaKind::Tv, 1396});
}
