#include "dmabuf_importer.h"

#include "../utils/logger.h"

#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace video {

namespace zc = zero_copy;

// The pure header spells EGL tokens as numbers so its tests need no EGL
// headers; pin every one of them to the real definitions here.
static_assert(zc::kEglNone == EGL_NONE, "EGL token drift");
static_assert(zc::kEglWidth == EGL_WIDTH, "EGL token drift");
static_assert(zc::kEglHeight == EGL_HEIGHT, "EGL token drift");
static_assert(zc::kEglLinuxDrmFourcc == EGL_LINUX_DRM_FOURCC_EXT, "EGL token drift");
static_assert(zc::kEglPlaneFd[0] == EGL_DMA_BUF_PLANE0_FD_EXT, "EGL token drift");
static_assert(zc::kEglPlaneFd[1] == EGL_DMA_BUF_PLANE1_FD_EXT, "EGL token drift");
static_assert(zc::kEglPlaneFd[2] == EGL_DMA_BUF_PLANE2_FD_EXT, "EGL token drift");
static_assert(zc::kEglPlaneOffset[0] == EGL_DMA_BUF_PLANE0_OFFSET_EXT, "EGL token drift");
static_assert(zc::kEglPlaneOffset[1] == EGL_DMA_BUF_PLANE1_OFFSET_EXT, "EGL token drift");
static_assert(zc::kEglPlaneOffset[2] == EGL_DMA_BUF_PLANE2_OFFSET_EXT, "EGL token drift");
static_assert(zc::kEglPlanePitch[0] == EGL_DMA_BUF_PLANE0_PITCH_EXT, "EGL token drift");
static_assert(zc::kEglPlanePitch[1] == EGL_DMA_BUF_PLANE1_PITCH_EXT, "EGL token drift");
static_assert(zc::kEglPlanePitch[2] == EGL_DMA_BUF_PLANE2_PITCH_EXT, "EGL token drift");
static_assert(zc::kEglPlaneModifierLo[0] == EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, "EGL token drift");
static_assert(zc::kEglPlaneModifierHi[0] == EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, "EGL token drift");
static_assert(zc::kEglPlaneModifierLo[1] == EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, "EGL token drift");
static_assert(zc::kEglPlaneModifierHi[1] == EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, "EGL token drift");
static_assert(zc::kEglPlaneModifierLo[2] == EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, "EGL token drift");
static_assert(zc::kEglPlaneModifierHi[2] == EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, "EGL token drift");
static_assert(zc::kEglYuvColorSpaceHint == EGL_YUV_COLOR_SPACE_HINT_EXT, "EGL token drift");
static_assert(zc::kEglSampleRangeHint == EGL_SAMPLE_RANGE_HINT_EXT, "EGL token drift");
static_assert(zc::kEglItuRec601 == EGL_ITU_REC601_EXT, "EGL token drift");
static_assert(zc::kEglItuRec709 == EGL_ITU_REC709_EXT, "EGL token drift");
static_assert(zc::kEglItuRec2020 == EGL_ITU_REC2020_EXT, "EGL token drift");
static_assert(zc::kEglYuvFullRange == EGL_YUV_FULL_RANGE_EXT, "EGL token drift");
static_assert(zc::kEglYuvNarrowRange == EGL_YUV_NARROW_RANGE_EXT, "EGL token drift");
static_assert(sizeof(EGLint) == sizeof(std::int32_t), "EGLint is 32-bit");

namespace {

// Same vertex stage as the copy path (gst_renderer.cpp) — the external
// program draws through the renderer's existing quad VAO (locations 0/1).
const char* kVertexEssl3 = R"(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 TexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

// The driver converts YUV -> RGB itself when sampling an external image,
// using the EGL_YUV_COLOR_SPACE / SAMPLE_RANGE hints given at import.
const char* kFragmentEssl3 = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
in vec2 TexCoord;
out vec4 FragColor;
uniform samplerExternalOES textureY;
void main() {
    FragColor = vec4(texture(textureY, TexCoord).rgb, 1.0);
}
)";

const char* kVertexEssl1 = R"(#version 100
attribute vec2 aPos;
attribute vec2 aTexCoord;
varying vec2 TexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

const char* kFragmentEssl1 = R"(#version 100
#extension GL_OES_EGL_image_external : require
precision mediump float;
varying vec2 TexCoord;
uniform samplerExternalOES textureY;
void main() {
    gl_FragColor = vec4(texture2D(textureY, TexCoord).rgb, 1.0);
}
)";

GLuint compile(GLenum type, const char* src, std::string* log) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512] = {0};
        glGetShaderInfoLog(s, sizeof(buf), nullptr, buf);
        *log = buf;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

std::string hex(unsigned v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%x", v);
    return buf;
}

void drain_gl_errors() {
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
    }
}

zc::ColorMatrix to_matrix(GstVideoColorMatrix m) {
    switch (m) {
        case GST_VIDEO_COLOR_MATRIX_BT709: return zc::ColorMatrix::Bt709;
        case GST_VIDEO_COLOR_MATRIX_BT601:
        case GST_VIDEO_COLOR_MATRIX_SMPTE240M:
        case GST_VIDEO_COLOR_MATRIX_FCC: return zc::ColorMatrix::Bt601;
        case GST_VIDEO_COLOR_MATRIX_BT2020: return zc::ColorMatrix::Bt2020;
        case GST_VIDEO_COLOR_MATRIX_UNKNOWN: return zc::ColorMatrix::Unknown;
        default: return zc::ColorMatrix::Other;
    }
}

zc::ColorRange to_range(GstVideoColorRange r) {
    switch (r) {
        case GST_VIDEO_COLOR_RANGE_0_255: return zc::ColorRange::Full;
        case GST_VIDEO_COLOR_RANGE_16_235: return zc::ColorRange::Limited;
        default: return zc::ColorRange::Unknown;
    }
}

// Plain (copy-path readable) video info for these caps. `modifier` is set
// for DMA_DRM caps; left at kDrmFormatModInvalid for legacy caps.
bool video_info_for(GstCaps* caps, GstVideoInfo* info, std::uint64_t* modifier,
                    bool* is_dma_drm) {
    *modifier = zc::kDrmFormatModInvalid;
    *is_dma_drm = false;
#if GST_CHECK_VERSION(1, 24, 0)
    if (gst_video_is_dma_drm_caps(caps)) {
        *is_dma_drm = true;
        GstVideoInfoDmaDrm drm;
        if (!gst_video_info_dma_drm_from_caps(&drm, caps)) return false;
        *modifier = drm.drm_modifier;
        // Only succeeds for LINEAR — exactly what dmabuf_appsink_caps()
        // asks for, so the copy fallback can always read these frames.
        return gst_video_info_dma_drm_to_video_info(&drm, info) != FALSE;
    }
#endif
    return gst_video_info_from_caps(info, caps) != FALSE;
}

}  // namespace

zc::Decision configure_zero_copy(bool board_candidate) {
    const bool flag = zc::env_flag_enabled(std::getenv("MDB_VIDEO_ZERO_COPY"));
    if (!flag || !board_candidate) {
        // Default path: no extension probe, nothing else touched.
        zc::Decision d = zc::decide(flag, board_candidate, nullptr);
        if (flag) {
            LOG_WARN("Video upload: copy ({}) — MDB_VIDEO_ZERO_COPY=1 ignored", d.reason);
        } else {
            LOG_INFO("Video upload: copy ({})", d.reason);
        }
        return d;
    }

    EGLDisplay dpy = eglGetCurrentDisplay();
    const char* egl_ext =
        dpy != EGL_NO_DISPLAY ? eglQueryString(dpy, EGL_EXTENSIONS) : nullptr;
    const char* gl_ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    zc::Decision d;
    if (dpy == EGL_NO_DISPLAY || egl_ext == nullptr || gl_ext == nullptr) {
        d = zc::decide(true, true, nullptr);
    } else {
        const zc::ExtensionAvailability ext = zc::parse_extensions(egl_ext, gl_ext);
        d = zc::decide(true, true, &ext);
    }
    if (d.enabled &&
        (eglGetProcAddress("eglCreateImageKHR") == nullptr ||
         eglGetProcAddress("eglDestroyImageKHR") == nullptr ||
         eglGetProcAddress("glEGLImageTargetTexture2DOES") == nullptr)) {
        d.enabled = false;
        d.reason = "EGL image entry points not resolvable";
    }

    if (d.enabled) {
        LOG_INFO("Video upload: zero-copy dmabuf requested (EXPERIMENTAL; "
                 "sampler {}, modifiers {}) — per-frame copy fallback stays armed",
                 d.dialect == zc::ShaderDialect::Essl3 ? "ESSL3 external" : "ESSL1 external",
                 d.use_modifiers ? "yes" : "no");
    } else {
        LOG_WARN("Video upload: copy ({}) — MDB_VIDEO_ZERO_COPY=1 ignored", d.reason);
    }
    return d;
}

DmabufImporter::DmabufImporter(const zc::Decision& decision) : decision_(decision) {}

DmabufImporter::~DmabufImporter() {
    // GL objects are the renderer's to delete (destroy_gl/reset_gl) while a
    // context is known to be current; here only non-GL resources go.
    release_held_samples();
    destroy_cached_images();
    drop_caps_cache();
}

bool DmabufImporter::init() {
    display_ = eglGetCurrentDisplay();
    create_image_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    destroy_image_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    image_target_texture_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    return decision_.enabled && display_ != EGL_NO_DISPLAY && create_image_ &&
           destroy_image_ && image_target_texture_;
}

bool DmabufImporter::ensure_gl() {
    if (gl_failed_) return false;
    if (texture_ == 0) {
        glGenTextures(1, &texture_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture_);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    }
    if (program_ == 0) {
        const bool essl3 = decision_.dialect == zc::ShaderDialect::Essl3;
        std::string log;
        GLuint vs = compile(GL_VERTEX_SHADER, essl3 ? kVertexEssl3 : kVertexEssl1, &log);
        GLuint fs = vs ? compile(GL_FRAGMENT_SHADER,
                                 essl3 ? kFragmentEssl3 : kFragmentEssl1, &log)
                       : 0;
        if (!vs || !fs) {
            if (vs) glDeleteShader(vs);
            LOG_WARN("Zero-copy: external-sampler shader failed to compile: {}", log);
            gl_failed_ = true;
            return false;
        }
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        if (!essl3) {  // ESSL 1.00 has no layout(location) qualifiers
            glBindAttribLocation(prog, 0, "aPos");
            glBindAttribLocation(prog, 1, "aTexCoord");
        }
        glLinkProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint linked = GL_FALSE;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) {
            char buf[512] = {0};
            glGetProgramInfoLog(prog, sizeof(buf), nullptr, buf);
            LOG_WARN("Zero-copy: external-sampler program failed to link: {}", buf);
            glDeleteProgram(prog);
            gl_failed_ = true;
            return false;
        }
        GLint prev = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &prev);
        glUseProgram(prog);
        glUniform1i(glGetUniformLocation(prog, "textureY"), 0);
        glUseProgram(static_cast<GLuint>(prev));
        program_ = prog;
    }
    return true;
}

DmabufImporter::Result DmabufImporter::import(GstSample* sample, Geometry* geo,
                                              std::string* error) {
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstCaps* caps = gst_sample_get_caps(sample);
    if (buffer == nullptr || caps == nullptr || gst_buffer_n_memory(buffer) == 0) {
        *error = "sample has no buffer/caps";
        return Result::Failed;
    }
    // Cheap early-out for the common non-dmabuf case (software decode).
    if (!gst_is_dmabuf_memory(gst_buffer_peek_memory(buffer, 0))) {
        return Result::NotDmabuf;
    }

    GstVideoInfo info;
    std::uint64_t modifier = zc::kDrmFormatModInvalid;
    bool is_dma_drm = false;
    if (!video_info_for(caps, &info, &modifier, &is_dma_drm)) {
        *error = is_dma_drm ? "DMA_DRM caps not linear/convertible"
                            : "could not parse video caps";
        return Result::Failed;
    }

    zc::FrameDesc f;
    switch (GST_VIDEO_INFO_FORMAT(&info)) {
        case GST_VIDEO_FORMAT_NV12: f.format = zc::PixelFormat::NV12; break;
        case GST_VIDEO_FORMAT_I420: f.format = zc::PixelFormat::I420; break;
        default:
            *error = std::string("unsupported format ") +
                     gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(&info));
            return Result::Failed;
    }

    // Plane layout: a decoder's GstVideoMeta (its own padding/alignment)
    // wins over the caps' default layout — same rule gst_video_frame_map
    // applies on the copy path.
    const GstVideoMeta* meta = gst_buffer_get_video_meta(buffer);
    f.width = static_cast<int>(meta ? meta->width : GST_VIDEO_INFO_WIDTH(&info));
    f.height = static_cast<int>(meta ? meta->height : GST_VIDEO_INFO_HEIGHT(&info));
    f.n_planes = static_cast<int>(meta ? meta->n_planes : GST_VIDEO_INFO_N_PLANES(&info));
    f.modifier = modifier;
    f.matrix = to_matrix(info.colorimetry.matrix);
    f.range = to_range(info.colorimetry.range);

    std::array<std::uint64_t, 3> inodes{0, 0, 0};
    for (int i = 0; i < f.n_planes && i < 3; ++i) {
        const gsize plane_offset =
            meta ? meta->offset[i] : GST_VIDEO_INFO_PLANE_OFFSET(&info, i);
        const gint stride = meta ? meta->stride[i] : GST_VIDEO_INFO_PLANE_STRIDE(&info, i);
        guint idx = 0, len = 0;
        gsize skip = 0;
        if (!gst_buffer_find_memory(buffer, plane_offset, 1, &idx, &len, &skip)) {
            *error = "plane " + std::to_string(i) + " offset outside the buffer";
            return Result::Failed;
        }
        GstMemory* mem = gst_buffer_peek_memory(buffer, idx);
        if (!gst_is_dmabuf_memory(mem)) {
            return Result::NotDmabuf;  // mixed buffer: copy it
        }
        auto& p = f.planes[static_cast<std::size_t>(i)];
        p.fd = gst_dmabuf_memory_get_fd(mem);
        p.offset = static_cast<std::uint64_t>(mem->offset + skip);
        p.stride = stride;
        struct stat st {};
        if (p.fd >= 0 && fstat(p.fd, &st) == 0) {
            inodes[static_cast<std::size_t>(i)] = static_cast<std::uint64_t>(st.st_ino);
        }
    }

    const zc::AttribResult attribs = zc::build_dmabuf_attribs(f, decision_.use_modifiers);
    if (!attribs.ok) {
        *error = attribs.error;
        return Result::Failed;
    }
    if (!ensure_gl()) {
        *error = "external-sampler shader unavailable";
        return Result::Failed;
    }

    // EGLImage: reuse the cached one for a recycled decoder buffer.
    const zc::ImageKey key = zc::make_image_key(f, inodes);
    const bool cacheable = zc::key_cacheable(key);
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    bool owned_temp = false;
    if (cacheable) {
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->key == key) {
                image = it->image;
                cache_.splice(cache_.begin(), cache_, it);  // mark MRU
                break;
            }
        }
    }
    if (image == EGL_NO_IMAGE_KHR) {
        const std::vector<EGLint> egl_attribs(attribs.attribs.begin(), attribs.attribs.end());
        image = create_image_(display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                              static_cast<EGLClientBuffer>(nullptr), egl_attribs.data());
        if (image == EGL_NO_IMAGE_KHR) {
            *error = "eglCreateImageKHR failed (EGL error " +
                     hex(static_cast<unsigned>(eglGetError())) + ") for " +
                     zc::format_name(f.format) + " " + std::to_string(f.width) + "x" +
                     std::to_string(f.height);
            return Result::Failed;
        }
        if (cacheable) {
            cache_.push_front({key, image});
            while (cache_.size() > kMaxCachedImages) {
                destroy_image_(display_, cache_.back().image);
                cache_.pop_back();
            }
        } else {
            owned_temp = true;
        }
    }

    drain_gl_errors();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture_);
    // Re-targeted on EVERY frame, cached image or not: the decoder rewrote
    // the buffer's contents, and drivers that keep a shadow copy of a
    // linear import (v3d tiles linear sources for the TMU) only refresh it
    // on a (re)bind.
    image_target_texture_(GL_TEXTURE_EXTERNAL_OES, static_cast<GLeglImageOES>(image));
    const GLenum gl_err = glGetError();
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    if (owned_temp) {
        // The texture keeps its own reference to the image's storage.
        destroy_image_(display_, image);
    }
    if (gl_err != GL_NO_ERROR) {
        *error = "glEGLImageTargetTexture2DOES failed (GL error " +
                 hex(static_cast<unsigned>(gl_err)) + ")";
        has_frame_ = false;
        return Result::Failed;
    }

    // Keep the buffer alive until the frame that samples it is presented.
    if (GstSample* evicted = held_.push(gst_sample_ref(sample))) {
        gst_sample_unref(evicted);
    }
    has_frame_ = true;

    geo->width = f.width;
    geo->height = f.height;
    geo->par_n = GST_VIDEO_INFO_PAR_N(&info) > 0 ? GST_VIDEO_INFO_PAR_N(&info) : 1;
    geo->par_d = GST_VIDEO_INFO_PAR_D(&info) > 0 ? GST_VIDEO_INFO_PAR_D(&info) : 1;
    geo->format = f.format;
    return Result::Imported;
}

void DmabufImporter::note_copied_frame() {
    has_frame_ = false;
    if (GstSample* evicted = held_.push(nullptr)) {
        gst_sample_unref(evicted);
    }
}

void DmabufImporter::bind_for_draw() const {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture_);
}

void DmabufImporter::unbind_after_draw() const {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
}

GstSample* DmabufImporter::plain_sample_for_copy(GstSample* sample) {
#if GST_CHECK_VERSION(1, 24, 0)
    GstCaps* caps = gst_sample_get_caps(sample);
    if (caps == nullptr || !gst_video_is_dma_drm_caps(caps)) return nullptr;
    if (drm_caps_ == nullptr || !gst_caps_is_equal(drm_caps_, caps)) {
        drop_caps_cache();
        GstVideoInfo info;
        std::uint64_t modifier = 0;
        bool is_dma_drm = false;
        if (!video_info_for(caps, &info, &modifier, &is_dma_drm)) return nullptr;
        plain_caps_ = gst_video_info_to_caps(&info);
        if (plain_caps_ == nullptr) return nullptr;
        drm_caps_ = gst_caps_ref(caps);
    }
    return gst_sample_new(gst_sample_get_buffer(sample), plain_caps_,
                          gst_sample_get_segment(sample), nullptr);
#else
    (void)sample;
    return nullptr;
#endif
}

void DmabufImporter::release_held_samples() {
    held_.clear([](GstSample* s) { gst_sample_unref(s); });
}

void DmabufImporter::destroy_cached_images() {
    if (destroy_image_ != nullptr && display_ != EGL_NO_DISPLAY) {
        for (auto& c : cache_) destroy_image_(display_, c.image);
    }
    cache_.clear();
}

void DmabufImporter::drop_caps_cache() {
    if (drm_caps_) gst_caps_unref(drm_caps_);
    if (plain_caps_) gst_caps_unref(plain_caps_);
    drm_caps_ = nullptr;
    plain_caps_ = nullptr;
}

void DmabufImporter::release_frames() {
    has_frame_ = false;
    release_held_samples();
    destroy_cached_images();
    drop_caps_cache();
    // The texture still references the last imported buffer's storage;
    // deleting it is the only way to let that dmabuf go.
    if (texture_ != 0 && eglGetCurrentContext() != EGL_NO_CONTEXT) {
        glDeleteTextures(1, &texture_);
        texture_ = 0;
    }
}

void DmabufImporter::reset_gl() {
    // Names may be invalid after an external context takeover; deleting a
    // stale name is harmless, same as GstRenderer::reset_gl does.
    destroy_gl();
}

void DmabufImporter::destroy_gl() {
    release_frames();
    if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
        if (texture_ != 0) glDeleteTextures(1, &texture_);
        if (program_ != 0) glDeleteProgram(program_);
    }
    texture_ = 0;
    program_ = 0;
    gl_failed_ = false;
}

}  // namespace video
