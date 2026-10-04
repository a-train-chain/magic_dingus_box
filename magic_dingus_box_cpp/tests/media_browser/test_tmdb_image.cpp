// tmdb_image.h — the TMDB size-segment rewrite behind grid-card posters.
#include <catch2/catch_test_macros.hpp>

#include "media_browser/artwork/artwork_cache.h"
#include "media_browser/tmdb_image.h"

using media_browser::is_tmdb_image_url;
using media_browser::tmdb_image_url_with_size;
using media_browser::tmdb_poster_url_for_card;

TEST_CASE("is_tmdb_image_url accepts only image.tmdb.org /t/p/ URLs",
          "[tmdb_image]") {
    CHECK(is_tmdb_image_url("https://image.tmdb.org/t/p/w500/abc.jpg"));
    CHECK(is_tmdb_image_url("http://image.tmdb.org/t/p/original/abc.jpg"));
    // Other hosts, even with the same path shape, are not ours to rewrite.
    CHECK_FALSE(is_tmdb_image_url("https://artworks.thetvdb.com/t/p/w500/a.jpg"));
    CHECK_FALSE(is_tmdb_image_url("https://evil.example/https://image.tmdb.org/t/p/w500/a.jpg"));
    CHECK_FALSE(is_tmdb_image_url("https://image.tmdb.org.evil.example/t/p/w500/a.jpg"));
    CHECK_FALSE(is_tmdb_image_url("https://assets.fanart.tv/fanart/movies/1/a.jpg"));
    // Malformed: no size segment / no file.
    CHECK_FALSE(is_tmdb_image_url("https://image.tmdb.org/t/p/"));
    CHECK_FALSE(is_tmdb_image_url("https://image.tmdb.org/t/p//a.jpg"));
    CHECK_FALSE(is_tmdb_image_url("https://image.tmdb.org/t/p/w500"));
    CHECK_FALSE(is_tmdb_image_url("https://image.tmdb.org/t/p/w500/"));
    CHECK_FALSE(is_tmdb_image_url(""));
}

TEST_CASE("tmdb_image_url_with_size rewrites only the size segment",
          "[tmdb_image]") {
    CHECK(tmdb_image_url_with_size("https://image.tmdb.org/t/p/w500/abc.jpg", "w185")
          == "https://image.tmdb.org/t/p/w185/abc.jpg");
    CHECK(tmdb_image_url_with_size("https://image.tmdb.org/t/p/original/abc.jpg", "w500")
          == "https://image.tmdb.org/t/p/w500/abc.jpg");
    CHECK(tmdb_image_url_with_size("http://image.tmdb.org/t/p/w780/x/y.png", "w185")
          == "http://image.tmdb.org/t/p/w185/x/y.png");
    // Idempotent.
    CHECK(tmdb_image_url_with_size("https://image.tmdb.org/t/p/w185/abc.jpg", "w185")
          == "https://image.tmdb.org/t/p/w185/abc.jpg");
    // Non-TMDB and malformed inputs pass through untouched.
    CHECK(tmdb_image_url_with_size("https://artworks.thetvdb.com/t/p/w500/a.jpg", "w185")
          == "https://artworks.thetvdb.com/t/p/w500/a.jpg");
    CHECK(tmdb_image_url_with_size("https://image.tmdb.org/t/p/w500", "w185")
          == "https://image.tmdb.org/t/p/w500");
    CHECK(tmdb_image_url_with_size("", "w185").empty());
}

TEST_CASE("tmdb_poster_url_for_card: grid cards get w185, heroes keep w500",
          "[tmdb_image]") {
    const std::string w500 = "https://image.tmdb.org/t/p/w500/abc.jpg";
    const std::string w185 = "https://image.tmdb.org/t/p/w185/abc.jpg";
    // The 9-column grids: (1280 - 2*60 - 8*8) / 9 = 121 logical px.
    CHECK(tmdb_poster_url_for_card(w500, (1280 - 2 * 60 - 8 * 8) / 9) == w185);
    // Queue-row thumbnail.
    CHECK(tmdb_poster_url_for_card(w500, 70) == w185);
    CHECK(tmdb_poster_url_for_card(w500, media_browser::kTmdbGridPosterMaxLogicalW) == w185);
    // Hero cards: SeriesDetail 160, overlay 200, Detail 280.
    CHECK(tmdb_poster_url_for_card(w500, 160) == w500);
    CHECK(tmdb_poster_url_for_card(w500, 280) == w500);
    // Non-TMDB art is untouched at any width.
    CHECK(tmdb_poster_url_for_card("https://artworks.thetvdb.com/p.jpg", 121)
          == "https://artworks.thetvdb.com/p.jpg");
}

TEST_CASE("grid size covers the largest grid card at 1080p output",
          "[tmdb_image]") {
    // MB lays out on a 1280x720 logical canvas; 1920x1080 is a 1.5x scale.
    // The w185 variant (185 px wide) must not be upscaled on the widest
    // card that still selects it.
    CHECK(media_browser::kTmdbGridPosterMaxLogicalW * 3 / 2 <= 185);
    // And w185 is ~7x cheaper in texture memory than w500.
    const auto w185 = media_browser::ArtworkCache::texture_bytes(185, 278);
    const auto w500 = media_browser::ArtworkCache::texture_bytes(500, 750);
    CHECK(w500 / w185 >= 7);
}
