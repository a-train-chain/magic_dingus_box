#pragma once

#include <string>
#include <cstdint>
#include <memory>

struct mpv_handle;
struct mpv_render_context;

namespace video {

class MpvPlayer {
public:
    MpvPlayer();
    ~MpvPlayer();

    // Initialize mpv with software decoding to avoid buffer issues
    bool initialize(const std::string& hwdec = "no");
    
    // Load a file for playback
    bool load_file(const std::string& path, double start = 0.0, double end = 0.0, bool loop = false);
    bool load_file_with_hwdec(const std::string& path, const std::string& hwdec, double start = 0.0, double end = 0.0, bool loop = false);

    // Playback control
    void play();
    void pause();
    void toggle_pause();
    void seek(double seconds);
    void seek_absolute(double timestamp);
    void stop();

    // Loop control
    void set_loop(bool enabled);

    // Properties
    void set_property(const std::string& name, const std::string& value);
    std::string get_property(const std::string& name);
    double get_property_double(const std::string& name) const;
    
    // State queries
    bool is_playing() const;
    bool is_paused() const;
    double get_position() const;
    double get_duration() const;
    
    // Volume control
    void set_volume(double volume);  // 0.0 to 100.0
    double get_volume() const;
    
    // Get mpv handle (for render context creation)
    mpv_handle* get_handle() const { return handle_; }

    // Audio device management
    void reconfigure_audio_device();
    void check_and_recover_audio();  // Detect and recover from audio issues

    // Cleanup
    void cleanup();

private:
    mpv_handle* handle_;
    bool initialized_;
    int audio_recovery_count_;  // Track recovery attempts to prevent excessive reconfiguration
    
    bool validate_audio_device(const std::string& device);
    void check_error(int status);
};

} // namespace video

