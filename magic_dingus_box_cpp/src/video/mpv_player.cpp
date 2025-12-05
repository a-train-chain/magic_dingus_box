#include "mpv_player.h"

#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <iostream>
#include <cstring>
#include <cmath>
#include <thread>
#include <chrono>

namespace video {

MpvPlayer::MpvPlayer()
    : handle_(nullptr)
    , initialized_(false)
    , audio_recovery_count_(0)
{
}

MpvPlayer::~MpvPlayer() {
    cleanup();
}

bool MpvPlayer::initialize(const std::string& hwdec) {
    std::cout << "Initializing MPV player with software decoding..." << std::endl;

    handle_ = mpv_create();
    if (!handle_) {
        std::cerr << "Failed to create mpv handle" << std::endl;
        return false;
    }

    // Try with gpu video output and drm context - this should work with DRM
    mpv_set_option_string(handle_, "vo", "gpu");
    mpv_set_option_string(handle_, "gpu-context", "drm");
    mpv_set_option_string(handle_, "drm-device", "/dev/dri/card1");

    // Simple software decoding configuration
    mpv_set_option_string(handle_, "hwdec", "no");
    mpv_set_option_string(handle_, "vd-lavc-dr", "no");
    mpv_set_option_string(handle_, "vd-lavc-threads", "1");

    // BARE BONES: Simple ALSA audio configuration - minimal settings
    // Use ALSA only, no PulseAudio fallback
    // Set audio device as an option during initialization (before mpv_initialize)
    // This ensures MPV opens the correct device from the start
    mpv_set_option_string(handle_, "ao", "alsa");
    mpv_set_option_string(handle_, "audio-device", "alsa/plughw:CARD=vc4hdmi0,DEV=0");
    mpv_set_option_string(handle_, "audio", "yes");  // Explicitly enable audio
    mpv_set_option_string(handle_, "mute", "no");
    mpv_set_option_string(handle_, "audio-channels", "auto");
    // Set default volume to 100% so audio works from the start
    mpv_set_option_string(handle_, "volume", "100");

    // Playback options
    mpv_set_option_string(handle_, "video-sync", "audio");
    mpv_set_option_string(handle_, "keep-open", "no");
    mpv_set_option_string(handle_, "idle", "yes");
    mpv_set_option_string(handle_, "input-default-bindings", "no");
    mpv_set_option_string(handle_, "input-vo-keyboard", "no");
    mpv_set_option_string(handle_, "osc", "no");
    mpv_set_option_string(handle_, "pause", "no");
    
    // Enable verbose audio logging to diagnose issues
    // Use 'info' level for ao and audio to see what's happening
    mpv_set_option_string(handle_, "msg-level", "ao=info,audio=info,all=error");

    // Initialize
    if (mpv_initialize(handle_) < 0) {
        std::cerr << "Failed to initialize mpv" << std::endl;
        mpv_destroy(handle_);
        handle_ = nullptr;
        return false;
    }
    
    // After initialization, set properties that require initialized handle
    set_property("hwdec", "no");
    
    // Let MPV handle audio completely automatically
    
    // Force 4:3 aspect ratio with proper letterboxing (side margins on wide displays)
    // video-aspect-override forces the aspect ratio regardless of video file metadata
    set_property("video-aspect-override", "4:3");
    // video-aspect also set to 4:3 for consistency
    set_property("video-aspect", "4:3");
    // Reset zoom and panscan to ensure no stretching - video will fill height with side margins
    set_property("video-zoom", "0");
    set_property("panscan", "0");
    // Ensure video scales properly without distortion
    set_property("keepaspect", "yes");

    initialized_ = true;
    return true;
}

bool MpvPlayer::load_file(const std::string& path, double start, double end, bool loop) {
    if (!initialized_) {
        return false;
    }

    // Set loop
    set_loop(loop);
    
    // CRITICAL: Set audio device BEFORE loading file so MPV opens it correctly
    // This must be done before loadfile, otherwise MPV may open audio with default device
    std::cout << "Setting audio device before file load: plughw:CARD=vc4hdmi0,DEV=0" << std::endl;
    set_property("audio-device", "alsa/plughw:CARD=vc4hdmi0,DEV=0");
    set_property("audio", "yes");  // Ensure audio is enabled
    set_property("volume", "100");
    set_property("mute", "no");
    
    // Verify audio device was set
    std::string current_device = get_property("audio-device");
    std::cout << "Audio device after setting (before load): '" << current_device << "'" << std::endl;
    
    // Set video aspect ratio properties before loading
    set_property("video-aspect-override", "4:3");
    set_property("video-aspect", "4:3");
    set_property("keepaspect", "yes");
    set_property("video-zoom", "0");
    set_property("panscan", "0");

    // Ensure software decoding
    set_property("hwdec", "no");
    
    // Load file
    const char* cmd[] = {"loadfile", path.c_str(), "replace", nullptr};
    int err = mpv_command(handle_, cmd);
    if (err < 0) {
        check_error(err);
        return false;
    }
    
    // Let MPV handle audio automatically - don't set audio=yes as it's not a valid property
    
    // Wait a moment for MPV to process the file load
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Verify audio configuration
    std::string ao = get_property("ao");
    std::string audio_device_after = get_property("audio-device");
    std::string mute_status = get_property("mute");
    double vol_after = get_volume();
    std::cout << "Audio status after file load - ao='" << ao << "', device='" << audio_device_after 
              << "', mute='" << mute_status << "', volume=" << vol_after << "%" << std::endl;
    
    // Check if audio is actually active (ao-active property)
    try {
        std::string ao_active = get_property("ao-active");
        std::cout << "Audio output active: '" << ao_active << "'" << std::endl;
        if (ao_active != "yes") {
            std::cerr << "WARNING: Audio output is not active! ao-active='" << ao_active << "'" << std::endl;
        }
    } catch (...) {
        // ao-active might not be available immediately
    }
    
    // Re-apply video scaling properties after file load

    // Seek to start if specified
    if (start > 0.0) {
        seek_absolute(start);
    }
    
    // Re-apply scaling properties after file load to ensure they take effect
    // This ensures the video maintains 4:3 aspect ratio with letterboxing
    // Note: These properties will be applied when video starts playing
    set_property("video-aspect-override", "4:3");
    set_property("video-aspect", "4:3");
    set_property("keepaspect", "yes");
    set_property("video-zoom", "0");
    set_property("panscan", "0");

    // Set end time via A-B loop if specified
    if (end > start) {
        // Use A-B loop to approximate end time
        std::string ab_a = std::to_string(start);
        std::string ab_b = std::to_string(end);
        set_property("ab-loop-a", ab_a);
        set_property("ab-loop-b", ab_b);
    }

    return true;
}

bool MpvPlayer::load_file_with_hwdec(const std::string& path, const std::string& hwdec, double start, double end, bool loop) {
    if (!initialized_) {
        return false;
    }

    // Set loop
    set_loop(loop);

    // CRITICAL: Set audio device BEFORE loading file so MPV opens it correctly
    set_property("audio-device", "alsa/plughw:CARD=vc4hdmi0,DEV=0");
    set_property("volume", "100");
    set_property("mute", "no");

    // Ensure video scaling properties are set before loading file
    set_property("video-aspect-override", "4:3");
    set_property("video-aspect", "4:3");
    set_property("keepaspect", "yes");
    set_property("video-zoom", "0");
    set_property("panscan", "0");

    // Load file with specific hwdec option if provided
    if (!hwdec.empty()) {
        // Use loadfile with per-file hwdec option
        std::string options = "hwdec=" + hwdec;
        const char* cmd[] = {"loadfile", path.c_str(), options.c_str(), nullptr};
        int err = mpv_command(handle_, cmd);
        if (err < 0) {
            check_error(err);
            return false;
        }
        std::cout << "Loaded file with hwdec=" << hwdec << ": " << path << std::endl;
    } else {
        // Normal loadfile
        const char* cmd[] = {"loadfile", path.c_str(), "replace", nullptr};
        int err = mpv_command(handle_, cmd);
        if (err < 0) {
            check_error(err);
            return false;
        }
    }

    // Re-apply audio settings after file load to ensure they're still set
    set_property("audio-device", "alsa/plughw:CARD=vc4hdmi0,DEV=0");
    set_property("volume", "100");
    set_property("mute", "no");
    
    // Wait a moment for MPV to process the file load
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Verify audio configuration
    std::string ao = get_property("ao");
    std::string audio_device_after = get_property("audio-device");
    std::string mute_status = get_property("mute");
    double vol_after = get_volume();
    std::cout << "Audio status after file load (hwdec) - ao='" << ao << "', device='" << audio_device_after 
              << "', mute='" << mute_status << "', volume=" << vol_after << "%" << std::endl;
    
    // Check if audio is actually active
    try {
        std::string ao_active = get_property("ao-active");
        std::cout << "Audio output active (hwdec): '" << ao_active << "'" << std::endl;
    } catch (...) {
        // ao-active might not be available immediately
    }

    // Seek to start if specified
    if (start > 0.0) {
        seek_absolute(start);
    }

    // Set end time via A-B loop if specified
    if (end > start) {
        std::string ab_a = std::to_string(start);
        std::string ab_b = std::to_string(end);
        set_property("ab-loop-a", ab_a);
        set_property("ab-loop-b", ab_b);
    }

    return true;
}

void MpvPlayer::play() {
    set_property("pause", "no");
}

void MpvPlayer::pause() {
    set_property("pause", "yes");
}

void MpvPlayer::toggle_pause() {
    const char* cmd[] = {"cycle", "pause", nullptr};
    mpv_command(handle_, cmd);
}

void MpvPlayer::seek(double seconds) {
    const char* cmd[] = {"seek", std::to_string(seconds).c_str(), "relative", nullptr};
    mpv_command(handle_, cmd);
}

void MpvPlayer::seek_absolute(double timestamp) {
    const char* cmd[] = {"seek", std::to_string(timestamp).c_str(), "absolute", nullptr};
    mpv_command(handle_, cmd);
}

void MpvPlayer::stop() {
    // BARE BONES: Just stop playback - let MPV handle everything
    const char* cmd[] = {"stop", nullptr};
    mpv_command(handle_, cmd);
}

void MpvPlayer::set_loop(bool enabled) {
    set_property("loop-file", enabled ? "inf" : "no");
}

void MpvPlayer::set_property(const std::string& name, const std::string& value) {
    if (!initialized_) {
        return;
    }
    // Use mpv_set_property for runtime properties (set after initialization)
    // mpv_set_option_string is only for options set before initialization
    const char* val = value.c_str();
    int err = mpv_set_property(handle_, name.c_str(), MPV_FORMAT_STRING, &val);
    if (err < 0) {
        // Log error but don't fail - some properties might not be settable at certain times
        std::cerr << "Warning: Failed to set property " << name << "=" << value 
                  << ": " << mpv_error_string(err) << std::endl;
    }
}

std::string MpvPlayer::get_property(const std::string& name) {
    if (!initialized_) {
        return "";
    }
    
    char* value = mpv_get_property_string(handle_, name.c_str());
    if (!value) {
        return "";
    }
    
    std::string result(value);
    mpv_free(value);
    return result;
}

double MpvPlayer::get_property_double(const std::string& name) const {
    if (!initialized_) {
        return 0.0;
    }
    double value = 0.0;
    mpv_get_property(handle_, name.c_str(), MPV_FORMAT_DOUBLE, &value);
    return value;
}

bool MpvPlayer::is_playing() const {
    if (!initialized_) {
        return false;
    }
    std::string pause = const_cast<MpvPlayer*>(this)->get_property("pause");
    return pause != "yes";
}

bool MpvPlayer::is_paused() const {
    if (!initialized_) {
        return true;
    }
    std::string pause = const_cast<MpvPlayer*>(this)->get_property("pause");
    return pause == "yes";
}

double MpvPlayer::get_position() const {
    if (!initialized_) {
        return 0.0;
    }
    double pos = 0.0;
    mpv_get_property(handle_, "time-pos", MPV_FORMAT_DOUBLE, &pos);
    return pos;
}

double MpvPlayer::get_duration() const {
    if (!initialized_) {
        return 0.0;
    }
    double dur = 0.0;
    mpv_get_property(handle_, "duration", MPV_FORMAT_DOUBLE, &dur);
    return dur;
}

void MpvPlayer::set_volume(double volume) {
    if (!initialized_) {
        return;
    }
    // Clamp volume to 0-100 range
    if (volume < 0.0) volume = 0.0;
    if (volume > 100.0) volume = 100.0;
    set_property("volume", std::to_string(volume));
}

double MpvPlayer::get_volume() const {
    if (!initialized_) {
        return 0.0;
    }
    double vol = 100.0;  // Default to 100%
    mpv_get_property(handle_, "volume", MPV_FORMAT_DOUBLE, &vol);
    return vol;
}


bool MpvPlayer::validate_audio_device(const std::string& device) {
    if (!initialized_) {
        return false;
    }
    try {
        std::string device_list = get_property("audio-device-list");
        // Check if device exists in the list
        return device_list.find(device) != std::string::npos;
    } catch (...) {
        // If we can't check, assume it's valid (fallback behavior)
        return true;
    }
}

void MpvPlayer::reconfigure_audio_device() {
    // BARE BONES: Stub - do nothing, let MPV handle audio
    // Removed complex reconfiguration logic that was causing issues
}

void MpvPlayer::check_and_recover_audio() {
    // BARE BONES: Stub - do nothing, let MPV handle audio
    // Removed complex recovery logic that was causing issues
}

void MpvPlayer::check_error(int status) {
    if (status < 0) {
        std::cerr << "mpv error: " << mpv_error_string(status) << std::endl;
    }
}

void MpvPlayer::cleanup() {
    if (handle_) {
        mpv_terminate_destroy(handle_);
        handle_ = nullptr;
    }
    initialized_ = false;
    audio_recovery_count_ = 0;  // Reset recovery counter on cleanup
}

} // namespace video

