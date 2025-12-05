#include "gbm_context.h"

#include <gbm.h>
#include <iostream>

namespace platform {

GbmContext::GbmContext()
    : device_(nullptr)
    , surface_(nullptr)
    , width_(0)
    , height_(0)
{
}

GbmContext::~GbmContext() {
    cleanup();
}

bool GbmContext::initialize(int drm_fd, uint32_t width, uint32_t height) {
    if (drm_fd < 0) {
        return false;
    }

    // Create GBM device
    device_ = gbm_create_device(drm_fd);
    if (!device_) {
        std::cerr << "Failed to create GBM device" << std::endl;
        return false;
    }

    // Create GBM surface
    surface_ = gbm_surface_create(device_,
                                  width, height,
                                  GBM_FORMAT_XRGB8888,
                                  GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!surface_) {
        std::cerr << "Failed to create GBM surface" << std::endl;
        cleanup();
        return false;
    }

    width_ = width;
    height_ = height;

    return true;
}

void GbmContext::cleanup() {
    if (surface_) {
        gbm_surface_destroy(surface_);
        surface_ = nullptr;
    }
    if (device_) {
        gbm_device_destroy(device_);
        device_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
}

} // namespace platform

