// Unit tests for the EXPERIMENTAL zero-copy video path's pure half
// (video/zero_copy_policy.h): the on/off decision, the EGL dmabuf
// attribute list, the EGLImage cache identity, the fallback counter and
// the sample-lifetime ring. The GPU half (video/dmabuf_importer.cpp) can
// only be validated on a real Pi 4B — see docs/ZERO_COPY_VIDEO.md.
//
// Pure logic, no GL/EGL/GStreamer — runs on the dev machine.

#include <catch2/catch_test_macros.hpp>

#include "video/zero_copy_policy.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace video::zero_copy;

namespace {

ExtensionAvailability all_extensions() {
    ExtensionAvailability e;
    e.egl_image_base = true;
    e.egl_dmabuf_import = true;
    e.egl_dmabuf_modifiers = true;
    e.gl_image_external = true;
    e.gl_image_external_essl3 = true;
    return e;
}

// Value following `key` in a kEglNone-terminated attribute list, or -1.
std::int64_t attr(const std::vector<std::int32_t>& a, std::int32_t key) {
    for (std::size_t i = 0; i + 1 < a.size(); i += 2) {
        if (a[i] == kEglNone) break;
        if (a[i] == key) return a[i + 1];
    }
    return -1;
}

bool has_key(const std::vector<std::int32_t>& a, std::int32_t key) {
    for (std::size_t i = 0; i + 1 < a.size(); i += 2) {
        if (a[i] == kEglNone) break;
        if (a[i] == key) return true;
    }
    return false;
}

// 1920x1080 NV12 as the Pi 4 decoder lays it out: one dmabuf, Y at 0,
// UV after 1088 padded rows.
FrameDesc nv12_1080p() {
    FrameDesc f;
    f.format = PixelFormat::NV12;
    f.width = 1920;
    f.height = 1080;
    f.n_planes = 2;
    f.planes[0] = {7, 0, 1920};
    f.planes[1] = {7, 1920u * 1088u, 1920};
    return f;
}

FrameDesc i420_854x480() {
    FrameDesc f;
    f.format = PixelFormat::I420;
    f.width = 854;
    f.height = 480;
    f.n_planes = 3;
    f.planes[0] = {9, 0, 896};
    f.planes[1] = {9, 896u * 480u, 448};
    f.planes[2] = {9, 896u * 480u + 448u * 240u, 448};
    return f;
}

}  // namespace

// ---------------------------------------------------------------------
// env flag + extension parsing
// ---------------------------------------------------------------------

TEST_CASE("env flag: only exactly \"1\" enables the experiment") {
    REQUIRE(env_flag_enabled("1"));
    REQUIRE_FALSE(env_flag_enabled(nullptr));
    REQUIRE_FALSE(env_flag_enabled(""));
    REQUIRE_FALSE(env_flag_enabled("0"));
    REQUIRE_FALSE(env_flag_enabled("true"));
    REQUIRE_FALSE(env_flag_enabled("yes"));
    REQUIRE_FALSE(env_flag_enabled("1 "));
    REQUIRE_FALSE(env_flag_enabled("10"));
}

TEST_CASE("has_extension matches whole tokens only") {
    const char* gl = "GL_EXT_foo GL_OES_EGL_image_external_essl3 GL_EXT_bar";
    REQUIRE(has_extension(gl, "GL_OES_EGL_image_external_essl3"));
    // The ESSL1 extension is a PREFIX of the essl3 one — must not match.
    REQUIRE_FALSE(has_extension(gl, "GL_OES_EGL_image_external"));
    REQUIRE(has_extension(gl, "GL_EXT_foo"));
    REQUIRE(has_extension(gl, "GL_EXT_bar"));
    REQUIRE_FALSE(has_extension(gl, "GL_EXT_fo"));
    REQUIRE_FALSE(has_extension(nullptr, "GL_EXT_foo"));
    REQUIRE_FALSE(has_extension(gl, ""));
    REQUIRE(has_extension("A B", "B"));
    REQUIRE(has_extension("B", "B"));
}

TEST_CASE("parse_extensions reads a realistic Mesa v3d extension set") {
    const char* egl =
        "EGL_EXT_buffer_age EGL_EXT_image_dma_buf_import "
        "EGL_EXT_image_dma_buf_import_modifiers EGL_KHR_image_base "
        "EGL_KHR_gl_texture_2D_image";
    const char* gl = "GL_OES_EGL_image GL_OES_EGL_image_external "
                     "GL_OES_EGL_image_external_essl3 GL_EXT_texture_rg";
    const ExtensionAvailability e = parse_extensions(egl, gl);
    REQUIRE(e.egl_image_base);
    REQUIRE(e.egl_dmabuf_import);
    REQUIRE(e.egl_dmabuf_modifiers);
    REQUIRE(e.gl_image_external);
    REQUIRE(e.gl_image_external_essl3);

    const ExtensionAvailability none = parse_extensions(nullptr, nullptr);
    REQUIRE_FALSE(none.egl_dmabuf_import);
    REQUIRE_FALSE(none.gl_image_external);
}

// ---------------------------------------------------------------------
// decision: flag x profile x extensions
// ---------------------------------------------------------------------

TEST_CASE("decision: flag OFF is copy, whatever the board and extensions") {
    const auto ext = all_extensions();
    for (bool board : {true, false}) {
        const Decision d = decide(false, board, &ext);
        REQUIRE_FALSE(d.enabled);
        REQUIRE(d.reason == "MDB_VIDEO_ZERO_COPY not set");
    }
    // The default path passes no extensions at all (it must not probe).
    REQUIRE_FALSE(decide(false, true, nullptr).enabled);
}

TEST_CASE("decision: flag ON but board not a candidate is copy") {
    const auto ext = all_extensions();
    const Decision d = decide(true, false, &ext);
    REQUIRE_FALSE(d.enabled);
    REQUIRE(d.reason == "board profile does not allow dmabuf import");
}

TEST_CASE("decision: flag ON + candidate but extensions unprobed is copy") {
    const Decision d = decide(true, true, nullptr);
    REQUIRE_FALSE(d.enabled);
    REQUIRE(d.reason.find("probed") != std::string::npos);
}

TEST_CASE("decision: every required extension is load-bearing") {
    {
        auto e = all_extensions();
        e.egl_dmabuf_import = false;
        const Decision d = decide(true, true, &e);
        REQUIRE_FALSE(d.enabled);
        REQUIRE(d.reason == "missing EGL_EXT_image_dma_buf_import");
    }
    {
        auto e = all_extensions();
        e.egl_image_base = false;
        const Decision d = decide(true, true, &e);
        REQUIRE_FALSE(d.enabled);
        REQUIRE(d.reason == "missing EGL_KHR_image_base");
    }
    {
        auto e = all_extensions();
        e.gl_image_external = false;
        e.gl_image_external_essl3 = false;
        const Decision d = decide(true, true, &e);
        REQUIRE_FALSE(d.enabled);
        REQUIRE(d.reason == "missing GL_OES_EGL_image_external");
    }
}

TEST_CASE("decision: all gates open enables zero-copy, ESSL3 preferred") {
    const auto e = all_extensions();
    const Decision d = decide(true, true, &e);
    REQUIRE(d.enabled);
    REQUIRE(d.dialect == ShaderDialect::Essl3);
    REQUIRE(d.use_modifiers);
}

TEST_CASE("decision: ESSL1 external sampler is the fallback dialect") {
    auto e = all_extensions();
    e.gl_image_external_essl3 = false;
    const Decision d = decide(true, true, &e);
    REQUIRE(d.enabled);
    REQUIRE(d.dialect == ShaderDialect::Essl1);
}

TEST_CASE("decision: modifiers extension is optional") {
    auto e = all_extensions();
    e.egl_dmabuf_modifiers = false;
    const Decision d = decide(true, true, &e);
    REQUIRE(d.enabled);
    REQUIRE_FALSE(d.use_modifiers);
}

TEST_CASE("dmabuf appsink caps: DMABuf only, linear NV12/I420 only") {
    const std::string caps = dmabuf_appsink_caps();
    REQUIRE(caps.find("memory:DMABuf") != std::string::npos);
    // Never system-memory caps here — those stay the existing fallback list.
    REQUIRE(caps.find("video/x-raw,") == std::string::npos);
    REQUIRE(caps.find("NV12") != std::string::npos);
    REQUIRE(caps.find("I420") != std::string::npos);
    REQUIRE(caps.find("YU12") != std::string::npos);
    // No explicit modifier suffix (":0x...") — linear only.
    REQUIRE(caps.find(":0x") == std::string::npos);
}

// ---------------------------------------------------------------------
// attribute list
// ---------------------------------------------------------------------

TEST_CASE("fourccs match drm_fourcc.h") {
    REQUIRE(kDrmFourccNV12 == 0x3231564eu);    // 'N''V''1''2'
    REQUIRE(kDrmFourccYUV420 == 0x32315559u);  // 'Y''U''1''2'
    REQUIRE(drm_fourcc_for(PixelFormat::Unsupported) == 0u);
    REQUIRE(plane_count(PixelFormat::NV12) == 2);
    REQUIRE(plane_count(PixelFormat::I420) == 3);
}

TEST_CASE("NV12 attribs: size, fourcc, two planes with offsets/pitches") {
    const AttribResult r = build_dmabuf_attribs(nv12_1080p(), false);
    REQUIRE(r.ok);
    const auto& a = r.attribs;
    REQUIRE(a.back() == kEglNone);
    REQUIRE(a.size() % 2 == 1);  // key/value pairs + terminator
    REQUIRE(attr(a, kEglWidth) == 1920);
    REQUIRE(attr(a, kEglHeight) == 1080);
    REQUIRE(attr(a, kEglLinuxDrmFourcc) == static_cast<std::int32_t>(kDrmFourccNV12));
    REQUIRE(attr(a, kEglPlaneFd[0]) == 7);
    REQUIRE(attr(a, kEglPlaneOffset[0]) == 0);
    REQUIRE(attr(a, kEglPlanePitch[0]) == 1920);
    REQUIRE(attr(a, kEglPlaneFd[1]) == 7);
    REQUIRE(attr(a, kEglPlaneOffset[1]) == 1920 * 1088);
    REQUIRE(attr(a, kEglPlanePitch[1]) == 1920);
    REQUIRE_FALSE(has_key(a, kEglPlaneFd[2]));
    // Untagged 1080p -> BT.709, limited range.
    REQUIRE(attr(a, kEglYuvColorSpaceHint) == kEglItuRec709);
    REQUIRE(attr(a, kEglSampleRangeHint) == kEglYuvNarrowRange);
    // No modifier stated -> none emitted.
    REQUIRE_FALSE(has_key(a, kEglPlaneModifierLo[0]));
}

TEST_CASE("I420 attribs: three planes, possibly separate dmabufs") {
    FrameDesc f = i420_854x480();
    f.planes[1].fd = 10;  // decoder gave each plane its own memory
    f.planes[2].fd = 11;
    const AttribResult r = build_dmabuf_attribs(f, false);
    REQUIRE(r.ok);
    REQUIRE(attr(r.attribs, kEglLinuxDrmFourcc) ==
            static_cast<std::int32_t>(kDrmFourccYUV420));
    REQUIRE(attr(r.attribs, kEglPlaneFd[0]) == 9);
    REQUIRE(attr(r.attribs, kEglPlaneFd[1]) == 10);
    REQUIRE(attr(r.attribs, kEglPlaneFd[2]) == 11);
    REQUIRE(attr(r.attribs, kEglPlanePitch[1]) == 448);
    REQUIRE(attr(r.attribs, kEglPlaneOffset[2]) == 896 * 480 + 448 * 240);
    // Untagged SD -> BT.601.
    REQUIRE(attr(r.attribs, kEglYuvColorSpaceHint) == kEglItuRec601);
}

TEST_CASE("colour hints follow the stream's colorimetry when tagged") {
    FrameDesc f = i420_854x480();
    f.matrix = ColorMatrix::Bt709;  // SD tagged 709 must stay 709
    f.range = ColorRange::Full;
    const AttribResult r = build_dmabuf_attribs(f, false);
    REQUIRE(r.ok);
    REQUIRE(attr(r.attribs, kEglYuvColorSpaceHint) == kEglItuRec709);
    REQUIRE(attr(r.attribs, kEglSampleRangeHint) == kEglYuvFullRange);

    REQUIRE(egl_color_space_hint(ColorMatrix::Bt601, 1080) == kEglItuRec601);
    REQUIRE(egl_color_space_hint(ColorMatrix::Bt2020, 2160) == kEglItuRec2020);
    REQUIRE(egl_color_space_hint(ColorMatrix::Unknown, 720) == kEglItuRec709);
    REQUIRE(egl_color_space_hint(ColorMatrix::Other, 576) == kEglItuRec601);
    REQUIRE(egl_sample_range_hint(ColorRange::Unknown) == kEglYuvNarrowRange);
    REQUIRE(egl_sample_range_hint(ColorRange::Limited) == kEglYuvNarrowRange);
}

TEST_CASE("modifier: emitted per plane only with the modifiers extension") {
    FrameDesc f = nv12_1080p();
    f.modifier = kDrmFormatModLinear;
    {
        const AttribResult r = build_dmabuf_attribs(f, true);
        REQUIRE(r.ok);
        REQUIRE(attr(r.attribs, kEglPlaneModifierLo[0]) == 0);
        REQUIRE(attr(r.attribs, kEglPlaneModifierHi[0]) == 0);
        REQUIRE(attr(r.attribs, kEglPlaneModifierLo[1]) == 0);
        REQUIRE_FALSE(has_key(r.attribs, kEglPlaneModifierLo[2]));
    }
    {
        // Linear without the extension: implicit default, still importable.
        const AttribResult r = build_dmabuf_attribs(f, false);
        REQUIRE(r.ok);
        REQUIRE_FALSE(has_key(r.attribs, kEglPlaneModifierLo[0]));
    }
    // A tiled modifier (Broadcom SAND128) is split into lo/hi halves...
    f.modifier = 0x0700000000000004ULL;
    {
        const AttribResult r = build_dmabuf_attribs(f, true);
        REQUIRE(r.ok);
        REQUIRE(attr(r.attribs, kEglPlaneModifierLo[0]) == 4);
        REQUIRE(attr(r.attribs, kEglPlaneModifierHi[0]) == 0x07000000);
    }
    // ...and refused without the extension (the driver would guess linear).
    {
        const AttribResult r = build_dmabuf_attribs(f, false);
        REQUIRE_FALSE(r.ok);
        REQUIRE(r.error.find("modifier") != std::string::npos);
    }
}

TEST_CASE("attribs refuse malformed frames instead of importing garbage") {
    {
        FrameDesc f = nv12_1080p();
        f.format = PixelFormat::Unsupported;
        REQUIRE_FALSE(build_dmabuf_attribs(f, false).ok);
    }
    {
        FrameDesc f = nv12_1080p();
        f.n_planes = 3;  // wrong for NV12
        const AttribResult r = build_dmabuf_attribs(f, false);
        REQUIRE_FALSE(r.ok);
        REQUIRE(r.error.find("needs 2 planes") != std::string::npos);
    }
    {
        FrameDesc f = nv12_1080p();
        f.planes[1].fd = -1;
        const AttribResult r = build_dmabuf_attribs(f, false);
        REQUIRE_FALSE(r.ok);
        REQUIRE(r.attribs.empty());
    }
    {
        FrameDesc f = nv12_1080p();
        f.planes[0].stride = 1900;  // narrower than a row
        REQUIRE_FALSE(build_dmabuf_attribs(f, false).ok);
    }
    {
        // I420 chroma rows are ceil(w/2) bytes: 427 for 854 wide.
        FrameDesc f = i420_854x480();
        f.planes[1].stride = 426;
        REQUIRE_FALSE(build_dmabuf_attribs(f, false).ok);
        f.planes[1].stride = 427;
        REQUIRE(build_dmabuf_attribs(f, false).ok);
    }
    {
        FrameDesc f = nv12_1080p();
        f.planes[1].offset = 0x100000000ULL;  // > EGLint
        REQUIRE_FALSE(build_dmabuf_attribs(f, false).ok);
    }
    {
        FrameDesc f = nv12_1080p();
        f.width = 0;
        REQUIRE_FALSE(build_dmabuf_attribs(f, false).ok);
    }
}

// ---------------------------------------------------------------------
// cache identity
// ---------------------------------------------------------------------

TEST_CASE("image key: inode identity, not fd number") {
    const FrameDesc a = nv12_1080p();
    FrameDesc b = a;
    b.planes[0].fd = 42;  // same buffer, dup()ed to another number
    b.planes[1].fd = 42;
    const std::array<std::uint64_t, 3> ino{1234, 1234, 0};
    REQUIRE(make_image_key(a, ino) == make_image_key(b, ino));
    // Same fd number but a different underlying buffer (fd reused).
    const std::array<std::uint64_t, 3> other{5678, 5678, 0};
    REQUIRE(make_image_key(a, ino) != make_image_key(a, other));
}

TEST_CASE("image key: layout and colour changes are different images") {
    const std::array<std::uint64_t, 3> ino{1, 1, 0};
    const FrameDesc a = nv12_1080p();
    FrameDesc b = a;
    b.planes[1].offset += 64;
    REQUIRE(make_image_key(a, ino) != make_image_key(b, ino));
    FrameDesc c = a;
    c.range = ColorRange::Full;
    REQUIRE(make_image_key(a, ino) != make_image_key(c, ino));
}

TEST_CASE("image key: unknown inode is never cached") {
    const FrameDesc a = nv12_1080p();
    REQUIRE(key_cacheable(make_image_key(a, {1, 1, 0})));
    REQUIRE_FALSE(key_cacheable(make_image_key(a, {1, 0, 0})));
    REQUIRE_FALSE(key_cacheable(ImageKey{}));
}

// ---------------------------------------------------------------------
// fallback counter
// ---------------------------------------------------------------------

TEST_CASE("fallback: trips permanently after N consecutive failures, once") {
    FallbackCounter c(3);
    REQUIRE_FALSE(c.on_failure());
    REQUIRE_FALSE(c.on_failure());
    REQUIRE_FALSE(c.permanently_disabled());
    REQUIRE(c.on_failure());  // third in a row trips it
    REQUIRE(c.permanently_disabled());
    REQUIRE_FALSE(c.on_failure());  // never reports the trip twice
    REQUIRE(c.permanently_disabled());
    c.on_success();  // a late success cannot re-enable it
    REQUIRE(c.permanently_disabled());
    REQUIRE(c.total_failures() == 4);
}

TEST_CASE("fallback: a success resets the streak") {
    FallbackCounter c(3);
    c.on_failure();
    c.on_failure();
    c.on_success();
    REQUIRE(c.consecutive_failures() == 0);
    REQUIRE_FALSE(c.on_failure());
    REQUIRE_FALSE(c.on_failure());
    REQUIRE_FALSE(c.permanently_disabled());
}

TEST_CASE("fallback: exactly one WARN for the whole session") {
    FallbackCounter c;
    REQUIRE(c.should_warn());
    REQUIRE_FALSE(c.should_warn());
    REQUIRE_FALSE(c.should_warn());
}

TEST_CASE("fallback: default threshold is about a second of video") {
    FallbackCounter c;
    REQUIRE(c.threshold() == FallbackCounter::kDefaultThreshold);
    REQUIRE(c.threshold() >= 24);
    REQUIRE(FallbackCounter(0).threshold() == 1);  // clamped, never 0
}

// ---------------------------------------------------------------------
// sample-lifetime ring
// ---------------------------------------------------------------------

TEST_CASE("hold ring: a frame is released only after the NEXT frame is shown") {
    HoldRing<int, 2> ring;
    REQUIRE(ring.push(1) == 0);  // nothing to release yet
    REQUIRE(ring.push(2) == 0);  // frame 1 may still be in flight
    REQUIRE(ring.push(3) == 1);  // frame 2 was presented -> 1 is free
    REQUIRE(ring.push(4) == 2);
    REQUIRE(ring.occupied() == 2);
}

TEST_CASE("hold ring: copy-path frames (empty pushes) drain it") {
    HoldRing<int, 2> ring;
    ring.push(1);
    ring.push(2);
    REQUIRE(ring.push(0) == 1);
    REQUIRE(ring.push(0) == 2);
    REQUIRE(ring.occupied() == 0);
}

TEST_CASE("hold ring: clear releases everything exactly once") {
    HoldRing<int, 2> ring;
    ring.push(5);
    ring.push(6);
    std::vector<int> released;
    ring.clear([&](int h) { released.push_back(h); });
    std::sort(released.begin(), released.end());
    REQUIRE(released == std::vector<int>{5, 6});
    REQUIRE(ring.occupied() == 0);
    ring.clear([&](int h) { released.push_back(h); });
    REQUIRE(released.size() == 2);
}
