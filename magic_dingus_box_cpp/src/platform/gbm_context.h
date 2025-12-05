#pragma once

#include <cstdint>

struct gbm_device;
struct gbm_surface;

namespace platform {

class GbmContext {
public:
    GbmContext();
    ~GbmContext();

    // Initialize GBM device from DRM fd
    bool initialize(int drm_fd, uint32_t width, uint32_t height);
    
    // Get GBM surface (for EGL)
    gbm_surface* get_surface() const { return surface_; }
    
    // Get GBM device
    gbm_device* get_device() const { return device_; }
    
    // Get width/height
    uint32_t get_width() const { return width_; }
    uint32_t get_height() const { return height_; }
    
    // Cleanup
    void cleanup();

private:
    gbm_device* device_;
    gbm_surface* surface_;
    uint32_t width_;
    uint32_t height_;
};

} // namespace platform

