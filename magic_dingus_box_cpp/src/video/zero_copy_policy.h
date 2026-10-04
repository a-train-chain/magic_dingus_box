#pragma once

// EXPERIMENTAL zero-copy video path — the pure half.
//
// Today every decoded frame is mapped and uploaded with glTexSubImage2D:
// the CPU (and Mesa's detiler) touch ~75-95 MB/s at 1080p24-30 on the
// render thread. On a Pi 4B, v4l2h264dec can hand out DMABuf-backed
// buffers instead, which EGL can wrap as an EGLImage (EGL_EXT_image_dma_buf
// _import) and GL can sample directly (samplerExternalOES) — no per-frame
// copy at all. See docs/ZERO_COPY_VIDEO.md for how to enable and validate.
//
// This header holds everything about that path that can be decided
// WITHOUT a GPU, EGL, or GStreamer, so it is unit-tested on the Mac
// (tests/video/test_zero_copy_policy.cpp):
//   - the on/off decision (env flag x board profile x extensions),
//   - the EGL attribute list for one dmabuf frame,
//   - the per-frame / permanent fallback counter,
//   - the sample-lifetime ring.
// The EGL token values are spelled out here as numbers so the tests need
// no EGL headers; video/dmabuf_importer.cpp static_asserts every one of
// them against <EGL/eglext.h>.
//
// OFF BY DEFAULT. With MDB_VIDEO_ZERO_COPY unset the kiosk never probes an
// extension, never adds DMABuf caps to the appsink, and runs the copy path
// exactly as before.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace video {
namespace zero_copy {

// ---------------------------------------------------------------------
// Decision
// ---------------------------------------------------------------------

// MDB_VIDEO_ZERO_COPY must be exactly "1". Anything else (unset, "0",
// "true", "yes") is OFF — an experiment must be opted into unambiguously.
inline bool env_flag_enabled(const char* value) {
    return value != nullptr && std::strcmp(value, "1") == 0;
}

// Whole-token match in a space-separated EGL/GL extension string, so
// "GL_OES_EGL_image_external" is NOT found inside
// "GL_OES_EGL_image_external_essl3".
inline bool has_extension(const char* list, const char* name) {
    if (list == nullptr || name == nullptr || *name == '\0') return false;
    const std::size_t n = std::strlen(name);
    const char* p = list;
    while ((p = std::strstr(p, name)) != nullptr) {
        const bool start_ok = (p == list) || (p[-1] == ' ');
        const bool end_ok = (p[n] == '\0') || (p[n] == ' ');
        if (start_ok && end_ok) return true;
        p += n;
    }
    return false;
}

struct ExtensionAvailability {
    bool egl_image_base = false;          // EGL_KHR_image_base (eglCreateImageKHR)
    bool egl_dmabuf_import = false;       // EGL_EXT_image_dma_buf_import
    bool egl_dmabuf_modifiers = false;    // EGL_EXT_image_dma_buf_import_modifiers
    bool gl_image_external = false;       // GL_OES_EGL_image_external
    bool gl_image_external_essl3 = false; // GL_OES_EGL_image_external_essl3
};

inline ExtensionAvailability parse_extensions(const char* egl_extensions,
                                              const char* gl_extensions) {
    ExtensionAvailability e;
    e.egl_image_base = has_extension(egl_extensions, "EGL_KHR_image_base");
    e.egl_dmabuf_import = has_extension(egl_extensions, "EGL_EXT_image_dma_buf_import");
    e.egl_dmabuf_modifiers =
        has_extension(egl_extensions, "EGL_EXT_image_dma_buf_import_modifiers");
    e.gl_image_external = has_extension(gl_extensions, "GL_OES_EGL_image_external");
    e.gl_image_external_essl3 =
        has_extension(gl_extensions, "GL_OES_EGL_image_external_essl3");
    return e;
}

// Which samplerExternalOES dialect the fragment shader is written in.
enum class ShaderDialect {
    None,
    Essl3,  // #version 300 es + GL_OES_EGL_image_external_essl3 (preferred:
            // matches the copy path's #version 300 es shaders)
    Essl1   // #version 100 + GL_OES_EGL_image_external
};

struct Decision {
    bool enabled = false;
    ShaderDialect dialect = ShaderDialect::None;
    bool use_modifiers = false;  // EGL_EXT_image_dma_buf_import_modifiers present
    std::string reason;          // the "(...)" of "Video upload: copy (...)"
};

// env_flag / board_candidate are checked FIRST and short-circuit: when
// either is false, `ext` is never read — the caller is expected to pass
// nullptr there and must not have probed anything (the default path stays
// byte-for-byte the old one). `ext` == nullptr with both gates open is
// treated as "could not probe" and stays on the copy path.
inline Decision decide(bool env_flag, bool board_candidate,
                       const ExtensionAvailability* ext) {
    Decision d;
    if (!env_flag) {
        d.reason = "MDB_VIDEO_ZERO_COPY not set";
        return d;
    }
    if (!board_candidate) {
        d.reason = "board profile does not allow dmabuf import";
        return d;
    }
    if (ext == nullptr) {
        d.reason = "EGL/GL extensions could not be probed";
        return d;
    }
    if (!ext->egl_image_base) {
        d.reason = "missing EGL_KHR_image_base";
        return d;
    }
    if (!ext->egl_dmabuf_import) {
        d.reason = "missing EGL_EXT_image_dma_buf_import";
        return d;
    }
    if (ext->gl_image_external_essl3) {
        d.dialect = ShaderDialect::Essl3;
    } else if (ext->gl_image_external) {
        d.dialect = ShaderDialect::Essl1;
    } else {
        d.reason = "missing GL_OES_EGL_image_external";
        return d;
    }
    d.enabled = true;
    d.use_modifiers = ext->egl_dmabuf_modifiers;
    return d;
}

// Appsink caps PREPENDED to the existing system-memory caps when (and only
// when) the decision is enabled. Caps order is preference order, so DMABuf
// wins whenever the decoder can produce it and the old
// I420/NV12/RGBA system-memory caps remain the automatic fallback (e.g.
// avdec_h265 software HEVC, which only produces system memory).
//   1. legacy DMABuf caps (format=NV12/I420 with the memory:DMABuf feature)
//   2. GStreamer >= 1.24 DMA_DRM caps, LINEAR NV12 / YU12 only (no
//      modifier suffix = linear), so every frame stays importable without
//      the modifiers extension AND mappable by the copy fallback.
inline const char* dmabuf_appsink_caps() {
    return "video/x-raw(memory:DMABuf), format=(string){ NV12, I420 }; "
           "video/x-raw(memory:DMABuf), format=(string)DMA_DRM, "
           "drm-format=(string){ NV12, YU12 }";
}

// ---------------------------------------------------------------------
// EGL dmabuf attribute list
// ---------------------------------------------------------------------

// EGL tokens (values from the Khronos registry; static_asserted against
// <EGL/eglext.h> in dmabuf_importer.cpp).
constexpr std::int32_t kEglNone = 0x3038;
constexpr std::int32_t kEglWidth = 0x3057;
constexpr std::int32_t kEglHeight = 0x3056;
constexpr std::int32_t kEglLinuxDrmFourcc = 0x3271;
constexpr std::int32_t kEglPlaneFd[3] = {0x3272, 0x3275, 0x3278};
constexpr std::int32_t kEglPlaneOffset[3] = {0x3273, 0x3276, 0x3279};
constexpr std::int32_t kEglPlanePitch[3] = {0x3274, 0x3277, 0x327A};
constexpr std::int32_t kEglPlaneModifierLo[3] = {0x3443, 0x3445, 0x3447};
constexpr std::int32_t kEglPlaneModifierHi[3] = {0x3444, 0x3446, 0x3448};
constexpr std::int32_t kEglYuvColorSpaceHint = 0x327B;
constexpr std::int32_t kEglSampleRangeHint = 0x327C;
constexpr std::int32_t kEglItuRec601 = 0x327F;
constexpr std::int32_t kEglItuRec709 = 0x3280;
constexpr std::int32_t kEglItuRec2020 = 0x3281;
constexpr std::int32_t kEglYuvFullRange = 0x3282;
constexpr std::int32_t kEglYuvNarrowRange = 0x3283;

// DRM fourccs (drm_fourcc.h): fourcc_code(a,b,c,d) = a | b<<8 | c<<16 | d<<24.
constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
}
constexpr std::uint32_t kDrmFourccNV12 = fourcc('N', 'V', '1', '2');
constexpr std::uint32_t kDrmFourccYUV420 = fourcc('Y', 'U', '1', '2');  // == I420
constexpr std::uint64_t kDrmFormatModLinear = 0;
constexpr std::uint64_t kDrmFormatModInvalid = 0x00ffffffffffffffULL;

enum class PixelFormat { NV12, I420, Unsupported };

inline std::uint32_t drm_fourcc_for(PixelFormat f) {
    switch (f) {
        case PixelFormat::NV12: return kDrmFourccNV12;
        case PixelFormat::I420: return kDrmFourccYUV420;
        case PixelFormat::Unsupported: break;
    }
    return 0;
}

inline int plane_count(PixelFormat f) {
    switch (f) {
        case PixelFormat::NV12: return 2;
        case PixelFormat::I420: return 3;
        case PixelFormat::Unsupported: break;
    }
    return 0;
}

inline const char* format_name(PixelFormat f) {
    switch (f) {
        case PixelFormat::NV12: return "NV12";
        case PixelFormat::I420: return "I420";
        case PixelFormat::Unsupported: break;
    }
    return "unsupported";
}

// Mirrors of GstVideoColorMatrix / GstVideoColorRange (kept GStreamer-free).
enum class ColorMatrix { Unknown, Bt601, Bt709, Bt2020, Other };
enum class ColorRange { Unknown, Full, Limited };

// EGL_YUV_COLOR_SPACE_HINT_EXT value. Unknown follows GStreamer's own
// default for untagged video: HD (>= 720 lines) is BT.709, SD is BT.601.
inline std::int32_t egl_color_space_hint(ColorMatrix m, int height) {
    switch (m) {
        case ColorMatrix::Bt709: return kEglItuRec709;
        case ColorMatrix::Bt601: return kEglItuRec601;
        case ColorMatrix::Bt2020: return kEglItuRec2020;
        case ColorMatrix::Unknown:
        case ColorMatrix::Other: break;
    }
    return height >= 720 ? kEglItuRec709 : kEglItuRec601;
}

// EGL_SAMPLE_RANGE_HINT_EXT value. Untagged video is limited ("TV") range.
inline std::int32_t egl_sample_range_hint(ColorRange r) {
    return r == ColorRange::Full ? kEglYuvFullRange : kEglYuvNarrowRange;
}

// One plane as GStreamer describes it, already resolved to its dmabuf:
// `fd` is the dmabuf holding the plane, `offset` is the byte offset of
// the plane INSIDE that dmabuf (GstMemory offset + skip within the
// memory), `stride` is the row pitch in bytes (GstVideoMeta when the
// decoder attached one, else GstVideoInfo).
struct PlaneSource {
    int fd = -1;
    std::uint64_t offset = 0;
    std::int64_t stride = 0;
};

// A GstVideoInfo-like description of one frame — deliberately a plain
// struct so the attribute builder is testable without GStreamer.
struct FrameDesc {
    PixelFormat format = PixelFormat::Unsupported;
    int width = 0;
    int height = 0;
    int n_planes = 0;
    std::array<PlaneSource, 3> planes{};
    // kDrmFormatModInvalid = "not stated" (legacy DMABuf caps). DMA_DRM
    // caps state it (linear only, see dmabuf_appsink_caps()).
    std::uint64_t modifier = kDrmFormatModInvalid;
    ColorMatrix matrix = ColorMatrix::Unknown;
    ColorRange range = ColorRange::Unknown;
};

struct AttribResult {
    bool ok = false;
    std::string error;                  // why not, for the one WARN log
    std::vector<std::int32_t> attribs;  // EGLint list, kEglNone-terminated
};

// Minimum bytes per row a plane needs (the stride must cover it).
inline std::int64_t min_row_bytes(PixelFormat f, int plane, int width) {
    const std::int64_t cw = (static_cast<std::int64_t>(width) + 1) / 2;
    switch (f) {
        case PixelFormat::NV12: return plane == 0 ? width : cw * 2;
        case PixelFormat::I420: return plane == 0 ? width : cw;
        case PixelFormat::Unsupported: break;
    }
    return 0;
}

inline AttribResult build_dmabuf_attribs(const FrameDesc& f, bool modifiers_ext) {
    AttribResult r;
    if (f.format == PixelFormat::Unsupported) {
        r.error = "unsupported pixel format";
        return r;
    }
    if (f.width <= 0 || f.height <= 0) {
        r.error = "invalid frame size";
        return r;
    }
    const int want = plane_count(f.format);
    if (f.n_planes != want) {
        r.error = std::string(format_name(f.format)) + " needs " +
                  std::to_string(want) + " planes, got " + std::to_string(f.n_planes);
        return r;
    }

    // Modifier policy. Not stated -> say nothing (driver assumes its
    // implicit layout; for v4l2 capture buffers that is linear). LINEAR
    // without the modifiers extension -> also say nothing (linear IS the
    // implicit default). Anything else needs the extension.
    bool emit_modifier = false;
    if (f.modifier != kDrmFormatModInvalid) {
        if (modifiers_ext) {
            emit_modifier = true;
        } else if (f.modifier != kDrmFormatModLinear) {
            r.error = "non-linear modifier without EGL_EXT_image_dma_buf_import_modifiers";
            return r;
        }
    }

    constexpr std::int64_t kMax = std::numeric_limits<std::int32_t>::max();
    std::vector<std::int32_t>& a = r.attribs;
    a.reserve(48);
    a.push_back(kEglWidth);
    a.push_back(f.width);
    a.push_back(kEglHeight);
    a.push_back(f.height);
    a.push_back(kEglLinuxDrmFourcc);
    a.push_back(static_cast<std::int32_t>(drm_fourcc_for(f.format)));
    for (int i = 0; i < want; ++i) {
        const PlaneSource& p = f.planes[static_cast<std::size_t>(i)];
        if (p.fd < 0) {
            r.error = "plane " + std::to_string(i) + " has no dmabuf fd";
            r.attribs.clear();
            return r;
        }
        if (p.stride < min_row_bytes(f.format, i, f.width) || p.stride > kMax) {
            r.error = "plane " + std::to_string(i) + " stride " +
                      std::to_string(p.stride) + " unusable";
            r.attribs.clear();
            return r;
        }
        if (p.offset > static_cast<std::uint64_t>(kMax)) {
            r.error = "plane " + std::to_string(i) + " offset out of EGLint range";
            r.attribs.clear();
            return r;
        }
        a.push_back(kEglPlaneFd[i]);
        a.push_back(p.fd);
        a.push_back(kEglPlaneOffset[i]);
        a.push_back(static_cast<std::int32_t>(p.offset));
        a.push_back(kEglPlanePitch[i]);
        a.push_back(static_cast<std::int32_t>(p.stride));
        if (emit_modifier) {
            a.push_back(kEglPlaneModifierLo[i]);
            a.push_back(static_cast<std::int32_t>(
                static_cast<std::uint32_t>(f.modifier & 0xffffffffULL)));
            a.push_back(kEglPlaneModifierHi[i]);
            a.push_back(static_cast<std::int32_t>(
                static_cast<std::uint32_t>(f.modifier >> 32)));
        }
    }
    a.push_back(kEglYuvColorSpaceHint);
    a.push_back(egl_color_space_hint(f.matrix, f.height));
    a.push_back(kEglSampleRangeHint);
    a.push_back(egl_sample_range_hint(f.range));
    a.push_back(kEglNone);
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------
// EGLImage cache identity
// ---------------------------------------------------------------------

// What makes two frames share one EGLImage: the same underlying dmabufs
// (fd NUMBERS are not identity — a closed fd's number is reused by the
// next open, and a decoder may dup() the same buffer to a new number — so
// the cache keys on the dmabuf inode from fstat), at the same layout.
struct ImageKey {
    std::uint32_t fourcc = 0;
    int width = 0;
    int height = 0;
    int n_planes = 0;
    std::array<std::uint64_t, 3> inode{};
    std::array<std::uint64_t, 3> offset{};
    std::array<std::int64_t, 3> stride{};
    std::uint64_t modifier = kDrmFormatModInvalid;
    std::int32_t color_space = 0;
    std::int32_t range = 0;

    bool operator==(const ImageKey& o) const {
        return fourcc == o.fourcc && width == o.width && height == o.height &&
               n_planes == o.n_planes && inode == o.inode && offset == o.offset &&
               stride == o.stride && modifier == o.modifier &&
               color_space == o.color_space && range == o.range;
    }
    bool operator!=(const ImageKey& o) const { return !(*this == o); }
};

// inodes[i] = st_ino of planes[i].fd (0 = unknown -> not cacheable).
inline ImageKey make_image_key(const FrameDesc& f,
                               const std::array<std::uint64_t, 3>& inodes) {
    ImageKey k;
    k.fourcc = drm_fourcc_for(f.format);
    k.width = f.width;
    k.height = f.height;
    k.n_planes = f.n_planes;
    for (int i = 0; i < f.n_planes && i < 3; ++i) {
        const auto s = static_cast<std::size_t>(i);
        k.inode[s] = inodes[s];
        k.offset[s] = f.planes[s].offset;
        k.stride[s] = f.planes[s].stride;
    }
    k.modifier = f.modifier;
    k.color_space = egl_color_space_hint(f.matrix, f.height);
    k.range = egl_sample_range_hint(f.range);
    return k;
}

inline bool key_cacheable(const ImageKey& k) {
    if (k.n_planes <= 0) return false;
    for (int i = 0; i < k.n_planes && i < 3; ++i) {
        if (k.inode[static_cast<std::size_t>(i)] == 0) return false;
    }
    return true;
}

// ---------------------------------------------------------------------
// Fallback counter
// ---------------------------------------------------------------------

// Import failures fall back to the copy path for THAT frame; after
// `threshold` CONSECUTIVE failures the zero-copy path is disabled for the
// rest of the process. A frame that simply is not a dmabuf (software
// decode, or the v4l2 pool copying under pressure) is NOT a failure — it
// is not counted and does not reset the streak; it is just copied.
class FallbackCounter {
public:
    static constexpr int kDefaultThreshold = 30;  // ~1 s of 24-30 fps video

    explicit FallbackCounter(int threshold = kDefaultThreshold)
        : threshold_(threshold < 1 ? 1 : threshold) {}

    void on_success() { consecutive_ = 0; }

    // Returns true exactly once: on the failure that trips the permanent
    // fallback.
    bool on_failure() {
        ++total_failures_;
        if (disabled_) return false;
        if (++consecutive_ >= threshold_) {
            disabled_ = true;
            return true;
        }
        return false;
    }

    // True for the FIRST failure ever — the one WARN log (with reason).
    bool should_warn() {
        if (warned_) return false;
        warned_ = true;
        return true;
    }

    bool permanently_disabled() const { return disabled_; }
    int consecutive_failures() const { return consecutive_; }
    long long total_failures() const { return total_failures_; }
    int threshold() const { return threshold_; }

private:
    int threshold_;
    int consecutive_ = 0;
    long long total_failures_ = 0;
    bool disabled_ = false;
    bool warned_ = false;
};

// ---------------------------------------------------------------------
// Sample-lifetime ring
// ---------------------------------------------------------------------

// Holds the last `Depth` imported frames' handles (GstSample*) so the
// dmabuf behind a texture cannot be recycled by the decoder while the GPU
// may still read it. With Depth = 2, pushing frame N+1 releases N-1: N-1
// was last drawn at least one present ago, i.e. the frame that used it has
// been presented ("one-frame lag"). push() returns the evicted handle for
// the caller to release; an empty slot is `T{}`.
template <typename T, std::size_t Depth = 2>
class HoldRing {
public:
    static_assert(Depth >= 1, "ring must hold at least the current frame");

    T push(T handle) {
        T evicted = slots_[next_];
        slots_[next_] = handle;
        next_ = (next_ + 1) % Depth;
        return evicted;
    }

    // Empties the ring; calls release(h) for every non-empty slot.
    template <typename F>
    void clear(F&& release) {
        for (auto& s : slots_) {
            if (s != T{}) release(s);
            s = T{};
        }
        next_ = 0;
    }

    std::size_t occupied() const {
        std::size_t n = 0;
        for (const auto& s : slots_) {
            if (s != T{}) ++n;
        }
        return n;
    }

private:
    std::array<T, Depth> slots_{};
    std::size_t next_ = 0;
};

}  // namespace zero_copy
}  // namespace video
