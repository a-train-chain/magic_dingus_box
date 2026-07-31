// Unit tests for DRM mode selection.
//
// The bench TV advertises ELEVEN 1920x1080 modes. The old code matched on
// resolution and broke on the first hit, so which refresh rate you got was
// whatever order the kernel happened to list them in. Landing on 1080p24 or
// 1080p30 would clamp the ENTIRE kiosk to that frame rate, because the main
// loop blocks waiting on the page-flip event.

#include <catch2/catch_test_macros.hpp>

#include "platform/mode_selection.h"

using platform::ModeCandidate;
using platform::pick_mode;

// vrefresh is in whole Hz here, matching drmModeModeInfo::vrefresh.
static const std::vector<ModeCandidate> kElevenModes = {
    {1920, 1080, 24, false},
    {1920, 1080, 25, false},
    {1920, 1080, 30, false},
    {1920, 1080, 50, false},
    {1920, 1080, 60, false},
    {1920, 1080, 30, false},
    {1280, 720,  60, false},
    {1280, 720,  50, false},
    {3840, 2160, 30, true},   // TV's preferred mode is 4K
    {1920, 1080, 24, false},
    {1920, 1080, 60, false},
};

TEST_CASE("picks the highest refresh among modes of the requested size") {
    // This is the whole point: the first 1920x1080 in the list is 24Hz.
    int i = pick_mode(kElevenModes, 1920, 1080);
    REQUIRE(i >= 0);
    REQUIRE(kElevenModes[i].width == 1920);
    REQUIRE(kElevenModes[i].height == 1080);
    REQUIRE(kElevenModes[i].vrefresh == 60);
}

TEST_CASE("resolution is honored even when the TV prefers a different one") {
    // The TV's DRM_MODE_TYPE_PREFERRED mode here is 4K. An explicit request
    // for 1080p must NOT be hijacked by it.
    int i = pick_mode(kElevenModes, 1920, 1080);
    REQUIRE(kElevenModes[i].width != 3840);
}

TEST_CASE("720p request still resolves to 60Hz — CRT path must not regress") {
    int i = pick_mode(kElevenModes, 1280, 720);
    REQUIRE(i >= 0);
    REQUIRE(kElevenModes[i].width == 1280);
    REQUIRE(kElevenModes[i].height == 720);
    REQUIRE(kElevenModes[i].vrefresh == 60);
}

TEST_CASE("unavailable resolution reports no match rather than guessing") {
    REQUIRE(pick_mode(kElevenModes, 800, 600) == -1);
    REQUIRE(pick_mode({}, 1920, 1080) == -1);
}

TEST_CASE("auto mode (0x0) takes the TV's preferred mode") {
    int i = pick_mode(kElevenModes, 0, 0);
    REQUIRE(i >= 0);
    REQUIRE(kElevenModes[i].is_preferred);
}

TEST_CASE("auto mode falls back to highest refresh when nothing is preferred") {
    std::vector<ModeCandidate> no_preferred = {
        {1280, 720, 30, false},
        {1280, 720, 60, false},
        {1280, 720, 50, false},
    };
    int i = pick_mode(no_preferred, 0, 0);
    REQUIRE(i >= 0);
    REQUIRE(no_preferred[i].vrefresh == 60);
}

TEST_CASE("a preferred mode at the requested size wins over a faster one") {
    // If the panel explicitly prefers a mode at the size we asked for,
    // trust it — EDID-preferred timings are the ones most likely to be
    // correctly implemented by the display.
    std::vector<ModeCandidate> modes = {
        {1920, 1080, 60, false},
        {1920, 1080, 50, true},
    };
    int i = pick_mode(modes, 1920, 1080);
    REQUIRE(modes[i].is_preferred);
    REQUIRE(modes[i].vrefresh == 50);
}

TEST_CASE("selection is stable — first of equally-ranked modes") {
    std::vector<ModeCandidate> modes = {
        {1920, 1080, 60, false},
        {1920, 1080, 60, false},
    };
    REQUIRE(pick_mode(modes, 1920, 1080) == 0);
}


TEST_CASE("progressive beats interlaced even at lower refresh") {
    // A TV advertising 1080i60 alongside only lower-rate progressive
    // timings: raw refresh comparison picked i60, and the kiosk's
    // pipeline never deinterlaces — combing on every menu and video.
    std::vector<ModeCandidate> modes = {
        {1920, 1080, 60, false, /*interlaced=*/true},
        {1920, 1080, 30, false, /*interlaced=*/false},
    };
    REQUIRE(pick_mode(modes, 1920, 1080) == 1);
}

TEST_CASE("among progressive modes refresh still decides") {
    std::vector<ModeCandidate> modes = {
        {1920, 1080, 60, false, /*interlaced=*/true},
        {1920, 1080, 30, false, /*interlaced=*/false},
        {1920, 1080, 50, false, /*interlaced=*/false},
    };
    REQUIRE(pick_mode(modes, 1920, 1080) == 2);
}

TEST_CASE("an interlaced-only size is still selectable") {
    std::vector<ModeCandidate> modes = {
        {1920, 1080, 60, false, /*interlaced=*/true},
    };
    REQUIRE(pick_mode(modes, 1920, 1080) == 0);
}

TEST_CASE("EDID-preferred outranks progressive-ness") {
    // A panel whose PREFERRED timing is interlaced is interlaced-native;
    // honoring the EDID stays the safer bet on cheap kiosk displays.
    std::vector<ModeCandidate> modes = {
        {1920, 1080, 30, false, /*interlaced=*/false},
        {1920, 1080, 60, true,  /*interlaced=*/true},
    };
    REQUIRE(pick_mode(modes, 1920, 1080) == 1);
}
