#include "platform/frame_presenter.h"

#include "platform/drm_display.h"
#include "platform/egl_context.h"

#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <sys/select.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace platform {

namespace {

void page_flip_handler(int /*fd*/, unsigned int /*frame*/,
                       unsigned int /*sec*/, unsigned int /*usec*/,
                       void* data) {
    if (auto* waiting = static_cast<bool*>(data)) {
        *waiting = false;
    }
}

}  // namespace

FramePresenter::FramePresenter(DrmDisplay& display, EglContext& egl,
                               const drmModeModeInfo& mode_info)
    : display_(display), egl_(egl), mode_info_(mode_info) {}

void FramePresenter::remove_fb(uint32_t fb_id) {
    drmModeRmFB(display_.get_fd(), fb_id);
}

void FramePresenter::release_previous_bo() {
    if (previous_bo_ != nullptr && egl_.get_gbm_surface() != nullptr) {
        gbm_surface_release_buffer(egl_.get_gbm_surface(), previous_bo_);
    }
    previous_bo_ = nullptr;
    previous_bo_handle_ = 0;
}

void FramePresenter::reset() {
    // Every cached fb goes, including the "current" one: the game owned the
    // CRTC since, so none of ours is on screen.
    fb_cache_.clear([this](uint32_t fb) { remove_fb(fb); });
    release_previous_bo();
    current_fb_id_ = 0;

    first_frame_ = true;
    force_setcrtc_frames_ = 10;  // Force SetCrtc for stability after reset
    consecutive_buffer_failures_ = 0;
    page_flip_failures_ = 0;
    successful_page_flips_ = 0;
}

void FramePresenter::shutdown() {
    fb_cache_.clear([this](uint32_t fb) { remove_fb(fb); });
    release_previous_bo();
    current_fb_id_ = 0;
}

void FramePresenter::present(uint32_t width, uint32_t height) {
    const auto rm = [this](uint32_t fb) { remove_fb(fb); };

    // Double buffering (GBM pools typically have only 2-3 buffers):
    // previous_bo_ was presented last frame and is safe to release now that
    // a newer frame has been rendered. Release it BEFORE locking a new one
    // so the pool has a free buffer. Its fb stays cached: the buffer cycles
    // back, and re-wrapping it every frame would churn kernel objects.
    if (previous_bo_ != nullptr) {
        gbm_surface_release_buffer(egl_.get_gbm_surface(), previous_bo_);
        previous_bo_ = nullptr;
        previous_bo_handle_ = 0;
    }

    // Bound the cache. Never evicts the fb on screen — see fb_cache.h.
    fb_cache_.trim(kMaxFbCache, current_fb_id_, rm);

    gbm_bo* bo = gbm_surface_lock_front_buffer(egl_.get_gbm_surface());
    if (!bo) {
        consecutive_buffer_failures_++;
        std::cerr << "Failed to lock front buffer! GPU memory may be exhausted." << std::endl;
        std::cerr << "  Consecutive failures: " << consecutive_buffer_failures_ << std::endl;
        std::cerr << "  This usually means GBM buffer pool is exhausted." << std::endl;

        if (consecutive_buffer_failures_ > 5) {
            std::cerr << "CRITICAL: Too many consecutive buffer failures - attempting recovery" << std::endl;
            // Keep the on-screen fb cached (and alive): RmFB'ing it would
            // blank the TV, and dropping it from the cache without RmFB
            // leaked it.
            fb_cache_.drop_all_except(current_fb_id_, rm);
            release_previous_bo();
            consecutive_buffer_failures_ = 0;
            std::cerr << "Recovery complete - cleared framebuffer cache" << std::endl;
        }

        // Sleep a bit and try again next frame
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        return;
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
            if (ret != 0 && first_frame_) {
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
            return;  // Skip this frame if framebuffer creation failed
        }
    }

    current_fb_id_ = fb_id;

    if (fb_id != 0) {
        if (first_frame_ || force_setcrtc_frames_ > 0) {
            // SetCrtc for the first frame and for several frames after a
            // reset — stabilizes the CRTC after RetroArch returns control.
            uint32_t connector_id = display_.get_connector_id();
            if (first_frame_) {
                std::cout << "Setting initial CRTC: fb_id=" << fb_id << ", crtc_id=" << display_.get_crtc_id() << std::endl;
            }
            int ret = drmModeSetCrtc(display_.get_fd(), display_.get_crtc_id(), fb_id, 0, 0,
                                     &connector_id, 1, &mode_info_);
            if (ret == 0) {
                if (first_frame_) {
                    std::cout << "Initial CRTC set successfully!" << std::endl;
                    first_frame_ = false;
                }
                if (force_setcrtc_frames_ > 0) {
                    force_setcrtc_frames_--;
                }
            } else {
                std::cerr << "Failed to set CRTC (ret=" << ret << "): " << strerror(errno) << std::endl;
            }
        } else {
            waiting_for_flip_ = true;

            int ret = drmModePageFlip(display_.get_fd(), display_.get_crtc_id(), fb_id,
                                      DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip_);
            if (ret != 0) {
                page_flip_failures_++;
                int err = errno;
                if (page_flip_failures_ % 10 == 0 || page_flip_failures_ < 5) {
                    std::cerr << "Warning: Page flip failed (ret=" << ret << ", errno=" << err << ": " << strerror(err) << ")" << std::endl;
                    std::cerr << "  (Failures: " << page_flip_failures_ << ", Successes: " << successful_page_flips_ << ")" << std::endl;
                }

                // If page flip fails, fall back to SetCrtc
                uint32_t connector_id = display_.get_connector_id();
                ret = drmModeSetCrtc(display_.get_fd(), display_.get_crtc_id(), fb_id, 0, 0,
                                     &connector_id, 1, &mode_info_);
                if (ret != 0) {
                    std::cerr << "Failed to set CRTC: " << strerror(errno) << std::endl;
                }
            } else {
                // Block until the flip lands (bounded: a lost event must
                // not hang the loop past the systemd watchdog).
                drmEventContext evctx = {};
                evctx.version = 2;
                evctx.page_flip_handler = page_flip_handler;

                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(display_.get_fd(), &fds);

                struct timeval timeout;
                timeout.tv_sec = 0;
                timeout.tv_usec = 100000;  // 100ms

                while (waiting_for_flip_) {
                    int sret = select(display_.get_fd() + 1, &fds, NULL, NULL, &timeout);
                    if (sret > 0) {
                        drmHandleEvent(display_.get_fd(), &evctx);
                    } else {
                        if (sret == 0) std::cerr << "Warning: Page flip wait timed out" << std::endl;
                        break;
                    }
                }

                successful_page_flips_++;
                if (successful_page_flips_ >= 100) {
                    if (page_flip_failures_ > 0) {
                        std::cout << "Page flip recovery: " << page_flip_failures_
                                  << " failures in last " << successful_page_flips_ << " frames" << std::endl;
                    }
                    page_flip_failures_ = 0;
                    successful_page_flips_ = 0;
                }
            }
        }
    }

    // Released at the start of the NEXT present, once a newer frame exists.
    previous_bo_ = bo;
    previous_bo_handle_ = bo_handle;
}

}  // namespace platform
