#pragma once

#include <string>
#include <optional>
#include <vector>

namespace retroarch {

struct GameLaunchInfo {
    std::string rom_path;
    std::string core_name;
    std::string overlay_path;
};

class RetroArchLauncher {
public:
    RetroArchLauncher();
    
    // Initialize - find RetroArch executable
    bool initialize();
    
    // Launch a game with RetroArch
    bool launch_game(const GameLaunchInfo& game_info, int system_volume_percent = 100);
    
    // Launch RetroArch Core Downloader
    bool open_core_downloader(int system_volume_percent = 100);
    
    // Check if RetroArch is available
    bool is_available() const { return retroarch_available_; }

private:
    // Find RetroArch executable
    std::optional<std::string> find_retroarch();
    
    // Launch RetroArch in DRM/KMS mode
    bool launch_drm(const GameLaunchInfo& game_info, int system_volume_percent);
    
    // Core downloader direct launch
    bool open_core_downloader_direct(int system_volume_percent);
    
    // Release controllers before launch
    void release_controllers();
    
    // Detect ALSA device for audio
    std::string detect_alsa_device();
    
    // Stop GStreamer and cleanup audio resources
    void stop_gstreamer_and_cleanup();

private:
    std::optional<std::string> retroarch_bin_;
    bool retroarch_available_;
};

} // namespace retroarch

