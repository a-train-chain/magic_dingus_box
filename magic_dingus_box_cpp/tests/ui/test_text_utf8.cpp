// Tests for ui/text_utf8.h -- the UTF-8 primitives every "shrink this string
// to fit a pixel budget" loop in the kiosk depends on.
//
// These live in test_ui_unit rather than test_media_browser_unit on purpose:
// text_utf8.h is header-only over <cstdint>/<string> with no GL and no link
// tail, and test_ui_unit builds regardless of ENABLE_MEDIA_BROWSER. The
// primitive is used from ui/renderer.cpp as well as from the Media Browser
// screens, so it must stay covered in the default (MB=OFF) build too.
//
// The bug these exist to prevent: a cut at an arbitrary BYTE offset splits a
// multi-byte sequence, decode_utf8 returns U+FFFD for the orphan, and the
// kiosk draws a replacement box over a movie title.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "ui/text_utf8.h"

// Byte layouts used throughout, so every expectation below is checkable by eye:
//   "é"  U+00E9   = C3 A9              (2 bytes)
//   "千" U+5343   = E5 8D 83           (3 bytes)
//   "•"  U+2022   = E2 80 A2           (3 bytes)
//   "🎬" U+1F3AC  = F0 9F 8E AC        (4 bytes)

// =====================================================================
// utf8_is_continuation
// =====================================================================

TEST_CASE("utf8_is_continuation identifies exactly the 10xxxxxx bytes",
          "[text_utf8]") {
    // ASCII: never a continuation byte.
    CHECK_FALSE(ui::utf8_is_continuation(0x00));
    CHECK_FALSE(ui::utf8_is_continuation('A'));
    CHECK_FALSE(ui::utf8_is_continuation(0x7F));

    // Continuation bytes span 0x80..0xBF inclusive -- and nothing else.
    CHECK(ui::utf8_is_continuation(0x80));
    CHECK(ui::utf8_is_continuation(0xA9));  // the tail of "é"
    CHECK(ui::utf8_is_continuation(0xBF));

    // Lead bytes of 2-, 3- and 4-byte sequences are boundaries, not
    // continuations. 0xC0 is the first non-continuation after the range,
    // which is the boundary the mask has to get right.
    CHECK_FALSE(ui::utf8_is_continuation(0xC0));
    CHECK_FALSE(ui::utf8_is_continuation(0xC3));  // lead of "é"
    CHECK_FALSE(ui::utf8_is_continuation(0xE5));  // lead of "千"
    CHECK_FALSE(ui::utf8_is_continuation(0xF0));  // lead of "🎬"
    CHECK_FALSE(ui::utf8_is_continuation(0xFF));
}

// =====================================================================
// utf8_floor_boundary
// =====================================================================

TEST_CASE("utf8_floor_boundary leaves ASCII lengths alone", "[text_utf8]") {
    // Every offset in an ASCII string is already a boundary, so the helper
    // must be the identity here -- otherwise it would silently shorten every
    // English title by a character.
    const std::string s = "Alien";
    for (std::size_t n = 0; n <= s.size(); ++n) {
        CHECK(ui::utf8_floor_boundary(s, n) == n);
    }
}

TEST_CASE("utf8_floor_boundary snaps back out of a 2-byte sequence",
          "[text_utf8]") {
    //  0  1   2  3   4
    //  A  m  C3 A9   l      ("Amél")
    const std::string s = "Amél";
    REQUIRE(s.size() == 5);

    CHECK(ui::utf8_floor_boundary(s, 0) == 0);
    CHECK(ui::utf8_floor_boundary(s, 1) == 1);
    CHECK(ui::utf8_floor_boundary(s, 2) == 2);  // just before "é": a boundary
    CHECK(ui::utf8_floor_boundary(s, 3) == 2);  // inside "é" -> snap back
    CHECK(ui::utf8_floor_boundary(s, 4) == 4);  // just after "é": a boundary
    CHECK(ui::utf8_floor_boundary(s, 5) == 5);
}

TEST_CASE("utf8_floor_boundary snaps back out of a 3-byte sequence",
          "[text_utf8]") {
    //  0  1  2  3  4  5
    // E5 8D 83 E3 81 A8    ("千と")
    const std::string s = "千と";
    REQUIRE(s.size() == 6);

    CHECK(ui::utf8_floor_boundary(s, 0) == 0);
    CHECK(ui::utf8_floor_boundary(s, 1) == 0);  // 1/3 into 千
    CHECK(ui::utf8_floor_boundary(s, 2) == 0);  // 2/3 into 千
    CHECK(ui::utf8_floor_boundary(s, 3) == 3);  // exactly after 千
    CHECK(ui::utf8_floor_boundary(s, 4) == 3);  // 1/3 into と
    CHECK(ui::utf8_floor_boundary(s, 5) == 3);  // 2/3 into と
    CHECK(ui::utf8_floor_boundary(s, 6) == 6);
}

TEST_CASE("utf8_floor_boundary snaps back out of a 4-byte sequence",
          "[text_utf8]") {
    //  0  1  2  3  4
    // F0 9F 8E AC 21       ("🎬!")
    const std::string s = "🎬!";
    REQUIRE(s.size() == 5);

    CHECK(ui::utf8_floor_boundary(s, 0) == 0);
    CHECK(ui::utf8_floor_boundary(s, 1) == 0);
    CHECK(ui::utf8_floor_boundary(s, 2) == 0);
    CHECK(ui::utf8_floor_boundary(s, 3) == 0);  // 3 of 4 bytes is still a split
    CHECK(ui::utf8_floor_boundary(s, 4) == 4);  // exactly after the emoji
    CHECK(ui::utf8_floor_boundary(s, 5) == 5);
}

TEST_CASE("utf8_floor_boundary clamps a length past the end", "[text_utf8]") {
    // Callers compute n from a pixel budget, so an n well past the end is
    // ordinary, not a bug. It must clamp rather than read out of bounds.
    const std::string s = "千";
    CHECK(ui::utf8_floor_boundary(s, 3) == 3);
    CHECK(ui::utf8_floor_boundary(s, 4) == 3);
    CHECK(ui::utf8_floor_boundary(s, 999) == 3);

    // Empty string: every length clamps to 0.
    CHECK(ui::utf8_floor_boundary("", 0) == 0);
    CHECK(ui::utf8_floor_boundary("", 5) == 0);
}

TEST_CASE("utf8_floor_boundary terminates on malformed input", "[text_utf8]") {
    // A string of bare continuation bytes has no boundary above 0. The walk
    // must bottom out at 0 rather than underflow size_t and read wildly.
    const std::string orphans = "\x80\x80\x80";
    CHECK(ui::utf8_floor_boundary(orphans, 1) == 0);
    CHECK(ui::utf8_floor_boundary(orphans, 2) == 0);
    CHECK(ui::utf8_floor_boundary(orphans, 3) == 3);  // == size, clamped

    // A truncated sequence: lead byte with only one of its two tails.
    const std::string truncated = "A\xE5\x8D";
    CHECK(ui::utf8_floor_boundary(truncated, 2) == 1);
    CHECK(ui::utf8_floor_boundary(truncated, 3) == 3);  // == size, clamped
}

// =====================================================================
// utf8_pop_back
// =====================================================================

TEST_CASE("utf8_pop_back drops a whole codepoint, not a byte", "[text_utf8]") {
    // This is the drop-in for `s.pop_back()` inside a shrink-to-fit loop.
    // Popping bytes is what put replacement boxes on accented titles.
    std::string ascii = "Alien";
    ui::utf8_pop_back(ascii);
    CHECK(ascii == "Alie");

    std::string two_byte = "Amé";
    REQUIRE(two_byte.size() == 4);
    ui::utf8_pop_back(two_byte);
    CHECK(two_byte == "Am");  // both bytes of "é" go, not just the tail
    CHECK(two_byte.size() == 2);

    std::string three_byte = "千と";
    REQUIRE(three_byte.size() == 6);
    ui::utf8_pop_back(three_byte);
    CHECK(three_byte == "千");
    CHECK(three_byte.size() == 3);

    std::string four_byte = "Hi🎬";
    REQUIRE(four_byte.size() == 6);
    ui::utf8_pop_back(four_byte);
    CHECK(four_byte == "Hi");
    CHECK(four_byte.size() == 2);
}

TEST_CASE("utf8_pop_back empties a string one codepoint at a time",
          "[text_utf8]") {
    // Callers use it as a loop body, so it has to keep making progress and
    // stop cleanly at empty -- never spin, never underflow.
    std::string s = "Drama • 千🎬";
    int guard = 0;
    while (!s.empty() && guard++ < 100) {
        const std::size_t before = s.size();
        ui::utf8_pop_back(s);
        REQUIRE(s.size() < before);  // progress every iteration
    }
    CHECK(s.empty());
    CHECK(guard < 100);

    // A no-op on empty, not a crash.
    std::string empty;
    ui::utf8_pop_back(empty);
    CHECK(empty.empty());
}

TEST_CASE("utf8_pop_back leaves no orphaned bytes behind", "[text_utf8]") {
    // The property that matters: whatever remains must re-decode with no
    // U+FFFD. Asserted at every intermediate length, since a shrink-to-fit
    // loop can stop at any one of them.
    std::string s = "Amélie 千と🎬 Drama • Fantasy";
    while (!s.empty()) {
        std::size_t pos = 0;
        while (pos < s.size()) {
            const std::size_t before = pos;
            const char32_t cp = ui::decode_utf8(s, pos);
            INFO("remainder=" << s << " byte_offset=" << before);
            CHECK(cp != 0xFFFD);
            REQUIRE(pos > before);
        }
        ui::utf8_pop_back(s);
    }
}
