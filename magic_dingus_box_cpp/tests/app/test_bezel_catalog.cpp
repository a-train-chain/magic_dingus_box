// The bezel catalog (assets/bezels/bezels.json) and each frame's visible
// opening.
//
// With the picture shown whole ("fit"), playlist video has to sit inside
// the bezel's transparent window, not under its art, so every bezel with an
// image declares that window in its 1920x1080 image pixels. The last case
// measures the shipped PNGs and holds every declared opening to them, so a
// redrawn frame cannot drift away from its catalog entry unnoticed.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "app/bezel_catalog.h"
#include "utils/stb_image.h"

namespace fs = std::filesystem;

namespace {

const fs::path kBezelDir =
    fs::path(__FILE__).parent_path() / ".." / ".." / "assets" / "bezels";

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Box {
    int x = 0, y = 0, w = 0, h = 0;
};

// Bounding box of the pixels with alpha < 16 — the window the video shows
// through. All zero when the image has no such pixel.
Box transparent_box(const unsigned char* rgba, int w, int h) {
    int min_x = w, min_y = h, max_x = -1, max_y = -1;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (rgba[(static_cast<size_t>(y) * w + x) * 4 + 3] >= 16) continue;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    if (max_x < 0) return Box{};
    return Box{min_x, min_y, max_x - min_x + 1, max_y - min_y + 1};
}

}  // namespace

TEST_CASE("parse_bezel_catalog reads each entry and its opening",
          "[bezel_catalog]") {
    const std::string json = R"({
      "bezels": [
        {"id": "framed", "name": "Framed", "file": "framed.png",
         "description": "has a window",
         "opening": {"x": 284, "y": 48, "w": 1348, "h": 984}},
        {"id": "procedural", "name": "Simple", "file": null,
         "description": "draws nothing"},
        {"id": "nameless", "file": "nameless.png", "description": "skipped"}
      ]
    })";

    const auto bezels = app::parse_bezel_catalog(json);

    // The entry without a name is dropped, as the loader always did.
    REQUIRE(bezels.size() == 2);

    CHECK(bezels[0].id == "framed");
    CHECK(bezels[0].name == "Framed");
    CHECK(bezels[0].file == "framed.png");
    CHECK(bezels[0].description == "has a window");
    CHECK(bezels[0].has_opening);
    CHECK(bezels[0].opening_x == 284);
    CHECK(bezels[0].opening_y == 48);
    CHECK(bezels[0].opening_w == 1348);
    CHECK(bezels[0].opening_h == 984);

    CHECK(bezels[1].id == "procedural");
    CHECK(bezels[1].file.empty());
    CHECK_FALSE(bezels[1].has_opening);
}

TEST_CASE("an opening that is not a rect inside the image is no opening",
          "[bezel_catalog]") {
    const std::string json = R"({
      "bezels": [
        {"name": "missing h", "file": "a.png",
         "opening": {"x": 284, "y": 48, "w": 1348}},
        {"name": "zero width", "file": "b.png",
         "opening": {"x": 284, "y": 48, "w": 0, "h": 984}},
        {"name": "string x", "file": "c.png",
         "opening": {"x": "284", "y": 48, "w": 1348, "h": 984}},
        {"name": "negative y", "file": "d.png",
         "opening": {"x": 284, "y": -1, "w": 1348, "h": 984}},
        {"name": "past the right edge", "file": "e.png",
         "opening": {"x": 600, "y": 48, "w": 1348, "h": 984}},
        {"name": "not an object", "file": "f.png", "opening": [284, 48, 1348, 984]}
      ]
    })";

    const auto bezels = app::parse_bezel_catalog(json);

    // The entries still load — only their openings are refused, so the
    // video falls back to the whole 4:3 area.
    REQUIRE(bezels.size() == 6);
    for (const auto& b : bezels) {
        INFO(b.name);
        CHECK_FALSE(b.file.empty());
        CHECK_FALSE(b.has_opening);
    }
}

TEST_CASE("parse_bezel_catalog returns nothing for a malformed catalog",
          "[bezel_catalog]") {
    CHECK(app::parse_bezel_catalog("").empty());
    CHECK(app::parse_bezel_catalog(R"({"bezels": [)").empty());
    CHECK(app::parse_bezel_catalog("[]").empty());
    CHECK(app::parse_bezel_catalog(R"({"bezels": {}})").empty());
    // A stray non-object entry is skipped, not fatal to the rest.
    const auto mixed = app::parse_bezel_catalog(
        R"({"bezels": ["junk", 7, {"name": "Kept", "file": null}]})");
    REQUIRE(mixed.size() == 1);
    CHECK(mixed[0].name == "Kept");
}

TEST_CASE("parse_bezel_catalog says why a catalog or an opening was refused",
          "[bezel_catalog]") {
    // The kiosk logs these: without them a bad catalog was only "No bezels
    // parsed", and a refused opening was silent.
    std::vector<std::string> problems;
    CHECK(app::parse_bezel_catalog(R"({"bezels": [)", &problems).empty());
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].rfind("not valid JSON: ", 0) == 0);
    CHECK(problems[0].find("Line 1") != std::string::npos);  // jsoncpp's text
    // One log line: jsoncpp's "* Line N, Column M\n  <message>" is joined.
    CHECK(problems[0].find('\n') == std::string::npos);
    CHECK(problems[0].find("; ") != std::string::npos);

    problems.clear();
    CHECK(app::parse_bezel_catalog(R"({"bezels": {}})", &problems).empty());
    REQUIRE(problems.size() == 1);
    CHECK(problems[0] == "no \"bezels\" array");

    problems.clear();
    const auto bezels = app::parse_bezel_catalog(R"({"bezels": [
        {"name": "Good", "file": "g.png",
         "opening": {"x": 284, "y": 48, "w": 1348, "h": 984}},
        {"name": "No opening", "file": null},
        {"name": "Missing h", "file": "a.png", "opening": {"x": 1, "y": 1, "w": 1}},
        {"name": "Too wide", "file": "b.png",
         "opening": {"x": 600, "y": 48, "w": 1348, "h": 984}},
        {"name": "Array", "file": "c.png", "opening": [1, 2, 3, 4]}
    ]})", &problems);
    REQUIRE(bezels.size() == 5);
    // One line per refused opening, naming the bezel; a valid opening and an
    // absent one say nothing.
    REQUIRE(problems.size() == 3);
    CHECK(problems[0].find("\"Missing h\"") != std::string::npos);
    CHECK(problems[0].find("\"h\"") != std::string::npos);
    CHECK(problems[1].find("\"Too wide\"") != std::string::npos);
    CHECK(problems[1].find("1920x1080") != std::string::npos);
    CHECK(problems[2].find("\"Array\"") != std::string::npos);
    CHECK(problems[2].find("not an object") != std::string::npos);
}

TEST_CASE("every shipped bezel declares the opening its image shows through",
          "[bezel_catalog]") {
    const fs::path catalog = kBezelDir / "bezels.json";
    REQUIRE(fs::exists(catalog));
    std::vector<std::string> problems;
    const auto bezels = app::parse_bezel_catalog(read_file(catalog), &problems);
    REQUIRE_FALSE(bezels.empty());
    CHECK(problems.empty());

    int with_image = 0;
    for (const auto& b : bezels) {
        INFO("bezel " << b.id);
        if (b.file.empty()) {
            // Procedural draws nothing; its opening is the whole 4:3 area.
            CHECK_FALSE(b.has_opening);
            continue;
        }
        ++with_image;

        int w = 0, h = 0, channels = 0;
        unsigned char* rgba =
            stbi_load((kBezelDir / b.file).string().c_str(), &w, &h, &channels, 4);
        REQUIRE(rgba != nullptr);
        const Box m = transparent_box(rgba, w, h);
        stbi_image_free(rgba);

        INFO("measured opening x" << m.x << " y" << m.y << " w" << m.w
                                  << " h" << m.h);
        CHECK(w == app::kBezelImageWidth);
        CHECK(h == app::kBezelImageHeight);
        CHECK(m.w > 0);
        CHECK(b.has_opening);
        // Each edge within 2 px of the measured window.
        CHECK(std::abs(b.opening_x - m.x) <= 2);
        CHECK(std::abs(b.opening_y - m.y) <= 2);
        CHECK(std::abs((b.opening_x + b.opening_w) - (m.x + m.w)) <= 2);
        CHECK(std::abs((b.opening_y + b.opening_h) - (m.y + m.h)) <= 2);
    }
    CHECK(with_image > 0);
}
