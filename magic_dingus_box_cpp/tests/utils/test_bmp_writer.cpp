// utils::encode_bmp24_from_rgba + screenshot_store — the file side of the
// kiosk's debug screenshot hook (debug/screenshot_capture).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "utils/bmp_writer.h"
#include "utils/screenshot_store.h"

namespace fs = std::filesystem;

namespace {

std::uint32_t u32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) |
           (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) |
           (static_cast<std::uint32_t>(b[at + 3]) << 24);
}
std::uint16_t u16(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

// width x height RGBA where pixel (x, y) = (x, y, 100 + x + y, 255);
// y = 0 is the first (bottom, GL-order) row.
std::vector<std::uint8_t> gradient(int w, int h) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w * h * 4));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint8_t* p = &px[static_cast<std::size_t>((y * w + x) * 4)];
            p[0] = static_cast<std::uint8_t>(x);
            p[1] = static_cast<std::uint8_t>(y);
            p[2] = static_cast<std::uint8_t>(100 + x + y);
            p[3] = 255;
        }
    }
    return px;
}

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("mdb_shot_test_" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void touch(const fs::path& p) { std::ofstream(p) << "x"; }

}  // namespace

TEST_CASE("bmp24_row_stride pads rows to 4 bytes", "[bmp]") {
    CHECK(utils::bmp24_row_stride(0) == 0);
    CHECK(utils::bmp24_row_stride(-3) == 0);
    CHECK(utils::bmp24_row_stride(1) == 4);   // 3 -> 4
    CHECK(utils::bmp24_row_stride(2) == 8);   // 6 -> 8
    CHECK(utils::bmp24_row_stride(3) == 12);  // 9 -> 12
    CHECK(utils::bmp24_row_stride(4) == 12);  // 12, already aligned
    CHECK(utils::bmp24_row_stride(5) == 16);  // 15 -> 16
    CHECK(utils::bmp24_row_stride(1920) == 5760);
}

TEST_CASE("BMP header fields", "[bmp]") {
    const int w = 3, h = 2;  // 9-byte rows -> 12 with padding
    const auto px = gradient(w, h);
    const auto bmp = utils::encode_bmp24_from_rgba(w, h, px.data(), px.size());

    REQUIRE(bmp.size() == 54u + 12u * 2u);
    CHECK(bmp[0] == 'B');
    CHECK(bmp[1] == 'M');
    CHECK(u32(bmp, 2) == bmp.size());      // file size
    CHECK(u32(bmp, 6) == 0u);              // reserved
    CHECK(u32(bmp, 10) == 54u);            // pixel data offset
    CHECK(u32(bmp, 14) == 40u);            // BITMAPINFOHEADER
    CHECK(u32(bmp, 18) == 3u);             // width
    CHECK(u32(bmp, 22) == 2u);             // height > 0: bottom-up
    CHECK(u16(bmp, 26) == 1u);             // planes
    CHECK(u16(bmp, 28) == 24u);            // bpp
    CHECK(u32(bmp, 30) == 0u);             // BI_RGB
    CHECK(u32(bmp, 34) == 24u);            // image size incl. padding
    CHECK(u32(bmp, 46) == 0u);             // colours used
    CHECK(u32(bmp, 50) == 0u);             // important colours
}

TEST_CASE("BMP rows are bottom-up BGR with zeroed padding", "[bmp]") {
    const int w = 3, h = 2;
    const auto px = gradient(w, h);
    const auto bmp = utils::encode_bmp24_from_rgba(w, h, px.data(), px.size());
    REQUIRE(bmp.size() == 78u);

    const std::size_t stride = 12;
    for (int y = 0; y < h; ++y) {
        // GL row y (bottom = 0) is BMP row y (bottom-up): straight copy.
        const std::size_t row = 54 + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            const std::size_t at = row + static_cast<std::size_t>(x) * 3;
            CHECK(bmp[at + 0] == static_cast<std::uint8_t>(100 + x + y));  // B
            CHECK(bmp[at + 1] == static_cast<std::uint8_t>(y));            // G
            CHECK(bmp[at + 2] == static_cast<std::uint8_t>(x));            // R
        }
        for (std::size_t pad = 9; pad < stride; ++pad) {
            CHECK(bmp[row + pad] == 0);
        }
    }
}

TEST_CASE("BMP with aligned rows has no padding", "[bmp]") {
    const auto px = gradient(4, 1);
    const auto bmp = utils::encode_bmp24_from_rgba(4, 1, px.data(), px.size());
    REQUIRE(bmp.size() == 54u + 12u);
    CHECK(u32(bmp, 34) == 12u);
}

TEST_CASE("BMP encode rejects bad input", "[bmp]") {
    const auto px = gradient(2, 2);
    CHECK(utils::encode_bmp24_from_rgba(0, 2, px.data(), px.size()).empty());
    CHECK(utils::encode_bmp24_from_rgba(2, 0, px.data(), px.size()).empty());
    CHECK(utils::encode_bmp24_from_rgba(-1, 2, px.data(), px.size()).empty());
    CHECK(utils::encode_bmp24_from_rgba(2, 2, nullptr, px.size()).empty());
    // One byte short of 2x2 RGBA.
    CHECK(utils::encode_bmp24_from_rgba(2, 2, px.data(), px.size() - 1).empty());
}

TEST_CASE("screenshot_filename is fixed-width UTC with milliseconds", "[screenshot]") {
    using namespace std::chrono;
    // 2026-10-04T15:30:12.345Z
    const system_clock::time_point t =
        system_clock::time_point(seconds(1791127812)) + milliseconds(345);
    CHECK(utils::screenshot_filename(t) == "20261004T153012_345Z.bmp");
    CHECK(utils::screenshot_filename(system_clock::time_point(seconds(1791127812))) ==
          "20261004T153012_000Z.bmp");
    // Later time sorts later as a plain string.
    CHECK(utils::screenshot_filename(t) <
          utils::screenshot_filename(t + milliseconds(1)));
}

TEST_CASE("write_file_atomic writes via .tmp and creates the directory", "[screenshot]") {
    TempDir tmp;
    const fs::path target = tmp.path / "screenshots" / "a.bmp";
    const std::vector<std::uint8_t> bytes = {1, 2, 3, 4, 5};
    std::string err;
    REQUIRE(utils::write_file_atomic(target.string(), bytes, &err));
    CHECK(err.empty());
    CHECK(fs::exists(target));
    CHECK_FALSE(fs::exists(target.string() + ".tmp"));
    std::ifstream f(target, std::ios::binary);
    std::vector<std::uint8_t> back((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
    CHECK(back == bytes);
}

TEST_CASE("write_file_atomic reports failure", "[screenshot]") {
    TempDir tmp;
    // Parent "directory" is a regular file: cannot create it.
    touch(tmp.path / "blocker");
    std::string err;
    CHECK_FALSE(utils::write_file_atomic((tmp.path / "blocker" / "x.bmp").string(),
                                         {1}, &err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("prune_screenshots keeps the newest N and sweeps leftovers", "[screenshot]") {
    TempDir tmp;
    for (int i = 0; i < 13; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "20261004T1530%02d_000Z.bmp", i);
        touch(tmp.path / name);
    }
    touch(tmp.path / "20261004T153099_000Z.bmp.tmp");  // interrupted write
    touch(tmp.path / "notes.txt");                      // not ours
    fs::create_directories(tmp.path / "old.bmp");       // dir: not touched

    CHECK(utils::prune_screenshots(tmp.path.string(), 10) == 4u);

    std::vector<std::string> left;
    for (const auto& e : fs::directory_iterator(tmp.path)) {
        left.push_back(e.path().filename().string());
    }
    CHECK(left.size() == 12u);  // 10 shots + notes.txt + old.bmp dir
    CHECK_FALSE(fs::exists(tmp.path / "20261004T153000_000Z.bmp"));
    CHECK_FALSE(fs::exists(tmp.path / "20261004T153002_000Z.bmp"));
    CHECK(fs::exists(tmp.path / "20261004T153003_000Z.bmp"));
    CHECK(fs::exists(tmp.path / "20261004T153012_000Z.bmp"));
    CHECK_FALSE(fs::exists(tmp.path / "20261004T153099_000Z.bmp.tmp"));
    CHECK(fs::exists(tmp.path / "notes.txt"));
    CHECK(fs::is_directory(tmp.path / "old.bmp"));
}

TEST_CASE("prune_screenshots on a missing directory is a no-op", "[screenshot]") {
    CHECK(utils::prune_screenshots("/nonexistent/mdb/screenshots", 10) == 0u);
}
