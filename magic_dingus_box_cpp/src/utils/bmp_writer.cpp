#include "utils/bmp_writer.h"

#include <limits>

namespace utils {

namespace {

constexpr std::size_t kFileHeaderSize = 14;
constexpr std::size_t kInfoHeaderSize = 40;

void put_u16(std::vector<std::uint8_t>& out, std::size_t at, std::uint16_t v) {
    out[at] = static_cast<std::uint8_t>(v & 0xFF);
    out[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

void put_u32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    }
}

}  // namespace

std::size_t bmp24_row_stride(int width) {
    if (width <= 0) return 0;
    return (static_cast<std::size_t>(width) * 3 + 3) & ~static_cast<std::size_t>(3);
}

std::vector<std::uint8_t> encode_bmp24_from_rgba(int width, int height,
                                                 const std::uint8_t* rgba,
                                                 std::size_t rgba_size) {
    if (width <= 0 || height <= 0 || rgba == nullptr) return {};

    const std::size_t w = static_cast<std::size_t>(width);
    const std::size_t h = static_cast<std::size_t>(height);
    const std::size_t src_stride = w * 4;
    if (rgba_size / h < src_stride) return {};  // too small (overflow-safe)

    const std::size_t dst_stride = bmp24_row_stride(width);
    const std::size_t pixel_bytes = dst_stride * h;
    const std::size_t header = kFileHeaderSize + kInfoHeaderSize;
    if (pixel_bytes / h != dst_stride ||
        pixel_bytes > std::numeric_limits<std::uint32_t>::max() - header) {
        return {};
    }

    std::vector<std::uint8_t> out(header + pixel_bytes, 0);

    // BITMAPFILEHEADER
    out[0] = 'B';
    out[1] = 'M';
    put_u32(out, 2, static_cast<std::uint32_t>(out.size()));
    // 6..9 reserved = 0
    put_u32(out, 10, static_cast<std::uint32_t>(header));  // pixel data offset

    // BITMAPINFOHEADER
    put_u32(out, 14, static_cast<std::uint32_t>(kInfoHeaderSize));
    put_u32(out, 18, static_cast<std::uint32_t>(width));
    put_u32(out, 22, static_cast<std::uint32_t>(height));  // >0: bottom-up
    put_u16(out, 26, 1);                                   // planes
    put_u16(out, 28, 24);                                  // bits per pixel
    put_u32(out, 30, 0);                                   // BI_RGB
    put_u32(out, 34, static_cast<std::uint32_t>(pixel_bytes));
    put_u32(out, 38, 2835);  // 72 DPI in pixels/metre — viewers ignore it
    put_u32(out, 42, 2835);
    // 46..53: colours used / important = 0

    for (std::size_t y = 0; y < h; ++y) {
        const std::uint8_t* src = rgba + y * src_stride;
        std::uint8_t* dst = out.data() + header + y * dst_stride;
        for (std::size_t x = 0; x < w; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 2];  // B
            dst[x * 3 + 1] = src[x * 4 + 1];  // G
            dst[x * 3 + 2] = src[x * 4 + 0];  // R
        }
        // Row padding bytes are already zero.
    }
    return out;
}

}  // namespace utils
