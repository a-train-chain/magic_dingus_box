#include "platform/frame_presenter.h"

#include "platform/drm_display.h"
#include "platform/egl_context.h"

#include <gbm.h>
#include <poll.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace platform {

namespace {

// drmHandleEvent has no context argument of its own: the flip's user data
// is the flip's TOKEN (see flip_sequencer.h rule 1), so the function to
// hand it to is parked here for the duration of the call. Render thread
// only.
thread_local const FlipBackend::EventFn* g_event_sink = nullptr;

void page_flip_handler(int /*fd*/, unsigned int /*frame*/,
                       unsigned int /*sec*/, unsigned int /*usec*/,
                       void* data) {
    if (g_event_sink != nullptr) {
        (*g_event_sink)(static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(data)));
    }
}

// libdrm's mode calls return -errno.
int as_errno(int ret) {
    if (ret == 0) return 0;
    if (ret < 0) return -ret;
    return errno != 0 ? errno : EIO;
}

// The kernel, for one frame: every call targets `fb_id`.
class DrmFlipBackend final : public FlipBackend {
public:
    DrmFlipBackend(int fd, uint32_t crtc_id, uint32_t connector_id,
                   uint32_t fb_id, drmModeModeInfo* mode)
        : fd_(fd), crtc_id_(crtc_id), connector_id_(connector_id),
          fb_id_(fb_id), mode_(mode) {}

    std::int64_t now_ms() override {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    bool pump_events(int timeout_ms, const EventFn& on_event) override {
        struct pollfd pfd = {};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        const int ready = poll(&pfd, 1, timeout_ms);
        if (ready < 0) return errno == EINTR;  // interrupted: caller re-times
        if (ready == 0) return true;           // nothing within the timeout
        if ((pfd.revents & POLLIN) == 0) return false;  // POLLERR / POLLHUP

        drmEventContext evctx = {};
        evctx.version = 2;
        evctx.page_flip_handler = page_flip_handler;
        g_event_sink = &on_event;
        const int ret = drmHandleEvent(fd_, &evctx);
        g_event_sink = nullptr;
        return ret == 0;
    }

    int page_flip(std::uint64_t token) override {
        return as_errno(drmModePageFlip(
            fd_, crtc_id_, fb_id_, DRM_MODE_PAGE_FLIP_EVENT,
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(token))));
    }

    int set_crtc() override {
        return as_errno(drmModeSetCrtc(fd_, crtc_id_, fb_id_, 0, 0,
                                       &connector_id_, 1, mode_));
    }

private:
    int fd_;
    uint32_t crtc_id_;
    uint32_t connector_id_;
    uint32_t fb_id_;
    drmModeModeInfo* mode_;
};

}  // namespace

FramePresenter::FramePresenter(DrmDisplay& display, EglContext& egl,
                               const drmModeModeInfo& mode_info)
    : display_(display), egl_(egl), mode_info_(mode_info) {}

void FramePresenter::remove_fb(uint32_t fb_id) {
    drmModeRmFB(display_.get_fd(), fb_id);
}

void FramePresenter::release_buffer(const HeldBuffer& held) {
    if (held.bo != nullptr && egl_.get_gbm_surface() != nullptr) {
        gbm_surface_release_buffer(egl_.get_gbm_surface(), held.bo);
    }
}

uint32_t FramePresenter::scanout_fb() const {
    return held_.scanout() ? held_.scanout()->fb_id : 0;
}

uint32_t FramePresenter::pending_fb() const {
    return held_.pending() ? held_.pending()->fb_id : 0;
}

void FramePresenter::reset() {
    // Every cached fb goes, including the "current" one: the game owned the
    // CRTC since, so none of ours is on screen — and no flip of ours is
    // pending any more (an event still queued for one is stale; the
    // sequencer never reuses a token).
    fb_cache_.clear([this](uint32_t fb) { remove_fb(fb); });
    held_.release_all([this](const HeldBuffer& b) { release_buffer(b); });

    sequencer_.reset();  // SetCrtc for the first frames back
    first_frame_announced_ = false;
    consecutive_buffer_failures_ = 0;
}

void FramePresenter::shutdown() {
    fb_cache_.clear([this](uint32_t fb) { remove_fb(fb); });
    held_.release_all([this](const HeldBuffer& b) { release_buffer(b); });
}

bool FramePresenter::present(uint32_t width, uint32_t height) {
    const auto rm = [this](uint32_t fb) { remove_fb(fb); };
    const auto release = [this](const HeldBuffer& b) { release_buffer(b); };

    // Bound the cache. Never evicts the fb on screen or the fb of a flip
    // still pending — see fb_cache.h.
    fb_cache_.trim(kMaxFbCache, scanout_fb(), rm, pending_fb());

    // The buffer on screen (and a pending flip's) stay locked until the
    // kernel is done with them — ScanoutBuffers. The surface has four, so
    // the frame just rendered always has one.
    gbm_bo* bo = gbm_surface_lock_front_buffer(egl_.get_gbm_surface());
    if (!bo) {
        consecutive_buffer_failures_++;
        std::cerr << "Failed to lock front buffer! GPU memory may be exhausted." << std::endl;
        std::cerr << "  Consecutive failures: " << consecutive_buffer_failures_ << std::endl;
        std::cerr << "  This usually means GBM buffer pool is exhausted." << std::endl;

        if (consecutive_buffer_failures_ > 5) {
            std::cerr << "CRITICAL: Too many consecutive buffer failures - attempting recovery" << std::endl;
            // Keep the on-screen fb and a pending flip's fb cached (and
            // alive): RmFB'ing one would blank the TV, and dropping it
            // from the cache without RmFB leaked it. Their buffers stay
            // held for the same reason.
            fb_cache_.drop_all_except(scanout_fb(), rm, pending_fb());
            consecutive_buffer_failures_ = 0;
            std::cerr << "Recovery complete - cleared framebuffer cache" << std::endl;
        }

        // Sleep a bit and try again next frame
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        return false;
    }

    consecutive_buffer_failures_ = 0;

    const uint32_t bo_handle = gbm_bo_get_handle(bo).u32;
    uint32_t fb_id = 0;

    if (auto cached = fb_cache_.find(bo_handle)) {
        fb_id = *cached;
    } else {
        uint32_t handles[4] = {0};
        uint32_t strides[4] = {0};
        uint32_t offsets[4] = {0};

        handles[0] = bo_handle;
        strides[0] = gbm_bo_get_stride(bo);
        offsets[0] = 0;

        // The format the GBM surface was created with.
        const uint32_t format = GBM_FORMAT_XRGB8888;

        int ret = drmModeAddFB2(display_.get_fd(), width, height, format,
                                handles, strides, offsets, &fb_id, 0);
        if (ret != 0) {
            // Fallback to AddFB (legacy API)
            ret = drmModeAddFB(display_.get_fd(), width, height, 24, 32,
                               strides[0], handles[0], &fb_id);
            if (ret != 0 && sequencer_.awaiting_first_frame()) {
                std::cerr << "AddFB2 failed (ret=" << ret << "), AddFB also failed (ret=" << ret << ")" << std::endl;
            }
        }

        if (ret == 0 && fb_id != 0) {
            fb_cache_.insert(bo_handle, fb_id, rm);
        } else {
            std::cerr << "ERROR: Failed to create DRM framebuffer (ret=" << ret << "): " << strerror(errno) << std::endl;
            std::cerr << "  width=" << width << ", height=" << height << std::endl;
            std::cerr << "  stride=" << strides[0] << ", handle=" << handles[0] << std::endl;
            std::cerr << "  fb_cache size=" << fb_cache_.size() << std::endl;
            gbm_surface_release_buffer(egl_.get_gbm_surface(), bo);
            return false;  // Skip this frame if framebuffer creation failed
        }
    }

    // SetCrtc, page flip, wait or drop: flip_sequencer.h decides, this
    // backend only makes the calls for this frame's fb.
    DrmFlipBackend backend(display_.get_fd(), display_.get_crtc_id(),
                           display_.get_connector_id(), fb_id, &mode_info_);

    const bool first_frame = sequencer_.awaiting_first_frame();
    if (first_frame && !first_frame_announced_) {
        std::cout << "Setting initial CRTC: fb_id=" << fb_id << ", crtc_id=" << display_.get_crtc_id() << std::endl;
        first_frame_announced_ = true;
    }

    const FrameReport report = sequencer_.present(backend);

    if (first_frame && report.result == FrameResult::ShownBySetCrtc) {
        std::cout << "Initial CRTC set successfully!" << std::endl;
    }

    // Buffers the kernel is done with go back to the surface; this frame's
    // is kept while it is on screen or its flip is pending, released now
    // if it was dropped.
    held_.apply(report, HeldBuffer{bo, fb_id}, release);

    // One line per episode of trouble (and one per 10 s while it lasts),
    // never one per frame.
    if (auto summary = sequencer_.take_summary(backend.now_ms())) {
        std::cerr << describe(*summary) << std::endl;
    }

    return report.result != FrameResult::Dropped;
}

}  // namespace platform
