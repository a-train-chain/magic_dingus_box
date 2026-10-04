// artwork_sizing.h — the card-slot downscale policy that keeps TVDB (and
// any other non-TMDB) posters from blowing the Pi 4B's texture budget.
#include <catch2/catch_test_macros.hpp>

#include "media_browser/artwork/artwork_cache.h"
#include "media_browser/artwork/artwork_sizing.h"

#include <cstdint>
#include <vector>

using namespace media_browser;

TEST_CASE("artwork_variant_for_slot: grid-sized slots are cards, heroes are full",
          "[artwork_sizing]") {
    CHECK(artwork_variant_for_slot(121.0f) == ArtworkVariant::Card);  // 9-col grid
    CHECK(artwork_variant_for_slot(70.0f) == ArtworkVariant::Card);   // queue thumb
    CHECK(artwork_variant_for_slot(
              static_cast<float>(kTmdbGridPosterMaxLogicalW)) == ArtworkVariant::Card);
    CHECK(artwork_variant_for_slot(160.0f) == ArtworkVariant::Full);  // SeriesDetail
    CHECK(artwork_variant_for_slot(200.0f) == ArtworkVariant::Full);  // overlay
    CHECK(artwork_variant_for_slot(280.0f) == ArtworkVariant::Full);  // Detail
}

TEST_CASE("artwork_cache_key keeps Full as the bare URL and separates Card",
          "[artwork_sizing]") {
    const std::string u = "https://artworks.thetvdb.com/banners/posters/1.jpg";
    CHECK(artwork_cache_key(u, ArtworkVariant::Full) == u);
    CHECK(artwork_cache_key(u, ArtworkVariant::Card) != u);
    CHECK(artwork_cache_key(u, ArtworkVariant::Card) !=
          artwork_cache_key(u, ArtworkVariant::Full));
    // Distinct URLs stay distinct as cards.
    CHECK(artwork_cache_key(u, ArtworkVariant::Card) !=
          artwork_cache_key(u + "x", ArtworkVariant::Card));
}

TEST_CASE("card box is at most 2x the widest grid card at 1080p",
          "[artwork_sizing]") {
    // 123 logical * 1.5 (1080p scale) = 184.5 -> 185 px; 2x = 370.
    CHECK(kCardMaxPixelW <= 2 * 185);
    CHECK(kCardMaxPixelW >= 185);  // never below 1:1 at 1080p
    CHECK(kCardMaxPixelH * 2 >= kCardMaxPixelW * 3);  // a 2:3 poster fits at full width
}

TEST_CASE("target_size_for_variant: TVDB poster shrinks for cards only",
          "[artwork_sizing]") {
    const PixelSize card = target_size_for_variant(680, 1000, ArtworkVariant::Card);
    CHECK(card.w == 370);
    CHECK(card.h == 544);
    const PixelSize full = target_size_for_variant(680, 1000, ArtworkVariant::Full);
    CHECK(full.w == 680);
    CHECK(full.h == 1000);

    // The memory win the fix is for: an 18-card page of TVDB posters fits
    // the Pi 4B's 64 MB budget with room to spare.
    const std::size_t per_card = ArtworkCache::texture_bytes(card.w, card.h);
    CHECK(per_card * 18 < 32u * 1024u * 1024u);
    // ...where the full-size variant (~3.6 MB each) ate the whole budget.
    CHECK(per_card * 3 < ArtworkCache::texture_bytes(680, 1000));
}

TEST_CASE("target_size_for_variant never upscales and leaves w185 alone",
          "[artwork_sizing]") {
    const PixelSize w185 = target_size_for_variant(185, 278, ArtworkVariant::Card);
    CHECK(w185.w == 185);
    CHECK(w185.h == 278);
    const PixelSize tiny = target_size_for_variant(10, 15, ArtworkVariant::Card);
    CHECK(tiny.w == 10);
    CHECK(tiny.h == 15);
    const PixelSize bad = target_size_for_variant(0, 100, ArtworkVariant::Card);
    CHECK(bad.w == 0);
    CHECK(bad.h == 0);
}

TEST_CASE("fit_within keeps aspect and respects both caps", "[artwork_sizing]") {
    // Landscape art in a card box is width-limited.
    const PixelSize land = fit_within(1280, 720, kCardMaxPixelW, kCardMaxPixelH);
    CHECK(land.w == 370);
    CHECK(land.h == 208);
    // A very tall image is height-limited.
    const PixelSize tall = fit_within(400, 2000, kCardMaxPixelW, kCardMaxPixelH);
    CHECK(tall.h == 555);
    CHECK(tall.w == 111);
    CHECK(tall.w <= kCardMaxPixelW);
    // Extreme aspect never collapses an axis to 0.
    const PixelSize sliver = fit_within(10000, 1, 370, 555);
    CHECK(sliver.w == 370);
    CHECK(sliver.h == 1);
    // No limit on either axis = unchanged.
    const PixelSize nolimit = fit_within(999, 777, 0, 0);
    CHECK(nolimit.w == 999);
    CHECK(nolimit.h == 777);
}

TEST_CASE("downscale_rgba_area averages coverage and keeps solid colors exact",
          "[artwork_sizing]") {
    // Solid colour stays exactly that colour at any ratio.
    std::vector<std::uint8_t> solid(7 * 5 * 4);
    for (std::size_t i = 0; i < solid.size(); i += 4) {
        solid[i] = 10; solid[i + 1] = 200; solid[i + 2] = 99; solid[i + 3] = 255;
    }
    const auto s = downscale_rgba_area(solid.data(), 7, 5, 3, 2);
    REQUIRE(s.size() == 3u * 2u * 4u);
    for (std::size_t i = 0; i < s.size(); i += 4) {
        CHECK(s[i] == 10);
        CHECK(s[i + 1] == 200);
        CHECK(s[i + 2] == 99);
        CHECK(s[i + 3] == 255);
    }

    // 2x2 -> 1x1 is the mean of the four pixels.
    const std::vector<std::uint8_t> quad = {
        0, 0, 0, 255,     100, 0, 0, 255,
        0, 200, 0, 255,   0, 0, 40, 255,
    };
    const auto m = downscale_rgba_area(quad.data(), 2, 2, 1, 1);
    REQUIRE(m.size() == 4u);
    CHECK(m[0] == 25);
    CHECK(m[1] == 50);
    CHECK(m[2] == 10);
    CHECK(m[3] == 255);

    // Non-integer ratio: 3 -> 2 columns. Column 0 covers src 0 fully and
    // half of src 1; column 1 covers the other half of 1 and all of 2.
    const std::vector<std::uint8_t> row = {
        0, 0, 0, 0,   90, 0, 0, 0,   180, 0, 0, 0,
    };
    const auto r = downscale_rgba_area(row.data(), 3, 1, 2, 1);
    REQUIRE(r.size() == 8u);
    CHECK(r[0] == 30);   // (0*1 + 90*0.5) / 1.5
    CHECK(r[4] == 150);  // (90*0.5 + 180*1) / 1.5
}

TEST_CASE("downscale_rgba_area: identity copy, clamping, invalid input",
          "[artwork_sizing]") {
    const std::vector<std::uint8_t> px = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(downscale_rgba_area(px.data(), 2, 1, 2, 1) == px);
    // Asking to upscale clamps to the source size.
    CHECK(downscale_rgba_area(px.data(), 2, 1, 9, 9) == px);
    CHECK(downscale_rgba_area(nullptr, 2, 1, 1, 1).empty());
    CHECK(downscale_rgba_area(px.data(), 0, 1, 1, 1).empty());
    CHECK(downscale_rgba_area(px.data(), 2, 1, 0, 1).empty());
}

TEST_CASE("artwork_drawn_recently: current and previous drawn frame only",
          "[artwork_sizing]") {
    CHECK(artwork_drawn_recently(5, 5));
    CHECK(artwork_drawn_recently(4, 5));
    CHECK_FALSE(artwork_drawn_recently(3, 5));
    // Never drawn / tracking unarmed protect nothing.
    CHECK_FALSE(artwork_drawn_recently(0, 5));
    CHECK_FALSE(artwork_drawn_recently(0, 0));
    CHECK_FALSE(artwork_drawn_recently(3, 0));
}
