#include "drm_display.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <vector>

namespace platform {

DrmDisplay::DrmDisplay()
    : drm_fd_(-1)
    , connector_id_(0)
    , crtc_id_(0)
    , encoder_id_(0)
    , saved_crtc_id_(0)
    , saved_crtc_(nullptr)
{
    current_mode_.width = 0;
    current_mode_.height = 0;
    current_mode_.refresh = 0;
}

DrmDisplay::~DrmDisplay() {
    cleanup();
}

bool DrmDisplay::initialize(const std::string& device_path) {
    // Try all available DRM devices
    std::vector<std::string> device_paths;
    
    // Scan all available DRM devices first
    std::cout << "Scanning for DRM devices..." << std::endl;
    for (int i = 0; i < 4; i++) {
        std::string path = "/dev/dri/card" + std::to_string(i);
        if (access(path.c_str(), R_OK) == 0) {
            // Check if it's a real DRM device (not v3d which doesn't support mode setting)
            int test_fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
            if (test_fd >= 0) {
                drmVersion* ver = drmGetVersion(test_fd);
                if (ver) {
                    std::string name = ver->name;
                    std::cout << "  Found DRM device: " << path << " (driver: " << name << ")" << std::endl;
                    drmFreeVersion(ver);
                    close(test_fd);
                    // Skip v3d (VideoCore) - it doesn't support mode setting
                    if (name != "v3d") {
                        device_paths.push_back(path);
                        std::cout << "    -> Added to candidate list" << std::endl;
                    } else {
                        std::cout << "    -> Skipped (v3d doesn't support mode setting)" << std::endl;
                    }
                } else {
                    close(test_fd);
                }
            }
        }
    }
    
    // If specific path given and not "auto", try that first
    if (!device_path.empty() && device_path != "auto") {
        // Check if it's already in the list
        bool found = false;
        for (const auto& p : device_paths) {
            if (p == device_path) {
                found = true;
                break;
            }
        }
        if (!found) {
            device_paths.insert(device_paths.begin(), device_path);
        }
    }
    
    if (device_paths.empty()) {
        std::cerr << "No suitable DRM devices found" << std::endl;
        std::cerr << "Available devices:" << std::endl;
        for (int i = 0; i < 4; i++) {
            std::string path = "/dev/dri/card" + std::to_string(i);
            if (access(path.c_str(), R_OK) == 0) {
                std::cerr << "  " << path << " (exists)" << std::endl;
            }
        }
        return false;
    }
    
    std::cout << "Will try " << device_paths.size() << " DRM device(s)" << std::endl;

    for (const auto& path : device_paths) {
        std::cout << "Trying DRM device: " << path << std::endl;
        
        // Open DRM device
        drm_fd_ = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (drm_fd_ < 0) {
            std::cerr << "Failed to open DRM device: " << path << " (" << strerror(errno) << ")" << std::endl;
            continue;
        }

        // Set master (required for mode setting)
        if (drmSetMaster(drm_fd_) < 0) {
            std::cerr << "Failed to set DRM master for " << path << " (" << strerror(errno) << ")" << std::endl;
            close(drm_fd_);
            drm_fd_ = -1;
            continue;
        }

        // If we got here, device opened successfully
        std::cout << "Successfully opened DRM device: " << path << std::endl;
        break;
    }

    if (drm_fd_ < 0) {
        std::cerr << "Failed to open any DRM device" << std::endl;
        return false;
    }

    // Find connector and CRTC
    if (!find_connector()) {
        std::cerr << "Failed to find connector" << std::endl;
        cleanup();
        return false;
    }

    if (!find_crtc()) {
        std::cerr << "Failed to find CRTC" << std::endl;
        cleanup();
        return false;
    }

    return true;
}

bool DrmDisplay::set_mode(uint32_t width, uint32_t height) {
    if (drm_fd_ < 0 || connector_id_ == 0 || crtc_id_ == 0) {
        return false;
    }

    return set_connector_mode(width, height);
}

bool DrmDisplay::find_connector() {
    drmModeRes* resources = drmModeGetResources(drm_fd_);
    if (!resources) {
        std::cerr << "Failed to get DRM resources: " << strerror(errno) << std::endl;
        std::cerr << "DRM fd: " << drm_fd_ << std::endl;
        
        // Try to get more info about the device
        drmVersion* version = drmGetVersion(drm_fd_);
        if (version) {
            std::cout << "DRM version: " << version->version_major << "." 
                      << version->version_minor << "." << version->version_patchlevel << std::endl;
            std::cout << "DRM name: " << version->name << std::endl;
            drmFreeVersion(version);
        }
        return false;
    }

    std::cout << "Found " << resources->count_connectors << " connectors" << std::endl;

    // Find first connected connector
    for (int i = 0; i < resources->count_connectors; i++) {
        drmModeConnector* conn = drmModeGetConnector(drm_fd_, resources->connectors[i]);
        if (!conn) {
            continue;
        }

        std::cout << "Connector " << conn->connector_id << ": ";
        switch (conn->connection) {
            case DRM_MODE_CONNECTED:
                std::cout << "CONNECTED";
                break;
            case DRM_MODE_DISCONNECTED:
                std::cout << "DISCONNECTED";
                break;
            default:
                std::cout << "UNKNOWN/OTHER (" << static_cast<int>(conn->connection) << ")";
                break;
        }
        std::cout << " (" << conn->count_modes << " modes)" << std::endl;

        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            connector_id_ = conn->connector_id;
            encoder_id_ = conn->encoder_id;
            std::cout << "Using connector " << connector_id_ << std::endl;
            drmModeFreeConnector(conn);
            drmModeFreeResources(resources);
            return true;
        }

        drmModeFreeConnector(conn);
    }

    std::cerr << "No connected connector found" << std::endl;
    drmModeFreeResources(resources);
    return false;
}

bool DrmDisplay::find_crtc() {
    drmModeRes* resources = drmModeGetResources(drm_fd_);
    if (!resources) {
        std::cerr << "Failed to get DRM resources in find_crtc" << std::endl;
        return false;
    }

    // Find CRTC for our encoder
    drmModeEncoder* encoder = drmModeGetEncoder(drm_fd_, encoder_id_);
    if (!encoder) {
        std::cerr << "Failed to get encoder " << encoder_id_ << std::endl;
        drmModeFreeResources(resources);
        return false;
    }

    crtc_id_ = encoder->crtc_id;
    std::cout << "Encoder " << encoder_id_ << " uses CRTC " << crtc_id_ << std::endl;
    
    // If encoder doesn't have a CRTC, try to find an available one
    if (crtc_id_ == 0) {
        std::cout << "Encoder has no CRTC, searching for available CRTC..." << std::endl;
        for (int i = 0; i < resources->count_crtcs; i++) {
            uint32_t crtc_id = resources->crtcs[i];
            drmModeCrtc* crtc = drmModeGetCrtc(drm_fd_, crtc_id);
            if (crtc) {
                // Check if this CRTC can be used with our encoder
                uint32_t possible_crtcs = encoder->possible_crtcs;
                if (possible_crtcs & (1 << i)) {
                    crtc_id_ = crtc_id;
                    std::cout << "Found available CRTC: " << crtc_id_ << std::endl;
                    drmModeFreeCrtc(crtc);
                    break;
                }
                drmModeFreeCrtc(crtc);
            }
        }
    }
    
    drmModeFreeEncoder(encoder);
    drmModeFreeResources(resources);

    if (crtc_id_ == 0) {
        std::cerr << "No CRTC available for encoder" << std::endl;
        return false;
    }

    // Save current CRTC state for restoration
    saved_crtc_ = drmModeGetCrtc(drm_fd_, crtc_id_);
    if (saved_crtc_) {
        saved_crtc_id_ = crtc_id_;
        std::cout << "Saved current CRTC state" << std::endl;
    }

    return true;
}

bool DrmDisplay::set_connector_mode(uint32_t width, uint32_t height) {
    drmModeConnector* conn = drmModeGetConnector(drm_fd_, connector_id_);
    if (!conn) {
        return false;
    }

    drmModeModeInfo* mode_to_set = nullptr;
    bool auto_mode = (width == 0 || height == 0);

    // Find matching mode
    for (int i = 0; i < conn->count_modes; i++) {
        drmModeModeInfo* mode = &conn->modes[i];
        
        if (auto_mode) {
            // Use preferred mode or first mode
            if (mode->type & DRM_MODE_TYPE_PREFERRED) {
                mode_to_set = mode;
                break;
            }
            if (!mode_to_set) {
                mode_to_set = mode;
            }
        } else {
            // Match exact resolution
            if (mode->hdisplay == width && mode->vdisplay == height) {
                mode_to_set = mode;
                break;
            }
        }
    }

    if (!mode_to_set) {
        drmModeFreeConnector(conn);
        return false;
    }

    // Get framebuffer ID - use saved CRTC's buffer if available, otherwise 0
    uint32_t fb_id = 0;
    if (saved_crtc_ && saved_crtc_id_ == crtc_id_) {
        drmModeCrtc* crtc = static_cast<drmModeCrtc*>(saved_crtc_);
        fb_id = crtc->buffer_id;
        std::cout << "Using saved CRTC framebuffer ID: " << fb_id << std::endl;
    }
    
    std::cout << "Setting mode: " << mode_to_set->hdisplay << "x" << mode_to_set->vdisplay 
              << "@" << mode_to_set->vrefresh << "Hz (" << mode_to_set->name << ")" << std::endl;
    std::cout << "CRTC: " << crtc_id_ << ", Connector: " << connector_id_ << ", FB: " << fb_id << std::endl;
    
    // Set the mode (x, y are 0 for fullscreen)
    // Note: Some drivers require a valid framebuffer. If fb_id is 0 and it fails,
    // we might need to create a dummy framebuffer, but for now try without one first.
    int ret = drmModeSetCrtc(drm_fd_, crtc_id_, fb_id, 0, 0,
                            &connector_id_, 1, mode_to_set);
    
    if (ret < 0) {
        int err = errno;
        std::cerr << "Failed to set CRTC mode: " << strerror(err) << " (errno=" << err << ")" << std::endl;
        std::cerr << "CRTC ID: " << crtc_id_ << ", Connector ID: " << connector_id_ << ", FB ID: " << fb_id << std::endl;
        
        // If fb_id was 0 and it failed, try with the current CRTC's buffer if different
        if (fb_id == 0 && saved_crtc_) {
            drmModeCrtc* crtc = static_cast<drmModeCrtc*>(saved_crtc_);
            if (crtc->buffer_id != 0 && crtc->crtc_id != crtc_id_) {
                std::cout << "Retrying with CRTC " << crtc->crtc_id << "'s framebuffer: " << crtc->buffer_id << std::endl;
                ret = drmModeSetCrtc(drm_fd_, crtc_id_, crtc->buffer_id, 0, 0,
                                    &connector_id_, 1, mode_to_set);
                if (ret == 0) {
                    std::cout << "Successfully set display mode (with fallback FB)" << std::endl;
                } else {
                    std::cerr << "Retry also failed: " << strerror(errno) << std::endl;
                    drmModeFreeConnector(conn);
                    return false;
                }
            } else {
                drmModeFreeConnector(conn);
                return false;
            }
        } else {
            drmModeFreeConnector(conn);
            return false;
        }
    }

    // Store current mode info
    current_mode_.width = mode_to_set->hdisplay;
    current_mode_.height = mode_to_set->vdisplay;
    current_mode_.refresh = mode_to_set->vrefresh;
    current_mode_.name = mode_to_set->name;

    drmModeFreeConnector(conn);
    return true;
}

void DrmDisplay::restore_crtc() {
    if (saved_crtc_ && saved_crtc_id_ != 0) {
        drmModeCrtc* crtc = static_cast<drmModeCrtc*>(saved_crtc_);
        drmModeSetCrtc(drm_fd_, saved_crtc_id_,
                      crtc->buffer_id, crtc->x, crtc->y,
                      &connector_id_, 1, &crtc->mode);
        drmModeFreeCrtc(crtc);
        saved_crtc_ = nullptr;
    }
}

void DrmDisplay::cleanup(bool restore_mode) {
    if (drm_fd_ >= 0) {
        if (restore_mode) {
            restore_crtc();
        } else {
            // For RetroArch handoff: just drop master and close
            // Don't set CRTC mode - let RetroArch discover and set it fresh
            // Setting the mode here can leave the CRTC in an invalid state
            std::cout << "Releasing DRM for RetroArch (dropping master, letting RetroArch set mode)..." << std::endl;
        }
        drmDropMaster(drm_fd_);
        close(drm_fd_);
        drm_fd_ = -1;
    }
    connector_id_ = 0;
    crtc_id_ = 0;
    encoder_id_ = 0;
}

void DrmDisplay::release_master(bool disable_crtc) {
    if (drm_fd_ >= 0) {
        if (disable_crtc && crtc_id_ > 0) {
            std::cout << "Disabling CRTC before dropping master..." << std::endl;
            drmModeSetCrtc(drm_fd_, crtc_id_, 0, 0, 0, nullptr, 0, nullptr);
        }
        std::cout << "Dropping DRM master (keeping FD open)..." << std::endl;
        drmDropMaster(drm_fd_);
    }
}

bool DrmDisplay::acquire_master() {
    if (drm_fd_ >= 0) {
        std::cout << "Acquiring DRM master..." << std::endl;
        if (drmSetMaster(drm_fd_) < 0) {
            std::cerr << "Failed to acquire DRM master: " << strerror(errno) << std::endl;
            return false;
        }
        return true;
    }
    return false;
}

} // namespace platform

