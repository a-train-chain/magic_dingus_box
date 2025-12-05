#pragma once

#include <cstdint>
#include <memory>
#include <EGL/egl.h>

struct mpv_handle;
struct mpv_render_context;

namespace video {

class MpvPlayer;

class MpvRenderer {
public:
    MpvRenderer();
    ~MpvRenderer();

    // Initialize render context from mpv player and EGL display
    bool initialize(mpv_handle* mpv, EGLDisplay egl_display);
    
    // Set viewport size
    void set_viewport_size(uint32_t width, uint32_t height);
    
    // Set mpv handle for property access (optional, for setting window size)
    void set_mpv_handle(mpv_handle* mpv);
    
    // Render current frame (call each frame in main loop)
    void render();
    
    // Check if redraw is needed
    bool needs_redraw() const { return needs_redraw_; }
    
    // Get update flags without rendering (to check if frame is ready)
    uint64_t get_update_flags() const;
    
    // Cleanup
    void cleanup();

private:
    mpv_render_context* render_context_;
    mpv_handle* mpv_handle_;  // For setting properties like window size
    uint32_t width_;
    uint32_t height_;
    bool needs_redraw_;
    
    static void on_update(void* ctx);
    void* update_callback_ctx_;
};

} // namespace video

