#pragma once

#include <cstdint>
#include <EGL/egl.h>

struct gbm_device;
struct gbm_surface;

namespace platform {

class EglContext {
public:
    EglContext();
    ~EglContext();

    // Initialize EGL from GBM device and surface
    bool initialize(gbm_device* gbm_dev, gbm_surface* gbm_surf);
    
    // Make context current
    bool make_current();
    
    // Swap buffers (returns GBM bo handle for page flip)
    bool swap_buffers();
    
    // Get the current GBM surface (for page flipping)
    gbm_surface* get_gbm_surface() const { return gbm_surface_; }
    
    // Get EGL display
    EGLDisplay get_display() const { return display_; }
    
    // Get EGL context
    EGLContext get_context() const { return context_; }
    
    // Get surface
    EGLSurface get_surface() const { return surface_; }
    
    // Get GL version info
    int get_major_version() const { return major_version_; }
    int get_minor_version() const { return minor_version_; }
    
    // Cleanup
    void cleanup();

private:
    EGLDisplay display_;
    EGLContext context_;
    EGLSurface surface_;
    EGLConfig config_;
    gbm_surface* gbm_surface_;
    int major_version_;
    int minor_version_;
    
    bool choose_config();
};

} // namespace platform

