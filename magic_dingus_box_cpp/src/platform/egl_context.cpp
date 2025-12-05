#include "egl_context.h"

#include <gbm.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <iostream>
#include <cstring>

namespace platform {

EglContext::EglContext()
    : display_(EGL_NO_DISPLAY)
    , context_(EGL_NO_CONTEXT)
    , surface_(EGL_NO_SURFACE)
    , config_(nullptr)
    , major_version_(0)
    , minor_version_(0)
{
}

EglContext::~EglContext() {
    cleanup();
}

bool EglContext::initialize(gbm_device* gbm_dev, gbm_surface* gbm_surf) {
    gbm_surface_ = gbm_surf;
    if (!gbm_dev || !gbm_surf) {
        return false;
    }

    // Get EGL display from GBM device
    display_ = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(gbm_dev));
    if (display_ == EGL_NO_DISPLAY) {
        std::cerr << "Failed to get EGL display: error " << eglGetError() << std::endl;
        return false;
    }

    // Initialize EGL
    if (!eglInitialize(display_, &major_version_, &minor_version_)) {
        std::cerr << "Failed to initialize EGL" << std::endl;
        display_ = EGL_NO_DISPLAY;
        return false;
    }

    // Choose config
    if (!choose_config()) {
        std::cerr << "Failed to choose EGL config" << std::endl;
        cleanup();
        return false;
    }

    // Create EGL surface from GBM surface
    surface_ = eglCreateWindowSurface(display_, config_,
                                     reinterpret_cast<EGLNativeWindowType>(gbm_surf),
                                     nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        std::cerr << "Failed to create EGL surface: " << eglGetError() << std::endl;
        cleanup();
        return false;
    }

    // Create EGL context
    EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };

    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, context_attribs);
    if (context_ == EGL_NO_CONTEXT) {
        std::cerr << "Failed to create EGL context: " << eglGetError() << std::endl;
        cleanup();
        return false;
    }

    // Make current
    if (!make_current()) {
        std::cerr << "Failed to make EGL context current" << std::endl;
        cleanup();
        return false;
    }

    return true;
}

bool EglContext::choose_config() {
    EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };

    EGLint num_configs;
    if (!eglChooseConfig(display_, attribs, &config_, 1, &num_configs) || num_configs == 0) {
        return false;
    }

    return true;
}

bool EglContext::make_current() {
    if (display_ == EGL_NO_DISPLAY || context_ == EGL_NO_CONTEXT || surface_ == EGL_NO_SURFACE) {
        return false;
    }

    return eglMakeCurrent(display_, surface_, surface_, context_);
}

bool EglContext::swap_buffers() {
    if (surface_ == EGL_NO_SURFACE) {
        return false;
    }

    return eglSwapBuffers(display_, surface_);
}

void EglContext::cleanup() {
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        
        if (context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(display_, context_);
            context_ = EGL_NO_CONTEXT;
        }
        
        if (surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
        }
        
        eglTerminate(display_);
        display_ = EGL_NO_DISPLAY;
    }
}

} // namespace platform

