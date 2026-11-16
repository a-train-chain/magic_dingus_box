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
//                 cached fb and every held buffer, and forces SetCrtc for
//                 the next 10 frames — RetroArch owned the CRTC in between,
//                 so a bare page flip onto it is not trustworthy.
//   shutdown() -> at exit, before EglContext/GbmContext are torn down (the
//                 held buffers belong to their surface).
//
// This file is only the DRM/GBM calls. WHEN to flip, wait, SetCrtc or drop
// a frame is platform::FlipSequencer, and which buffers are still the
// kernel's is platform::ScanoutBuffers (flip_sequencer.h) — both pure and
// tested on the Mac against a fake kernel. Read the rules at the top of
// that header before changing anything here: a flip requested on top of a
// pending one, or an event taken for the wrong flip, is how one late
// vblank used to cost every later frame.
//
// The display fd and GBM surface are looked up through DrmDisplay /
// EglContext on every call, never cached: both can be recreated across a
// game session, and a cached copy would outlive them.

#include <cstdint>
#include <xf86drmMode.h>

#include "platform/fb_cache.h"
#include "platform/flip_sequencer.h"

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
    //
    // Returns false when the frame was DROPPED: it is not on screen and
    // never will be (an earlier page flip was still pending when this
    // frame's wait budget ended, the kernel refused it, or no buffer /
    // framebuffer could be had). The picture on screen is unchanged. A
    // caller that draws only when something changed (the redraw gate) must
    // draw again, or the dropped frame's content is never shown.
    //
    // Waits at most FlipSequencer::kFrameWaitBudgetMs for flip events.
    bool present(uint32_t width, uint32_t height);
    void reset();
    void shutdown();

private:
    // At most this many framebuffers stay cached (a GBM surface cycles
    // 2-4 buffers; 4 leaves headroom without hoarding).
    static constexpr std::size_t kMaxFbCache = 4;

    // A rendered buffer the kernel still has: on screen, or the target of
    // a page flip that has not completed.
    struct HeldBuffer {
        gbm_bo* bo = nullptr;
        uint32_t fb_id = 0;
    };

    void remove_fb(uint32_t fb_id);
    void release_buffer(const HeldBuffer& held);
    uint32_t scanout_fb() const;
    uint32_t pending_fb() const;

    DrmDisplay& display_;
    EglContext& egl_;
    drmModeModeInfo mode_info_;

    FbCache fb_cache_;
    FlipSequencer sequencer_;
    ScanoutBuffers<HeldBuffer> held_;
    bool first_frame_announced_ = false;
    int consecutive_buffer_failures_ = 0;
};

}  // namespace platform
