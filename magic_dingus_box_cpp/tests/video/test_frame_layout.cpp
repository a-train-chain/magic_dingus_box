// Raw video caps carry no stride field, so the old upload path assumed
// stride == width. GStreamer pads rows to 4 bytes: an 854x480 I420 frame
// has a 856-byte Y stride and 428-byte chroma strides, and uploading it
// with row length 854 sheared the picture diagonally and smeared colour.
// compute_frame_upload turns GStreamer's real strides into GL upload
// parameters; these tests pin the arithmetic.

#include <catch2/catch_test_macros.hpp>

#include "video/frame_layout.h"

using video::compute_frame_upload;
using video::RawFormat;

TEST_CASE("854x480 I420 uses the padded strides, real chroma width",
          "[video][frame_layout]") {
    // GStreamer default layout: Y stride RU4(854)=856, chroma RU4(427)=428.
    const auto f = compute_frame_upload(RawFormat::I420, 854, 480, {856, 428, 428});
    REQUIRE(f.valid());
    REQUIRE(f.n_planes == 3);
    REQUIRE(f.planes[0].row_length == 856);
    REQUIRE(f.planes[0].tex_width == 854);
    REQUIRE(f.planes[0].tex_height == 480);
    // Chroma texture is the REAL chroma width, not the stride — sampling
    // the padding column would stretch colour rightwards.
    REQUIRE(f.planes[1].tex_width == 427);
    REQUIRE(f.planes[1].row_length == 428);
    REQUIRE(f.planes[1].tex_height == 240);
    REQUIRE(f.planes[2].tex_width == 427);
    REQUIRE(f.planes[2].row_length == 428);
}

TEST_CASE("odd dimensions round chroma up", "[video][frame_layout]") {
    const auto f = compute_frame_upload(RawFormat::I420, 853, 481, {856, 428, 428});
    REQUIRE(f.valid());
    REQUIRE(f.planes[1].tex_width == 427);
    REQUIRE(f.planes[1].tex_height == 241);
}

TEST_CASE("NV12 chroma row length is counted in RG texels",
          "[video][frame_layout]") {
    // 854 wide NV12: Y stride 856, UV stride 856 bytes = 428 RG texels.
    const auto f = compute_frame_upload(RawFormat::NV12, 854, 480, {856, 856, 0});
    REQUIRE(f.valid());
    REQUIRE(f.n_planes == 2);
    REQUIRE(f.planes[0].row_length == 856);
    REQUIRE(f.planes[1].tex_width == 427);
    REQUIRE(f.planes[1].row_length == 428);
}

TEST_CASE("decoder-aligned strides (GstVideoMeta) are honoured",
          "[video][frame_layout]") {
    // e.g. a hardware decoder padding rows to 64 bytes.
    const auto f = compute_frame_upload(RawFormat::NV12, 1920, 1080, {1920, 1920, 0});
    REQUIRE(f.valid());
    const auto g = compute_frame_upload(RawFormat::I420, 1280, 720, {1344, 704, 704});
    REQUIRE(g.valid());
    REQUIRE(g.planes[0].row_length == 1344);
    REQUIRE(g.planes[1].row_length == 704);
    REQUIRE(g.planes[1].tex_width == 640);
}

TEST_CASE("RGBA row length is stride / 4", "[video][frame_layout]") {
    const auto f = compute_frame_upload(RawFormat::RGBA, 854, 480, {3416, 0, 0});
    REQUIRE(f.valid());
    REQUIRE(f.planes[0].row_length == 854);
    const auto padded = compute_frame_upload(RawFormat::RGBA, 10, 2, {48, 0, 0});
    REQUIRE(padded.valid());
    REQUIRE(padded.planes[0].row_length == 12);
}

TEST_CASE("impossible strides are rejected, not uploaded",
          "[video][frame_layout]") {
    // Stride smaller than a row would read past the plane.
    REQUIRE_FALSE(compute_frame_upload(RawFormat::I420, 854, 480, {854, 400, 428}).valid());
    // RGBA / RG strides must be whole texels.
    REQUIRE_FALSE(compute_frame_upload(RawFormat::RGBA, 10, 2, {42, 0, 0}).valid());
    REQUIRE_FALSE(compute_frame_upload(RawFormat::NV12, 10, 2, {10, 11, 0}).valid());
    // Zero-size frames.
    REQUIRE_FALSE(compute_frame_upload(RawFormat::I420, 0, 0, {0, 0, 0}).valid());
}
