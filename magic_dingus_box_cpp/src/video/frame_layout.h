#pragma once

#include <array>

namespace video {

// Per-plane texture-upload parameters for a raw video frame.
//
// Raw video caps carry NO stride field. The old upload path read a
// "stride" field from the caps that never exists, so it always assumed
// stride == width. GStreamer's default layout pads every row to 4 bytes
// (GST_ROUND_UP_4), so any width that is not a multiple of 4 — 854x480
// being the common one, and every I420 chroma plane of such a video —
// was uploaded with the wrong row pitch and the wrong plane offsets: a
// diagonally sheared picture with smeared colour. The strides and plane
// offsets now come from GstVideoInfo / GstVideoFrame (which also honours
// a decoder's GstVideoMeta), and this pure helper turns them into what GL
// needs. GL_UNPACK_ROW_LENGTH is core in OpenGL ES 3.0, counted in texels.
enum class RawFormat { RGBA, I420, NV12 };

struct PlaneUpload {
    int tex_width = 0;    // texture width in texels
    int tex_height = 0;   // texture height in rows
    int row_length = 0;   // GL_UNPACK_ROW_LENGTH (texels per source row)
    bool valid = false;   // stride usable for this plane?
};

struct FrameUpload {
    int n_planes = 0;
    std::array<PlaneUpload, 3> planes{};
    // True when every plane is valid; upload nothing otherwise (a garbage
    // frame is worse than repeating the previous one).
    bool valid() const {
        if (n_planes <= 0) return false;
        for (int i = 0; i < n_planes; ++i) {
            if (!planes[static_cast<size_t>(i)].valid) return false;
        }
        return true;
    }
};

namespace detail {
inline PlaneUpload make_plane(int tex_w, int tex_h, int stride_bytes,
                              int bytes_per_texel) {
    PlaneUpload p;
    p.tex_width = tex_w;
    p.tex_height = tex_h;
    if (tex_w <= 0 || tex_h <= 0 || bytes_per_texel <= 0 ||
        stride_bytes < tex_w * bytes_per_texel ||
        stride_bytes % bytes_per_texel != 0) {
        return p;  // valid = false
    }
    p.row_length = stride_bytes / bytes_per_texel;
    p.valid = true;
    return p;
}
}  // namespace detail

// strides: bytes per row of each plane, as reported by GStreamer.
//   RGBA: plane 0, 4 bytes/texel, w x h
//   I420: Y w x h (1 B), U and V ceil(w/2) x ceil(h/2) (1 B)
//   NV12: Y w x h (1 B), interleaved UV ceil(w/2) x ceil(h/2) (2 B, GL_RG)
// Chroma textures are sized to the REAL chroma width, never the stride:
// sampling the padding columns would stretch colour to the right.
inline FrameUpload compute_frame_upload(RawFormat format, int width,
                                        int height,
                                        const std::array<int, 3>& strides) {
    FrameUpload f;
    const int cw = (width + 1) / 2;
    const int ch = (height + 1) / 2;
    switch (format) {
        case RawFormat::RGBA:
            f.n_planes = 1;
            f.planes[0] = detail::make_plane(width, height, strides[0], 4);
            break;
        case RawFormat::I420:
            f.n_planes = 3;
            f.planes[0] = detail::make_plane(width, height, strides[0], 1);
            f.planes[1] = detail::make_plane(cw, ch, strides[1], 1);
            f.planes[2] = detail::make_plane(cw, ch, strides[2], 1);
            break;
        case RawFormat::NV12:
            f.n_planes = 2;
            f.planes[0] = detail::make_plane(width, height, strides[0], 1);
            f.planes[1] = detail::make_plane(cw, ch, strides[1], 2);
            break;
    }
    return f;
}

}  // namespace video
