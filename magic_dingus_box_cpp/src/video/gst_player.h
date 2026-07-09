#pragma once

#include "video_player.h"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <chrono>  // steady_clock::time_point (seek watchdog + qos log timers)

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

    // QoS warning rate-limiter state (v1.6.x). GStreamer emits one QOS
    // bus message per late frame, each carrying the running total. Without
    // rate-limiting, a playlist auto-advance produced 5000-10000 log lines
    // in a single millisecond. See bus_call() for the throttle logic.
    guint64 last_qos_logged_dropped_ = 0;
    std::chrono::steady_clock::time_point last_qos_log_time_{};

    // EOS callback
    std::function<void()> eos_callback_;

    // Stored PulseAudio device name for pipeline re-creation
    std::string pulse_device_;

    // ── Seek coalescing (scrub / fast-forward freeze fix) ───────────
    // Rapid scrub input fires FLUSH seeks faster than the Pi's
    // v4l2h264dec hardware decoder can complete each one. Flushing the
    // V4L2 decoder again while a prior flush is still draining its
    // capture-queue buffers deadlocks it → frozen video. We serialize
    // seeks: at most one FLUSH seek in flight at a time. While one is
    // in progress, further requests only update the pending target
    // (latest-wins); the ASYNC_DONE bus message fires the trailing
    // seek once the pipeline re-prerolls. All fields are render-thread-
    // only — seek()/seek_absolute() and the bus pump in update_state()
    // both run on the render thread, so no locking is required.
    bool seek_in_progress_ = false;
    bool has_pending_seek_ = false;
    gint64 pending_seek_target_ns_ = 0;
    GstSeekFlags pending_seek_snap_ = GST_SEEK_FLAG_SNAP_NEAREST;
    std::chrono::steady_clock::time_point seek_started_at_{};

    // Clamp + store the absolute target, then either fire immediately
    // (no seek in flight) or queue it behind the in-flight seek.
    void request_seek(gint64 target_ns, GstSeekFlags snap);
    // Issue the actual gst_element_seek_simple for pending_seek_target_ns_
    // and mark a seek in progress. Caller must have set the target/snap.
    void fire_seek();

    void update_position();
};

} // namespace video
