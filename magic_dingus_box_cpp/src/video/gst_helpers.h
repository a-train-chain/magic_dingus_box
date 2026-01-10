#pragma once

#include <gst/gst.h>
#include <memory>
#include <utility>

namespace video {

/**
 * RAII wrapper for GStreamer objects (GstElement, GstCaps, GstBus, etc.)
 * Automatically calls gst_object_unref when the object goes out of scope.
 *
 * Usage:
 *   GstPtr<GstElement> element = make_element("videoconvert", "convert");
 *   if (!element) return false;  // Creation failed
 *
 *   // Use element.get() to access the raw pointer
 *   gst_bin_add(GST_BIN(bin), element.get());
 *
 *   // Transfer ownership to GStreamer (e.g., when adding to a bin)
 *   gst_bin_add(GST_BIN(bin), element.release());
 */

// Custom deleter for GstObject-derived types
struct GstObjectDeleter {
    void operator()(gpointer obj) const {
        if (obj) {
            gst_object_unref(obj);
        }
    }
};

// Custom deleter for GstCaps (not a GstObject, uses different unref)
struct GstCapsDeleter {
    void operator()(GstCaps* caps) const {
        if (caps) {
            gst_caps_unref(caps);
        }
    }
};

// Custom deleter for GError
struct GErrorDeleter {
    void operator()(GError* error) const {
        if (error) {
            g_error_free(error);
        }
    }
};

// Smart pointer type for GstElement and other GstObject-derived types
template<typename T>
using GstPtr = std::unique_ptr<T, GstObjectDeleter>;

// Smart pointer type for GstCaps
using GstCapsPtr = std::unique_ptr<GstCaps, GstCapsDeleter>;

// Smart pointer type for GError
using GErrorPtr = std::unique_ptr<GError, GErrorDeleter>;

/**
 * Create a GStreamer element with RAII wrapper.
 *
 * @param factory The factory name (e.g., "videoconvert", "appsink")
 * @param name The element name (can be nullptr for auto-generated name)
 * @return GstPtr<GstElement> that owns the element, or nullptr if creation failed
 */
inline GstPtr<GstElement> make_element(const char* factory, const char* name = nullptr) {
    return GstPtr<GstElement>(gst_element_factory_make(factory, name));
}

/**
 * Create a GStreamer bin with RAII wrapper.
 *
 * @param name The bin name
 * @return GstPtr<GstElement> that owns the bin, or nullptr if creation failed
 */
inline GstPtr<GstElement> make_bin(const char* name) {
    return GstPtr<GstElement>(gst_bin_new(name));
}

/**
 * Create a GStreamer pipeline with RAII wrapper.
 *
 * @param name The pipeline name
 * @return GstPtr<GstElement> that owns the pipeline, or nullptr if creation failed
 */
inline GstPtr<GstElement> make_pipeline(const char* name) {
    return GstPtr<GstElement>(gst_pipeline_new(name));
}

/**
 * Create simple GstCaps with RAII wrapper.
 *
 * @param media_type The media type (e.g., "video/x-raw")
 * @return GstCapsPtr that owns the caps
 */
inline GstCapsPtr make_caps_simple(const char* media_type) {
    return GstCapsPtr(gst_caps_new_empty_simple(media_type));
}

/**
 * Get bus from pipeline with RAII wrapper.
 *
 * @param pipeline The pipeline element
 * @return GstPtr<GstBus> that owns the bus reference
 */
inline GstPtr<GstBus> get_bus(GstElement* pipeline) {
    return GstPtr<GstBus>(gst_pipeline_get_bus(GST_PIPELINE(pipeline)));
}

/**
 * Get static pad from element with RAII wrapper.
 *
 * @param element The element
 * @param name The pad name
 * @return GstPtr<GstPad> that owns the pad reference
 */
inline GstPtr<GstPad> get_static_pad(GstElement* element, const char* name) {
    return GstPtr<GstPad>(gst_element_get_static_pad(element, name));
}

/**
 * RAII guard for GStreamer initialization.
 * Ensures gst_deinit() is called on destruction if needed.
 * Note: In most cases, GStreamer doesn't need explicit deinit,
 * but this can be useful for testing or clean shutdown scenarios.
 */
class GstInitGuard {
public:
    GstInitGuard() : initialized_(false) {}

    bool init(GError** error = nullptr) {
        if (!initialized_) {
            initialized_ = gst_init_check(nullptr, nullptr, error);
        }
        return initialized_;
    }

    bool is_initialized() const { return initialized_; }

    // Note: gst_deinit() is generally not recommended as it can cause issues
    // with plugins. Only use if you know what you're doing.
    // ~GstInitGuard() { if (initialized_) gst_deinit(); }

private:
    bool initialized_;
};

} // namespace video
