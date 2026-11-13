// Playlist video is drawn into "the 4:3 area" (a centred 4:3 rect in
// Modern TV mode, the whole non-square framebuffer in CRT Native mode).
// place_picture decides where the picture goes and which part of the frame
// is sampled: crop covers the 4:3 area, fit shows the whole frame inside
// the visible opening (the bezel's window when a bezel is drawn). All the
// math is in DISPLAY aspect, so a 1280x720 framebuffer shown as 4:3 still
// gets an undistorted picture.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "video/picture_placement.h"

using Catch::Approx;

namespace {
const video::IRect T{0, 0, 960, 720};
const video::IRect T1080{240, 0, 1440, 1080};
const video::IRect MDB{284, 48, 1348, 984};  // MDB bezel opening, 1080p
const video::IRect CRT{0, 0, 1280, 720};
}  // namespace

TEST_CASE("crop fills the 4:3 target and samples the centre of the frame",
          "[video][picture_placement]") {
    auto c = video::place_picture(16.0/9, T, 4.0/3, T, true);
    CHECK(c.viewport == T);
    CHECK(c.tex.u0 == Approx(0.125)); CHECK(c.tex.u1 == Approx(0.875));
    CHECK(c.tex.v0 == Approx(0.0));   CHECK(c.tex.v1 == Approx(1.0));
}

TEST_CASE("fit shows the whole frame with bars", "[video][picture_placement]") {
    CHECK(video::place_picture(16.0/9, T, 4.0/3, T, false).viewport == video::IRect{0, 90, 960, 540});
}

TEST_CASE("fit sits inside the bezel opening; crop ignores the opening",
          "[video][picture_placement]") {
    CHECK(video::place_picture(16.0/9, T1080, 4.0/3, MDB, false).viewport == video::IRect{284, 161, 1348, 758});
    // The bezel covers the edges of a cropped picture, as today.
    CHECK(video::place_picture(16.0/9, T1080, 4.0/3, MDB, true).viewport == T1080);
}

TEST_CASE("CRT Native: a non-square framebuffer shown as 4:3",
          "[video][picture_placement]") {
    CHECK(video::place_picture(16.0/9, CRT, 4.0/3, CRT, false).viewport == video::IRect{0, 90, 1280, 540});
    CHECK(video::place_picture(16.0/9, CRT, 4.0/3, CRT, true).tex.u0 == Approx(0.125));
}

TEST_CASE("portrait source crops top and bottom, fits with side bars",
          "[video][picture_placement]") {
    auto p = video::place_picture(9.0/16, T, 4.0/3, T, true);
    CHECK(p.tex.v0 == Approx(0.2890625)); CHECK(p.tex.v1 == Approx(0.7109375)); CHECK(p.viewport == T);
    CHECK(video::place_picture(9.0/16, T, 4.0/3, T, false).viewport == video::IRect{277, 0, 405, 720});
}

TEST_CASE("4:3 source: full target, full frame", "[video][picture_placement]") {
    auto q = video::place_picture(4.0/3, T, 4.0/3, T, true);
    CHECK(q.viewport == T); CHECK(q.tex.u0 == Approx(0.0)); CHECK(q.tex.u1 == Approx(1.0));
}

TEST_CASE("unknown aspect draws the whole frame over the whole target",
          "[video][picture_placement]") {
    // No usable aspect yet (caps not negotiated): today's behaviour, not a
    // NaN cast to int.
    for (bool crop : {true, false}) {
        auto z = video::place_picture(0.0, T, 4.0/3, T, crop);
        CHECK(z.viewport == T);
        CHECK(z.tex.u0 == Approx(0.0)); CHECK(z.tex.u1 == Approx(1.0));
        CHECK(z.tex.v0 == Approx(0.0)); CHECK(z.tex.v1 == Approx(1.0));
    }
    // An empty opening fits inside the target rather than drawing nothing.
    CHECK(video::place_picture(16.0/9, T, 4.0/3, video::IRect{0, 0, 0, 0}, false).viewport == video::IRect{0, 90, 960, 540});
}

TEST_CASE("placement helpers", "[video][picture_placement]") {
    CHECK(video::is_four_by_three(640.0/480)); CHECK(video::is_four_by_three(720.0*8/(480*9))); // 720x480, PAR 8:9
    CHECK_FALSE(video::is_four_by_three(1.5)); CHECK_FALSE(video::is_four_by_three(16.0/9));
    CHECK(video::four_by_three_rect(1280, 720) == video::IRect{160, 0, 960, 720});
    CHECK(video::four_by_three_rect(1920, 1080) == video::IRect{240, 0, 1440, 1080});
    CHECK(video::four_by_three_rect(640, 480) == video::IRect{0, 0, 640, 480});
    CHECK(video::four_by_three_rect(1000, 1000) == video::IRect{0, 125, 1000, 750});  // letterbox
    CHECK(video::scale_image_rect(MDB, 1920, 1080, 1280, 720) == video::IRect{189, 32, 899, 656});
}
