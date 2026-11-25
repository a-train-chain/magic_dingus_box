// video::content_rects — the content rectangle every frame draws into, in
// framebuffer pixels (content) and in the UI's logical canvas
// (logical_content). It was computed inline in main.cpp's render loop; it
// moved here unchanged so the Tube engine and main.cpp cannot disagree.
// The table below pins every combination the kiosk can reach to the values
// main.cpp's inline code produced.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include <string>

#include "utils/config.h"
#include "video/content_area.h"

// A failed IRect check prints the rect, not "{?} == {?}".
namespace Catch {
template <>
struct StringMaker<video::IRect> {
    static std::string convert(const video::IRect& r) {
        return "{" + std::to_string(r.x) + ", " + std::to_string(r.y) + ", " +
               std::to_string(r.w) + ", " + std::to_string(r.h) + "}";
    }
};
}  // namespace Catch

TEST_CASE("content rects: the kiosk's three layouts", "[video]") {
    auto m = video::content_rects(true, false, false, 1920, 1080, 1280, 720);
    REQUIRE(m.content == video::IRect{240, 0, 1440, 1080});
    REQUIRE(m.logical_content == video::IRect{160, 0, 960, 720}); REQUIRE(m.letterbox_active);
    auto f = video::content_rects(true, true, false, 1920, 1080, 1280, 720);   // full-screen film
    REQUIRE(f.content == video::IRect{0, 0, 1920, 1080}); REQUIRE_FALSE(f.letterbox_active);
    REQUIRE(f.logical_content == video::IRect{0, 0, 1280, 720});
    REQUIRE(video::content_rects(true, false, true, 1920, 1080, 1280, 720).content == video::IRect{0, 0, 1920, 1080});
    auto c = video::content_rects(false, false, false, 1280, 720, 640, 480);   // CRT Native
    REQUIRE(c.content == video::IRect{0, 0, 1280, 720}); REQUIRE(c.logical_content == video::IRect{0, 0, 640, 480});
    REQUIRE(video::content_rects(true, false, false, 1280, 720, 1280, 720).content == video::IRect{160, 0, 960, 720});
}

TEST_CASE("content rects: every reachable combination matches main.cpp's inline computation",
          "[video]") {
    // Canvas sizes are config::display::logical_canvas(): Modern TV draws
    // into 1280x720, CRT Native into 640x480. Framebuffers: Modern TV asks
    // for 1920x1080 and CRT Native for 1280x720 (target_drm_mode), and
    // pick_mode may land on 1280x720 for either; 720x480 / 720x576 are the
    // SD fallbacks a converter can advertise.
    //
    // Expected values, from main.cpp before the move:
    //   letterbox_active = modern_tv && !full_screen_film && !media_browser
    //   content          = letterbox ? four_by_three_rect(fb)     : {0,0,fb}
    //   logical_content  = letterbox ? four_by_three_rect(canvas) : {0,0,canvas}
    // four_by_three_rect(w,h): cw = h*4/3, ch = h; if cw > w then cw = w,
    // ch = w*3/4; centred. Worked: 1920x1080 -> {240,0,1440,1080};
    // 1280x720 -> {160,0,960,720}; 720x480 -> {40,0,640,480};
    // 720x576 -> cw 768 > 720 -> {0,18,720,540}.
    struct Row {
        bool modern_tv, film, mb;
        int fb_w, fb_h;
        video::IRect content, logical;
        bool letterbox;
    };
    constexpr auto modern = config::display::logical_canvas(false);
    constexpr auto crt = config::display::logical_canvas(true);
    STATIC_REQUIRE(modern.w == 1280);
    STATIC_REQUIRE(modern.h == 720);
    STATIC_REQUIRE(crt.w == 640);
    STATIC_REQUIRE(crt.h == 480);
    const video::IRect modern_full{0, 0, 1280, 720};
    const video::IRect modern_43{160, 0, 960, 720};
    const video::IRect crt_full{0, 0, 640, 480};
    const Row rows[] = {
        // Modern TV, 1920x1080
        {true, false, false, 1920, 1080, {240, 0, 1440, 1080}, modern_43, true},
        {true, true, false, 1920, 1080, {0, 0, 1920, 1080}, modern_full, false},
        {true, false, true, 1920, 1080, {0, 0, 1920, 1080}, modern_full, false},
        {true, true, true, 1920, 1080, {0, 0, 1920, 1080}, modern_full, false},
        // Modern TV, 1280x720
        {true, false, false, 1280, 720, {160, 0, 960, 720}, modern_43, true},
        {true, true, false, 1280, 720, {0, 0, 1280, 720}, modern_full, false},
        {true, false, true, 1280, 720, {0, 0, 1280, 720}, modern_full, false},
        {true, true, true, 1280, 720, {0, 0, 1280, 720}, modern_full, false},
        // Modern TV, SD fallbacks
        {true, false, false, 720, 480, {40, 0, 640, 480}, modern_43, true},
        {true, false, false, 720, 576, {0, 18, 720, 540}, modern_43, true},
        {true, true, false, 720, 576, {0, 0, 720, 576}, modern_full, false},
    };
    for (const auto& r : rows) {
        CAPTURE(r.modern_tv, r.film, r.mb, r.fb_w, r.fb_h);
        const auto got = video::content_rects(r.modern_tv, r.film, r.mb, r.fb_w, r.fb_h,
                                              modern.w, modern.h);
        CHECK(got.content == r.content);
        CHECK(got.logical_content == r.logical);
        CHECK(got.letterbox_active == r.letterbox);
    }
    // CRT Native never letterboxes: whole framebuffer, whole 640x480
    // canvas, whatever the film or Media Browser state.
    for (bool film : {false, true}) {
        for (bool mb : {false, true}) {
            for (auto fb : {video::IRect{0, 0, 1280, 720}, video::IRect{0, 0, 1920, 1080},
                            video::IRect{0, 0, 720, 480}, video::IRect{0, 0, 720, 576}}) {
                CAPTURE(film, mb, fb.w, fb.h);
                const auto got = video::content_rects(false, film, mb, fb.w, fb.h, crt.w, crt.h);
                CHECK(got.content == fb);
                CHECK(got.logical_content == crt_full);
                CHECK_FALSE(got.letterbox_active);
            }
        }
    }
}

TEST_CASE("Widescreen guide movies share the Marquee playback opening", "[content_area]") {
    CHECK(video::marquee_opening(1280, 720) == video::IRect{40, 40, 1200, 640});
    CHECK(video::marquee_opening(1920, 1080) == video::IRect{60, 60, 1800, 960});
    for (double aspect : {1.66, 16.0 / 9.0, 2.39}) {
        CHECK(video::framed_guide_movie(true, true, true, aspect));
        CHECK_FALSE(video::framed_guide_movie(false, true, true, aspect));
        CHECK_FALSE(video::framed_guide_movie(true, false, true, aspect));
        CHECK_FALSE(video::framed_guide_movie(true, true, false, aspect));
    }
    for (double aspect : {0.0, 0.75, 4.0 / 3.0, 1.35})
        CHECK_FALSE(video::framed_guide_movie(true, true, true, aspect));
}
