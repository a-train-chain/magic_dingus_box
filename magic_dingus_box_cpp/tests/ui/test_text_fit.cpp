// Tests for ui/text_fit.h: fitting the Channel Guide panel's free-text
// strings (playlist title, item title, artist, next title) to a pixel width
// with the font's real measurement, ending in "…", never cutting a UTF-8
// sequence. The measure here is a fake: 10 px per code point unless a test
// says otherwise ("…" is one code point, so 10 px too).

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "ui/text_fit.h"
#include "ui/text_utf8.h"

namespace {

int ten_per_cp(const std::string& s) {
    int n = 0;
    for (unsigned char c : s)
        if (!ui::utf8_is_continuation(c)) ++n;
    return 10 * n;
}

// Every byte sequence decodes without a replacement character.
bool valid_utf8(const std::string& s) {
    std::size_t pos = 0;
    while (pos < s.size()) {
        if (ui::decode_utf8(s, pos) == 0xFFFD) return false;
    }
    return true;
}

const std::string kEll = "\xE2\x80\xA6";

}  // namespace

TEST_CASE("fit_to_width keeps a string that fits", "[text_fit]") {
    CHECK(ui::fit_to_width("ABCDE", 50, ten_per_cp) == "ABCDE");
    CHECK(ui::fit_to_width("", 0, ten_per_cp) == "");
}

TEST_CASE("fit_to_width cuts to the widest prefix that fits with the ellipsis",
          "[text_fit]") {
    // 10 code points = 100 px; 55 px holds 4 + "…".
    const std::string out = ui::fit_to_width("ABCDEFGHIJ", 55, ten_per_cp);
    CHECK(out == "ABCD" + kEll);
    CHECK(ten_per_cp(out) <= 55);
    // Exactly on the boundary: 5 + "…" = 60.
    CHECK(ui::fit_to_width("ABCDEFGHIJ", 60, ten_per_cp) == "ABCDE" + kEll);
}

TEST_CASE("fit_to_width uses the measure, not a character count", "[text_fit]") {
    // A proportional face: 'W' 20 px, 'i' 4 px, "…" 10 px.
    auto prop = [](const std::string& s) {
        int w = 0;
        std::size_t pos = 0;
        while (pos < s.size()) {
            const char32_t c = ui::decode_utf8(s, pos);
            w += c == U'W' ? 20 : c == U'i' ? 4 : 10;
        }
        return w;
    };
    // "iiiiiiiiii" = 40 px fits 40; "WWWWW" = 100 px does not fit 40.
    CHECK(ui::fit_to_width("iiiiiiiiii", 40, prop) == "iiiiiiiiii");
    CHECK(ui::fit_to_width("WWWWW", 40, prop) == "W" + kEll);       // 20 + 10
    CHECK(ui::fit_to_width("iiiiiiiiiiW", 40, prop) == "iiiiiii" + kEll);  // 28 + 10
}

TEST_CASE("fit_to_width never cuts inside a UTF-8 sequence", "[text_fit]") {
    // "é" (2 bytes), "千" (3 bytes), "🎬" (4 bytes): 8 code points, 80 px.
    const std::string s = "\xC3\xA9\xE5\x8D\x83\xF0\x9F\x8E\xAC\xC3\xA9\xE5\x8D\x83\xF0\x9F\x8E\xAC"
                          "\xC3\xA9\xE5\x8D\x83";
    for (int w = 10; w <= 90; ++w) {
        const std::string out = ui::fit_to_width(s, w, ten_per_cp);
        INFO("max_w " << w << " -> " << out);
        CHECK(valid_utf8(out));
        CHECK(ten_per_cp(out) <= w);
    }
    CHECK(ui::fit_to_width(s, 35, ten_per_cp) == "\xC3\xA9\xE5\x8D\x83" + kEll);
}

TEST_CASE("fit_to_width drops spaces before the ellipsis", "[text_fit]") {
    // "AB CDEFG" at 40 px: 3 code points "AB " + "…" -> "AB…".
    CHECK(ui::fit_to_width("AB CDEFG", 40, ten_per_cp) == "AB" + kEll);
}

TEST_CASE("fit_to_width returns nothing when not even the ellipsis fits",
          "[text_fit]") {
    CHECK(ui::fit_to_width("ABCDEF", 9, ten_per_cp) == "");
    CHECK(ui::fit_to_width("ABCDEF", 10, ten_per_cp) == kEll);
}

TEST_CASE("wrap_to_lines keeps one line when it fits", "[text_fit]") {
    const auto lines = ui::wrap_to_lines("Sacred Steel", 150, 2, ten_per_cp);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "Sacred Steel");
}

TEST_CASE("wrap_to_lines breaks at spaces and fits the last line", "[text_fit]") {
    // 150 px = 15 code points a line.
    const auto lines =
        ui::wrap_to_lines("Obscure Guitar for Ruining Parties", 150, 2, ten_per_cp);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "Obscure Guitar");
    CHECK(lines[1] == "for Ruining Pa" + kEll);
    for (const auto& l : lines) CHECK(ten_per_cp(l) <= 150);
}

TEST_CASE("wrap_to_lines puts the rest on the second line whole when it fits",
          "[text_fit]") {
    const auto lines = ui::wrap_to_lines("Chill Guitar for Good Vibes", 150, 2, ten_per_cp);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "Chill Guitar");
    CHECK(lines[1] == "for Good Vibes");
}

TEST_CASE("wrap_to_lines cuts a word wider than a line between code points",
          "[text_fit]") {
    const auto lines =
        ui::wrap_to_lines("Supercalifragilistic expialidocious", 100, 2, ten_per_cp);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "Supercalif");
    CHECK(lines[1] == "ragilisti" + kEll);
    // A multi-byte word cut stays valid UTF-8.
    const auto u = ui::wrap_to_lines(
        "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9 x", 30, 2, ten_per_cp);
    REQUIRE(u.size() == 2);
    CHECK(u[0] == "\xC3\xA9\xC3\xA9\xC3\xA9");
    CHECK(valid_utf8(u[1]));
}

TEST_CASE("wrap_to_lines with one line is fit_to_width", "[text_fit]") {
    const auto lines = ui::wrap_to_lines("ABCDEFGHIJ", 55, 1, ten_per_cp);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "ABCD" + kEll);
    CHECK(ui::wrap_to_lines("", 55, 2, ten_per_cp).empty());
    CHECK(ui::wrap_to_lines("ABC", 55, 0, ten_per_cp).empty());
}
