// Unit tests for the display resolution policy — which DRM mode each
// display mode requests, what logical UI canvas it renders into, and how
// logical-space pixel constants map to framebuffer pixels.
//
// This policy is the thing that lets MODERN_TV run a 1920x1080 framebuffer
// while every existing layout constant keeps rendering at IDENTICAL
// proportions: the UI is drawn into a 1280x720 logical canvas that the
// shader stretches across the full framebuffer (exactly 1.5x, same 16:9).
// CRT_NATIVE must be completely unaffected.

#include <catch2/catch_test_macros.hpp>

#include "utils/config.h"

using namespace config::display;

// ---------------------------------------------------------------
// Physical DRM mode
// ---------------------------------------------------------------

TEST_CASE("CRT native requests 720p — never 1080p") {
    // The Pi 5 has no composite out, so CRT rigs run through an
    // HDMI->composite converter. CRT_MAX_HEIGHT=720 exists to keep that
    // converter on a mode it is known to accept; 1080p must never reach
    // the CRT path.
    Size m = target_drm_mode(/*crt_native=*/true);
    REQUIRE(m.w == PREFERRED_WIDTH);
    REQUIRE(m.h == PREFERRED_HEIGHT);
    REQUIRE(m.h <= CRT_MAX_HEIGHT);
}

TEST_CASE("Modern TV requests 1080p") {
    Size m = target_drm_mode(/*crt_native=*/false);
    REQUIRE(m.w == 1920);
    REQUIRE(m.h == 1080);
}

// ---------------------------------------------------------------
// Logical UI canvas — the mechanism that preserves the look
// ---------------------------------------------------------------

TEST_CASE("Modern TV keeps the 1280x720 logical canvas") {
    // Every existing layout constant (theme.cpp fonts/margins, the Media
    // Browser's literal 1280s) is written for this space. Holding it
    // fixed is what makes 1080p a pure resolution change.
    Size c = logical_canvas(/*crt_native=*/false);
    REQUIRE(c.w == 1280);
    REQUIRE(c.h == 720);
}

TEST_CASE("CRT native keeps its existing 640x480 logical canvas") {
    Size c = logical_canvas(/*crt_native=*/true);
    REQUIRE(c.w == 640);
    REQUIRE(c.h == 480);
}

TEST_CASE("Modern TV logical canvas divides the physical mode exactly 1.5x") {
    // The whole approach depends on a clean uniform scale with no
    // rounding drift and no aspect change. 1920/1280 == 1080/720 == 1.5.
    Size m = target_drm_mode(false);
    Size c = logical_canvas(false);
    REQUIRE(m.w * c.h == m.h * c.w);          // identical aspect ratio
    REQUIRE(m.w * 2 == c.w * 3);              // exactly 1.5x horizontally
    REQUIRE(m.h * 2 == c.h * 3);              // exactly 1.5x vertically
}

TEST_CASE("the 4:3 pillarbox stays integer-exact at both resolutions") {
    // Playlist videos are 640x480 (4:3) and must present as 4:3 in
    // Modern TV mode. 720p -> (160,0,960,720); 1080p -> (240,0,1440,1080).
    // Both exact, no truncation.
    for (int h : {720, 1080}) {
        int w = h * 4 / 3;
        REQUIRE(w * 3 == h * 4);              // exactly 4:3, no rounding
    }
    REQUIRE(720 * 4 / 3 == 960);
    REQUIRE(1080 * 4 / 3 == 1440);
}

// ---------------------------------------------------------------
// Logical -> physical scaling for pixel constants
// ---------------------------------------------------------------

TEST_CASE("the playback wood-frame inset scales with the framebuffer") {
    // The marquee frame is drawn as a full-screen quad in LOGICAL space
    // (so the 1280x720 art stretches to fill), but set_render_inset()
    // takes FRAMEBUFFER pixels. Leaving the inset at a literal 40 while
    // the frame's inner edge moves to 60 would let the cabinet art crop
    // ~20px off every side of the movie.
    REQUIRE(to_physical_px(40, /*fb_height=*/720,  /*logical_height=*/720) == 40);
    REQUIRE(to_physical_px(40, /*fb_height=*/1080, /*logical_height=*/720) == 60);
}

TEST_CASE("to_physical_px is identity when framebuffer equals logical") {
    REQUIRE(to_physical_px(17, 720, 720) == 17);
    REQUIRE(to_physical_px(0, 1080, 720) == 0);
}

TEST_CASE("to_physical_px never divides by zero") {
    REQUIRE(to_physical_px(40, 1080, 0) == 40);
}
