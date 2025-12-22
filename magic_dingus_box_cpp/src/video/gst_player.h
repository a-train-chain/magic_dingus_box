#pragma once

#include "video_player.h"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>

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

    // GStreamer specific
    GstElement* get_pipeline() const { return pipeline_; }
    GstElement* get_appsink() const { return appsink_; }

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
    
    // Bus watch
    guint bus_watch_id_;
    static gboolean bus_call(GstBus* bus, GstMessage* msg, gpointer data);
    
    void update_position();
};

} // namespace video
