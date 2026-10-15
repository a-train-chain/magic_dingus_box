#pragma once

// Scans the frame EGL just rendered out to the TV: locks the GBM front
// buffer, wraps it in a (cached) DRM framebuffer, and SetCrtc's or
// page-flips it. Extracted from a ~250-line lambda in main.cpp so its
// state has one owner and an explicit lifecycle:
//
//   construct  -> after DrmDisplay/GBM/EGL are up and the mode is set
//   present()  -> once per rendered frame, AFTER egl.swap_buffers()
//   reset()    -> after a game returns the display (DRM master re-acquired,
//                 mode restored), BEFORE the next present(). Drops every
//                 cached fb and the held buffer, and forces SetCrtc for the
//                 next 10 frames — RetroArch owned the CRTC in between, so a
//                 bare page flip onto it is not trustworthy.
//   shutdown() -> at exit, before EglContext/GbmContext are torn down (the
//                 held buffer belongs to their surface).
//
// The display fd and GBM surface are looked up through DrmDisplay /
// EglContext on every call, never cached: both can be recreated across a
// game session, and a cached copy would outlive them.

#include <cstdint>
#include <xf86drmMode.h>

#include "platform/fb_cache.h"

struct gbm_bo;

namespace platform {

class DrmDisplay;
class EglContext;

class FramePresenter {
public:
    // mode_info is the exact timing set on the CRTC at boot; it is copied
    // because it never changes for the life of the process (a resolution
    // change is deferred to a restart — see main.cpp's display-mode block).
    FramePresenter(DrmDisplay& display, EglContext& egl,
                   const drmModeModeInfo& mode_info);

    FramePresenter(const FramePresenter&) = delete;
    FramePresenter& operator=(const FramePresenter&) = delete;

    // width/height: the CURRENT mode, used to size new framebuffers.
    void present(uint32_t width, uint32_t height);
    void reset();
    void shutdown();

private:
    // At most this many framebuffers stay cached (a GBM surface cycles
    // 2-3 buffers; 4 leaves headroom without hoarding).
    static constexpr std::size_t kMaxFbCache = 4;

    void remove_fb(uint32_t fb_id);
    void release_previous_bo();

    DrmDisplay& display_;
    EglContext& egl_;
    drmModeModeInfo mode_info_;

    FbCache fb_cache_;
    gbm_bo* previous_bo_ = nullptr;       // presented last frame; released next frame
    uint32_t previous_bo_handle_ = 0;
    uint32_t current_fb_id_ = 0;          // on screen right now
    bool first_frame_ = true;
    int force_setcrtc_frames_ = 0;        // SetCrtc instead of flip after a reset
    int consecutive_buffer_failures_ = 0;
    int page_flip_failures_ = 0;
    int successful_page_flips_ = 0;
    // Must outlive the flip it is passed to — the event handler writes it.
    bool waiting_for_flip_ = false;
};

}  // namespace platform
