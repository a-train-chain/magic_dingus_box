#pragma once

#include <cstdint>
#include <memory>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

namespace video {

class GstPlayer;

class GstRenderer {
public:
    GstRenderer();
    ~GstRenderer();

    // Initialize renderer with GstPlayer and EGL display (if needed)
    bool initialize(GstPlayer* player);
    
    // Set viewport size
    void set_viewport_size(uint32_t width, uint32_t height);
    
    // Render current frame
    void render();
    
    // Check if we have a new frame
    uint64_t get_update_flags() const;
    
    // Constants for update flags (matching MPV's for compatibility)
    static const uint64_t UPDATE_FRAME = 1;

    void cleanup();
    
    // Reset GL resources after external context takeover (e.g., RetroArch)
    // This invalidates current GL resources and triggers lazy re-init on next render
    void reset_gl();
    
    // Set letterbox mode for 4:3 content in 16:9 frame
    // When enabled, video renders to a centered 4:3 viewport
    void set_letterbox_mode(bool enabled);
    void set_screen_size(uint32_t width, uint32_t height) { width_ = width; height_ = height; }
    void set_swap_uv(bool enabled) { swap_uv_ = enabled; }
    
    // Check if texture is ready
    bool is_ready() const { return gl_initialized_; }

private:
    GstPlayer* player_;
    GstElement* appsink_;
    
    uint32_t width_;
    uint32_t height_;
    
    // OpenGL resources
    GLuint texture_ids_[3]; // Support up to 3 planes (Y, U, V)
    GLuint program_id_;
    GLuint vao_id_;
    GLuint vbo_id_;
    
    // Current frame properties
    int frame_width_;
    int frame_height_;
    int frame_format_; // 0=RGBA, 1=I420, 2=NV12
    
    bool gl_initialized_;
    bool letterbox_mode_ = false;  // When true, render 4:3 centered
    bool swap_uv_ = false; // Swap U/V planes (fix for red video)
    
    void init_gl_resources();
    void upload_frame(GstSample* sample);
    void render_quad();
    void update_shader(int format);
};

} // namespace video
