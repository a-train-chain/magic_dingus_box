#pragma once

// Uncompressed 24-bit BMP encoder for the kiosk's debug screenshot hook
// (debug/screenshot_capture). Hand-written on purpose: BMP is a 54-byte
// header plus raw rows, and every image viewer and browser opens it, so it
// needs no new dependency on the box. Pure logic, unit-tested on the Mac
// (tests/utils/test_bmp_writer.cpp).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace utils {

// Bytes per BMP pixel row for a 24-bit image: width*3 rounded up to a
// multiple of 4 (the format's row alignment). 0 for width <= 0.
std::size_t bmp24_row_stride(int width);

// Encodes `rgba` as a BITMAPFILEHEADER + BITMAPINFOHEADER (BI_RGB, 24 bpp,
// positive height = bottom-up) BMP.
//
// `rgba` holds `height` rows of `width * 4` bytes (R, G, B, A), row 0 being
// the BOTTOM of the picture — glReadPixels' order, which is also BMP's
// native bottom-up order, so rows are copied straight through. Alpha is
// dropped; channels are reordered to BMP's B, G, R.
//
// Returns an empty vector when width/height are not positive, when
// `rgba_size` is smaller than width*height*4, or when the result would not
// fit the format's 32-bit size fields.
std::vector<std::uint8_t> encode_bmp24_from_rgba(int width, int height,
                                                 const std::uint8_t* rgba,
                                                 std::size_t rgba_size);

}  // namespace utils
