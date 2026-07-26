#pragma once

#include <cstdint>
#include <memory>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include "stream_gate.h"

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

    // Toggle source-aspect-ratio preservation in render_quad's
    // default (non-letterbox) path. When `true` (default), the
    // renderer compares frame_w/h × PAR to width_/height_ and adds
    // letterbox/pillar bars so the video keeps its true display
    // aspect on the screen — necessary in Modern TV mode for Media
    // Browser playback so a 2.35:1 movie doesn't get vertically
    // stretched to fill the full 16:9 HDMI output.
    //
    // When `false`, the renderer skips that math and stretches the
    // video to fill the entire framebuffer instead. This is what
    // CRT_NATIVE mode wants: the Pi outputs 1280×720 HDMI to a CRT
    // TV, and the TV's HDMI input does its own 16:9→4:3 conversion
    // at the receiving end (typically by horizontal cropping or
    // anamorphic squeeze). If the Pi pre-pillarboxes the 4:3 video
    // INSIDE the 1280×720 frame, the CRT TV's downstream conversion
    // then adds ANOTHER round of bars and the operator sees a tiny
    // window-boxed image with margins on all four sides.
    //
    // Defaults to true so it doesn't break Modern TV's existing
    // aspect-preserving behavior; main.cpp flips it to false when
    // entering CRT_NATIVE mode.
    void set_aspect_preserve(bool enabled) { aspect_preserve_ = enabled; }
    void set_screen_size(uint32_t width, uint32_t height) { width_ = width; height_ = height; }
    void set_swap_uv(bool enabled) { swap_uv_ = enabled; }

    // Inset rect — the sub-region of the default framebuffer the
    // video should render INTO. width=0 / height=0 means "use the
    // full canvas" (the default; resets back when leaving Marquee
    // playback). When set, all of the existing letterbox /
    // aspect-preserve / fill-to-edges math runs within this rect, so
    // Pulp Fiction's 2.39:1 still letterboxes correctly inside the
    // wood-frame's interior — it just letterboxes inside (40, 40,
    // 1200, 640) instead of (0, 0, 1280, 720).
    //
    // Used by Media Browser playback to keep the movie INSIDE the
    // 40 px wood-frame overlay. Caller must also clear the
    // framebuffer to black before render() so the gap between the
    // inset rect and any letterbox bars stays clean (the renderer
    // only paints inside the inset).
    void set_render_inset(int x, int y, int w, int h) {
        render_inset_x_ = x;
        render_inset_y_ = y;
        render_inset_w_ = w;
        render_inset_h_ = h;
    }
    
    // Check if texture is ready
    bool is_ready() const { return gl_initialized_; }

private:
    GstPlayer* player_;
    GstElement* appsink_;
    
    uint32_t width_;
    uint32_t height_;

    // Inset rect — when render_inset_w_ > 0 AND render_inset_h_ > 0,
    // the renderer treats this rect as its working canvas instead of
    // the full (width_ × height_) framebuffer. Defaults to 0 (off) so
    // legacy callers see no change. See set_render_inset() docstring.
    int render_inset_x_ = 0;
    int render_inset_y_ = 0;
    int render_inset_w_ = 0;
    int render_inset_h_ = 0;

    // OpenGL resources
    GLuint texture_ids_[3]; // Support up to 3 planes (Y, U, V)
    GLuint program_id_;
    GLuint vao_id_;
    GLuint vbo_id_;
    
    // Current frame properties
    int frame_width_;
    int frame_height_;
    // Pixel aspect ratio (PAR) of the source. Most modern video has PAR 1:1
    // (square pixels), but DVDs, some broadcast captures, and certain MKV
    // remuxes encode anamorphically (e.g. 720x480 with PAR 32:27 to display
    // as 16:9). Display aspect = (frame_width * par_n) / (frame_height * par_d).
    // Both default to 1 so square-pixel content works without explicit detect.
    int frame_par_num_ = 1;
    int frame_par_den_ = 1;
    int frame_format_; // 0=RGBA, 1=I420, 2=NV12
    
    bool gl_initialized_;
    bool letterbox_mode_ = false;  // When true, render 4:3 centered
    bool aspect_preserve_ = true;  // When false, stretch to fill (CRT_NATIVE behavior)
    bool swap_uv_ = false; // Swap U/V planes (fix for red video)
    bool frame_dirty_ = false;  // Set when new frame arrives from appsink

    // Suppresses drawing across stream boundaries: the textures hold the
    // PREVIOUS video's last frame until the new stream's first sample
    // uploads, and painting them flashes that stale frame (the intro
    // reappearing before a movie starts). See stream_gate.h.
    StreamGate stream_gate_;
    int suppressed_ticks_ = 0;  // Diagnostics: black frames drawn this boundary
    bool textures_allocated_ = false;  // Track if textures have been allocated
    int allocated_width_ = 0;
    int allocated_height_ = 0;
    int allocated_format_ = -1;  // Track allocated format for glTexSubImage2D
    
    void init_gl_resources();
    void upload_frame(GstSample* sample);
    void render_quad();
    void update_shader(int format);
};

} // namespace video
