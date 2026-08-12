#pragma once

#include <string>
#include <optional>
#include <vector>

#include "launch_contract.h"

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
    // audio_output: 0=AUTO, 1=HDMI, 2=HEADPHONE (matches app::AudioOutput enum)
    bool launch_game(const GameLaunchInfo& game_info, int system_volume_percent = 100, float volume_offset_db = 0.0f, int audio_output = 0, const LaunchOptions& opts = LaunchOptions{});

    // Check if RetroArch is available
    bool is_available() const { return retroarch_available_; }

private:
    // Find RetroArch executable
    std::optional<std::string> find_retroarch();
    
    // Launch RetroArch in DRM/KMS mode
    bool launch_drm(const GameLaunchInfo& game_info, int system_volume_percent, float volume_offset_db, int audio_output, const LaunchOptions& opts);

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
