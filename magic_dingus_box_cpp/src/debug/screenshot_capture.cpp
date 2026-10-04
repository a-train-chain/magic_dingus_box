#include "debug/screenshot_capture.h"

#include <GLES3/gl3.h>

#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

#include "utils/bmp_writer.h"
#include "utils/logger.h"
#include "utils/screenshot_store.h"

namespace fs = std::filesystem;

namespace debug {

ScreenshotCapture::ScreenshotCapture(std::string data_dir)
    : request_path_(data_dir + "/screenshot_request"),
      screenshots_dir_(data_dir + "/screenshots") {}

ScreenshotCapture::~ScreenshotCapture() {
    if (worker_.joinable()) worker_.join();
}

bool ScreenshotCapture::poll(std::chrono::steady_clock::time_point now) {
    if (pending_) return true;
    if (now < next_poll_) return false;
    next_poll_ = now + kPollInterval;
    // The worker deletes the request file when it finishes; until then the
    // file still exists and must not trigger a second capture.
    if (busy_.load(std::memory_order_acquire)) return false;
    std::error_code ec;
    if (fs::exists(request_path_, ec)) {
        pending_ = true;
        LOG_INFO("Screenshot requested — capturing the next drawn frame");
    }
    return pending_;
}

void ScreenshotCapture::drop_request() {
    std::error_code ec;
    fs::remove(request_path_, ec);
}

void ScreenshotCapture::capture_before_swap(int width, int height) {
    if (!pending_) return;
    pending_ = false;

    if (width <= 0 || height <= 0) {
        LOG_WARN("Screenshot: invalid framebuffer size {}x{} — request dropped",
                 width, height);
        drop_request();
        return;
    }

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) *
                                     static_cast<std::size_t>(height) * 4);

    // Errors left over from earlier rendering must not be blamed on (or
    // hide behind) the readback. Bounded: a lost context can return errors
    // forever on some drivers.
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
    }

    GLint prev_read_fb = 0;
    GLint prev_pack_buffer = 0;
    GLint prev_pack_alignment = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fb);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pack_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack_alignment);

    // The default framebuffer's back buffer is what eglSwapBuffers is about
    // to present. A bound pack buffer would turn the pointer argument into
    // a buffer offset, so unbind it for the call.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glReadBuffer(GL_BACK);  // the default FB's default; no restore needed
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const GLenum read_err = glGetError();

    glPixelStorei(GL_PACK_ALIGNMENT, prev_pack_alignment);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(prev_pack_buffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read_fb));

    if (read_err != GL_NO_ERROR) {
        LOG_WARN("Screenshot: glReadPixels failed (GL error 0x{:x}) — request dropped",
                 static_cast<unsigned>(read_err));
        drop_request();
        return;
    }

    // Previous write finished (poll() refuses requests while busy_), so this
    // join returns immediately; it only reclaims the thread handle.
    if (worker_.joinable()) worker_.join();
    busy_.store(true, std::memory_order_release);

    const std::string name =
        utils::screenshot_filename(std::chrono::system_clock::now());
    worker_ = std::thread([this, width, height, name,
                           px = std::move(pixels)]() {
        const std::string path =
            screenshots_dir_ + "/" + (name.empty() ? "screenshot.bmp" : name);
        const auto bmp =
            utils::encode_bmp24_from_rgba(width, height, px.data(), px.size());
        std::string error;
        if (bmp.empty()) {
            LOG_WARN("Screenshot: BMP encode failed for {}x{}", width, height);
        } else if (!utils::write_file_atomic(path, bmp, &error)) {
            LOG_WARN("Screenshot: write failed: {}", error);
        } else {
            utils::prune_screenshots(screenshots_dir_, utils::kScreenshotsKept);
            LOG_INFO("Screenshot saved: {} ({}x{})", path, width, height);
        }
        drop_request();
        busy_.store(false, std::memory_order_release);
    });
}

}  // namespace debug
