#pragma once

// EXPERIMENTAL zero-copy video path — the GPU half (EGL + GL + GStreamer).
// The pure half (decision, attribute list, fallback counter, hold ring) is
// video/zero_copy_policy.h; enabling/validating is docs/ZERO_COPY_VIDEO.md.
//
// Lifecycle:
//   main.cpp  configure_zero_copy()      (EGL context current; logs once)
//   GstPlayer set_zero_copy_dmabuf()     (DMABuf caps prepended)
//   GstRenderer enable_zero_copy()       (creates one DmabufImporter)
//   per frame DmabufImporter::import()   (EGLImage -> external texture)
// Everything here runs on the render thread, the only thread with the EGL
// context current.

#include "zero_copy_policy.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <gst/gst.h>

#include <list>
#include <string>

namespace video {

// Reads MDB_VIDEO_ZERO_COPY and, ONLY when it is "1" and the board is a
// candidate, probes the current EGL display / GL context for the import
// extensions and entry points. Logs the outcome once:
//   "Video upload: copy (<reason>)" or
//   "Video upload: zero-copy dmabuf requested (...)".
// Requires the kiosk's EGL context to be current.
zero_copy::Decision configure_zero_copy(bool board_candidate);

class DmabufImporter {
public:
    enum class Result {
        Imported,   // bound to the external texture; draw with program()
        NotDmabuf,  // system-memory frame (software decode / pool copy): copy it
        Failed      // import error: copy it, count it, `error` says why
    };

    struct Geometry {
        int width = 0;
        int height = 0;
        int par_n = 1;
        int par_d = 1;
        zero_copy::PixelFormat format = zero_copy::PixelFormat::Unsupported;
    };

    explicit DmabufImporter(const zero_copy::Decision& decision);
    ~DmabufImporter();

    DmabufImporter(const DmabufImporter&) = delete;
    DmabufImporter& operator=(const DmabufImporter&) = delete;

    // Resolves the EGL/GL entry points. False = unusable (never import).
    bool init();

    // Import the sample's buffer. On Imported, the importer holds its own
    // ref on `sample` (hold ring) until the frame after next is imported.
    Result import(GstSample* sample, Geometry* geo, std::string* error);

    // A copy-path frame was drawn instead: advance the hold ring so a run
    // of copied frames does not pin decoder buffers forever.
    void note_copied_frame();

    // True when an imported frame is bound and drawable.
    bool has_frame() const { return has_frame_ && texture_ != 0 && program_ != 0; }
    GLuint program() const { return program_; }
    void bind_for_draw() const;     // unit 0 <- external texture
    void unbind_after_draw() const;

    // DMA_DRM caps (GStreamer >= 1.24) cannot be read by the copy path's
    // gst_video_info_from_caps. For a sample whose caps are DMA_DRM, returns
    // a new sample with equivalent plain video/x-raw caps (linear only) for
    // the copy fallback; otherwise nullptr (use the sample as-is).
    GstSample* plain_sample_for_copy(GstSample* sample);

    // Drop every held sample and cached EGLImage (frees the decoder's
    // buffers). Deletes the external texture when a context is current.
    // Called on pipeline stop (GstPlayer hook), stream change, fallback.
    void release_frames();

    // External GL context takeover (RetroArch): GL names are invalid.
    void reset_gl();
    // Normal teardown with the context current.
    void destroy_gl();

private:
    struct CachedImage {
        zero_copy::ImageKey key;
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
    };

    bool ensure_gl();
    void destroy_cached_images();
    void release_held_samples();
    void drop_caps_cache();

    zero_copy::Decision decision_;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    PFNEGLCREATEIMAGEKHRPROC create_image_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture_ = nullptr;

    GLuint texture_ = 0;
    GLuint program_ = 0;
    bool gl_failed_ = false;
    bool has_frame_ = false;

    // Small decoder pools (v4l2h264dec: a handful of capture buffers) are
    // recycled, so the same dmabuf comes back every few frames: reuse its
    // EGLImage. Bounded so a resolution change or a pool reallocation
    // cannot pin old buffers (each EGLImage holds a dmabuf reference).
    static constexpr std::size_t kMaxCachedImages = 12;
    std::list<CachedImage> cache_;  // front = most recently used

    zero_copy::HoldRing<GstSample*, 2> held_;

    GstCaps* drm_caps_ = nullptr;    // last DMA_DRM caps seen (ref)
    GstCaps* plain_caps_ = nullptr;  // its plain equivalent (ref)
};

}  // namespace video
