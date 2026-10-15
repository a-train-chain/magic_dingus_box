#pragma once

// Debug screenshot hook: `touch <data>/screenshot_request` on the box and the
// next drawn frame — the full composited picture (video + UI + CRT + bezel +
// Media Browser + toast + post-game fade) — lands in
// <data>/screenshots/<UTC>.bmp. Exists so a change to rendering can be
// validated on real hardware without a camera pointed at the TV.
//
// Cost model (the render thread must never pay more than one readback):
//   - poll(): one stat() of the request file, at most every kPollInterval;
//   - capture_before_swap(): ONE glReadPixels of the back buffer, on the
//     frame after a request was seen;
//   - BMP encode, file write, retention prune, request-file removal and
//     the log line all run on a worker thread. A new request is not picked
//     up while a previous one is still being written.
//
// Wiring in main.cpp: poll() before the redraw gate, whose decision it
// forces (a skipped frame would never be captured); capture_before_swap()
// after the last draw of the frame and before eglSwapBuffers.

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace debug {

class ScreenshotCapture {
public:
    static constexpr std::chrono::milliseconds kPollInterval{250};

    // data_dir: config::get_data_path(). Files go to data_dir/screenshots.
    explicit ScreenshotCapture(std::string data_dir);
    ~ScreenshotCapture();  // joins an in-flight write

    ScreenshotCapture(const ScreenshotCapture&) = delete;
    ScreenshotCapture& operator=(const ScreenshotCapture&) = delete;

    // Cheap; call every iteration. Returns true while a capture is pending,
    // i.e. the caller must draw (and present) this iteration.
    bool poll(std::chrono::steady_clock::time_point now);

    bool pending() const { return pending_; }

    // Call with the frame fully drawn into the default framebuffer, right
    // before eglSwapBuffers. No-op unless a capture is pending. GL errors
    // are logged and drop the request; GL state touched (read framebuffer,
    // pack buffer, pack alignment) is restored.
    void capture_before_swap(int width, int height);

private:
    void drop_request();

    std::string request_path_;
    std::string screenshots_dir_;
    std::chrono::steady_clock::time_point next_poll_{};
    bool pending_ = false;
    std::atomic<bool> busy_{false};
    std::thread worker_;
};

}  // namespace debug
