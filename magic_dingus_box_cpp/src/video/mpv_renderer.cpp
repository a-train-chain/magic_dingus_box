#include "mpv_renderer.h"

#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <iostream>
#include <cstring>
#include <cassert>

// Get OpenGL function pointer (required by mpv)
static void* get_proc_address(void* /* fn_ctx */, const char* name) {
    return reinterpret_cast<void*>(eglGetProcAddress(name));
}

namespace video {

MpvRenderer::MpvRenderer()
    : render_context_(nullptr)
    , mpv_handle_(nullptr)
    , width_(1280)
    , height_(720)
    , needs_redraw_(false)
    , update_callback_ctx_(this)
{
}

MpvRenderer::~MpvRenderer() {
    cleanup();
}

bool MpvRenderer::initialize(mpv_handle* mpv, EGLDisplay egl_display) {
    if (!mpv) {
        std::cerr << "MpvRenderer::initialize: mpv handle is null" << std::endl;
        return false;
    }
    if (egl_display == EGL_NO_DISPLAY) {
        std::cerr << "MpvRenderer::initialize: egl_display is EGL_NO_DISPLAY" << std::endl;
        return false;
    }

    std::cout << "Creating mpv render context..." << std::endl;

    // Create render context with OpenGL params
    // Note: mpv uses OpenGL (not OpenGL ES) but works with GLES contexts
    mpv_opengl_init_params gl_init_params;
    gl_init_params.get_proc_address = get_proc_address;
    gl_init_params.get_proc_address_ctx = nullptr;

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    int err = mpv_render_context_create(&render_context_, mpv, params);
    if (err < 0) {
        std::cerr << "Failed to create mpv render context: " << mpv_error_string(err) << std::endl;
        return false;
    }
    
    std::cout << "MPV render context created successfully" << std::endl;

    // Set update callback
    mpv_render_context_set_update_callback(render_context_, on_update, update_callback_ctx_);

    return true;
}

void MpvRenderer::set_viewport_size(uint32_t width, uint32_t height) {
    if (width > 0 && height > 0) {
        width_ = width;
        height_ = height;
        std::cout << "MPV render target size set to: " << width << "x" << height << std::endl;
        
        // Note: The FBO size in render() tells mpv the rendering surface size
        // mpv will automatically letterbox/pillarbox based on aspect ratio settings
        // No need to set width/height properties (they're read-only and report video resolution)
    }
}

void MpvRenderer::set_mpv_handle(mpv_handle* mpv) {
    mpv_handle_ = mpv;
}

uint64_t MpvRenderer::get_update_flags() const {
    if (!render_context_) {
        return 0;
    }
    return mpv_render_context_update(render_context_);
}

void MpvRenderer::render() {
    if (!render_context_ || width_ == 0 || height_ == 0) {
        return;
    }

    uint64_t flags = mpv_render_context_update(render_context_);
    
    // Only render when UPDATE_FRAME is set - this prevents unnecessary render calls
    // and improves performance
    if (!(flags & MPV_RENDER_UPDATE_FRAME)) {
        return;  // No new frame available, skip rendering
    }

    // Set viewport to full screen (for 4:3 CRT native mode)
    glViewport(0, 0, width_, height_);

    // Don't clear - mpv handles rendering completely and will cover the entire screen
    // Clearing can cause flickering, especially during video playback
    // mpv's render will fill the entire framebuffer with the video frame

    // Render mpv frame to fill entire screen
    mpv_opengl_fbo fbo;
    fbo.fbo = 0;  // Default framebuffer (0 = default)
    fbo.w = static_cast<int>(width_);
    fbo.h = static_cast<int>(height_);
    fbo.internal_format = 0;  // 0 = use default format

    int flip_y = 1;  // Flip Y for OpenGL ES

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    int err = mpv_render_context_render(render_context_, params);
    
    // Only log errors (not successes) to reduce console spam
    if (err < 0) {
        static int error_count = 0;
        if (error_count < 5) {
            std::cerr << "MPV render ERROR: " << mpv_error_string(err) << " (code=" << err << ")" << std::endl;
            error_count++;
        }
    }

    needs_redraw_ = false;
}

void MpvRenderer::on_update(void* ctx) {
    MpvRenderer* self = static_cast<MpvRenderer*>(ctx);
    self->needs_redraw_ = true;
}

void MpvRenderer::cleanup() {
    if (render_context_) {
        mpv_render_context_set_update_callback(render_context_, nullptr, nullptr);
        mpv_render_context_free(render_context_);
        render_context_ = nullptr;
    }
}

} // namespace video

