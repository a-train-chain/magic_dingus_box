#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct drm_mode_modeinfo;

namespace platform {

struct DisplayMode {
    uint32_t width;
    uint32_t height;
    uint32_t refresh;  // Hz * 1000
    std::string name;
};

class DrmDisplay {
public:
    DrmDisplay();
    ~DrmDisplay();

    // Initialize DRM device, find connector, set mode
    bool initialize(const std::string& device_path = "/dev/dri/card0");
    
    // Set specific resolution (e.g., 720x480) or auto-detect
    bool set_mode(uint32_t width = 0, uint32_t height = 0);
    
    // Get current mode
    DisplayMode get_current_mode() const { return current_mode_; }
    
    // Get DRM file descriptor (for GBM)
    int get_fd() const { return drm_fd_; }
    
    // Get connector and CRTC IDs
    uint32_t get_connector_id() const { return connector_id_; }
    uint32_t get_crtc_id() const { return crtc_id_; }
    
    // Cleanup
    // restore_mode: if true, restores CRTC to saved mode; if false, leaves current mode intact
    void cleanup(bool restore_mode = true);

    // Release DRM master without closing FD (for RetroArch handoff)
    // disable_crtc: if true, turns off the display signal before dropping master
    void release_master(bool disable_crtc = false);
    
    // Re-acquire DRM master
    bool acquire_master();

private:
    int drm_fd_;
    uint32_t connector_id_;
    uint32_t crtc_id_;
    uint32_t encoder_id_;
    uint32_t saved_crtc_id_;
    void* saved_crtc_;
    DisplayMode current_mode_;
    
    bool find_connector();
    bool find_crtc();
    bool set_connector_mode(uint32_t width, uint32_t height);
    void restore_crtc();
};

} // namespace platform

