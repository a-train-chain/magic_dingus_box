#pragma once

#include "video_player.h"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>

namespace video {

class GstPlayer : public VideoPlayer {
public:
    GstPlayer();
    ~GstPlayer() override;

    bool is_initialized() const { return initialized_; }

    bool initialize(const std::string& hwdec = "no") override;
    bool load_file(const std::string& path, double start = 0.0, double end = 0.0, bool loop = false) override;
    
    void play() override;
    void pause() override;
    void toggle_pause() override;
    void seek(double seconds) override;
    void seek_absolute(double timestamp) override;
    void stop() override;
    
    bool is_playing() const override;
    bool is_paused() const override;
    double get_position() const override;
    double get_duration() const override;
    
    void set_volume(double volume) override;
    double get_volume() const override;
    
    void cleanup() override;

    // Set PulseAudio device on the audio sink (e.g. sink name)
    // Must be called after initialize(). Also stored for pipeline re-creation.
    void set_audio_device(const std::string& pulse_device);

    // GStreamer specific
    GstElement* get_pipeline() const { return pipeline_; }
    GstElement* get_appsink() const { return appsink_; }

    // EOS callback
    void set_eos_callback(std::function<void()> cb) { eos_callback_ = std::move(cb); }

    // Poll for state updates (call this regularly from main loop)
    void update_state();

private:
    GstElement* pipeline_;
    GstElement* playbin_; // We use playbin for simplicity
    GstElement* appsink_;
    
    bool initialized_;
    std::atomic<bool> is_playing_;
    std::atomic<bool> is_paused_;
    std::atomic<double> duration_;
    std::atomic<double> position_;
    
    // Bus message dispatcher. Called from update_state()'s polling drain
    // (NOT registered with gst_bus_add_watch — see gst_player.cpp::update_state
    // for the rationale on why we poll instead of using GLib watches).
    static gboolean bus_call(GstBus* bus, GstMessage* msg, gpointer data);

    // Deferred decoder inspection (counts down frames after playback starts)
    int decoder_inspect_frames_;
    bool decoder_inspected_ = false;  // Guard to run decoder inspection only once per pipeline

    // EOS callback
    std::function<void()> eos_callback_;

    // Stored PulseAudio device name for pipeline re-creation
    std::string pulse_device_;

    void update_position();
};

} // namespace video
