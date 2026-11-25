#pragma once

#include "video_player.h"
#include "deferred_start.h"
#include "log_rate_limit.h"
#include "pipeline_control.h"
#include "query_cache.h"
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
    bool at_eos() const override { return at_eos_.load(); }
    bool has_error() const override { return has_error_.load(); }
    double get_position() const override;
    double get_duration() const override;
    
    // Temporarily silence only the program while a fullscreen channel tunes.
    void set_tuner_muted(bool muted);
    bool audio_only_stream() const { return audio_only_stream_; }
    void set_volume(double volume) override;
    double get_volume() const override;
    
    void cleanup() override;

    // Monotonic count of load_file() calls — a "which stream is this"
    // token. GstRenderer's StreamGate compares it across render ticks to
    // suppress drawing the PREVIOUS stream's leftover frame while the
    // new pipeline decodes its first one. 0 = nothing ever loaded.
    uint64_t stream_generation() const { return stream_generation_; }

    // The appsink can only hold the CURRENT stream's frames: the control
    // thread has taken the pipeline through NULL (which empties the appsink)
    // for the latest load_file(). Until then a sample in it is the previous
    // stream's, so GstRenderer must not pull one (it would be drawn as the
    // new stream's first frame).
    bool frames_current() const {
        return flushed_generation_.load() == stream_generation_;
    }

    // A stop/load/play/pause/seek handed to the control thread has not been
    // carried out yet (pipeline_control.h). update_state() leaves the
    // pipeline alone meanwhile; callers whose deadlines count from a load
    // (PlaylistPlayback's start bound) hold them while this is true, so
    // time spent waiting on the SD card is not the stream's time.
    bool pipeline_busy() const { return control_.busy(); }

    // One command has been in hand for kWedgedAfter (30 s): wedged, not slow.
    // main.cpp then stops pinging the systemd watchdog, so a deadlocked
    // decoder or a dead card still ends in a restart, as it did when these
    // calls blocked the render loop itself.
    bool pipeline_wedged() const { return control_.wedged(); }

    // The pipeline's current state, its pending one and the last state
    // change's return, e.g. "PAUSED -> PLAYING (ASYNC)" — read with a ZERO
    // timeout, so it never blocks the render thread — plus the control
    // thread's command in hand, if any. For the stall watchdog's log line
    // (PlaylistTransport::pipeline_state).
    std::string describe_state() const;

    // Set PulseAudio device on the audio sink (e.g. sink name)
    // Must be called after initialize(). Also stored for pipeline re-creation.
    void set_audio_device(const std::string& pulse_device);

    // GStreamer specific
    GstElement* get_pipeline() const { return pipeline_; }
    GstElement* get_appsink() const { return appsink_; }

    // EOS callback
    void set_eos_callback(std::function<void()> cb) { eos_callback_ = std::move(cb); }

    // Poll for state updates (call this regularly from main loop)
    void update_state() override;

private:
    GstElement* pipeline_;
    GstElement* playbin_; // We use playbin for simplicity
    GstElement* appsink_;
    
    bool initialized_;
    std::atomic<bool> is_playing_;
    std::atomic<bool> is_paused_;
    // Latched by bus_call on GST_MESSAGE_EOS; cleared by stop() (which
    // every load_file() runs through), by cleanup() (via forget_stream) and by fire_seek() (a FLUSH seek
    // out of EOS resumes playback). Needed because a pipeline REMAINS in
    // GST_STATE_PLAYING after posting EOS — without the latch,
    // update_state()'s state poll overwrote bus_call's is_playing_=false
    // within the same call and the player could never report "stopped"
    // after natural end-of-stream.
    std::atomic<bool> at_eos_{false};
    // Latched by bus_call on GST_MESSAGE_ERROR; cleared ONLY by stop()
    // (which every load_file() runs through) and cleanup() (via forget_stream). Unlike at_eos_ a seek does
    // not clear it — a failed pipeline stays failed until it is torn down.
    // Gates is_playing_ the same way at_eos_ does: an errored pipeline can
    // keep reporting GST_STATE_PLAYING, and the state poll used to flip
    // is_playing_ back to true on the very next frame, hiding the error
    // from every caller.
    std::atomic<bool> has_error_{false};
    std::atomic<double> duration_;
    std::atomic<double> position_;
    // The volume last asked for (percent). get_volume() answers from it:
    // reading playbin's "volume" asks pulsesink, which waits for a
    // PulseAudio round trip — on the render thread, at every video start.
    std::atomic<double> volume_pct_{100.0};
    std::atomic<bool> tuner_muted_{false};  // read by the worker before starting a load
    bool audio_only_stream_ = false;       // discovered at PLAYING, cleared when the stream changes
    // Written by the control thread once a load's NULL has run (see
    // frames_current()); stream_generation_ is the render thread's own.
    std::atomic<uint64_t> flushed_generation_{0};
    // The control thread's seek_simple() returned FALSE for the seek with
    // this serial: no ASYNC_DONE will come, so update_state() clears
    // seek_in_progress_ (see fire_seek()) — if it is still the seek in
    // flight. seek_serial_ (render thread) numbers every fire_seek() and is
    // bumped by forget_stream(), so a late rejection from the previous
    // stream never matches.
    std::atomic<uint64_t> rejected_seek_serial_{0};
    uint64_t seek_serial_ = 0;
    // Per-frame memo of the two pipeline queries (query_cache.h):
    // update_position() refreshes them once per frame, the getters reuse
    // that answer for the rest of the frame. Invalidated by seeks/stop.
    QueryCache position_query_;
    QueryCache duration_query_;
    
    // Bus message dispatcher. Called from update_state()'s polling drain
    // (NOT registered with gst_bus_add_watch — see gst_player.cpp::update_state
    // for the rationale on why we poll instead of using GLib watches).
    static gboolean bus_call(GstBus* bus, GstMessage* msg, gpointer data);

    // Deferred decoder inspection (counts down frames after playback starts)
    int decoder_inspect_frames_;
    bool decoder_inspected_ = false;  // Guard to run decoder inspection only once per pipeline
    uint64_t stream_generation_ = 0;  // Bumped by every load_file(); see stream_generation()

    // QoS warning rate-limiter state (v1.6.x). GStreamer emits one QOS
    // bus message per late frame, each carrying the running total. Without
    // rate-limiting, a playlist auto-advance produced 5000-10000 log lines
    // in a single millisecond. See bus_call() for the throttle logic.
    guint64 last_qos_logged_dropped_ = 0;
    std::chrono::steady_clock::time_point last_qos_log_time_{};

    // GStreamer WARNING messages are logged at WARN, at most one per source
    // element per 5 s (cleared per load_file), so the next frozen picture
    // names the element that complained without a chatty one flooding the
    // journal. See bus_call().
    PerSourceLogLimit warning_log_limit_{std::chrono::seconds(5)};

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

    // load_file(start > 0)'s seek, held until update_state()'s poll sees
    // the pipeline prerolled (render-thread-only, like the fields above).
    DeferredStart deferred_start_;

    // Clamp + store the absolute target, then either fire immediately
    // (no seek in flight) or queue it behind the in-flight seek.
    void request_seek(gint64 target_ns, GstSeekFlags snap);
    // Issue the actual gst_element_seek_simple for pending_seek_target_ns_
    // and mark a seek in progress. Caller must have set the target/snap.
    void fire_seek();

    void update_position();

    // The pipeline as an owned reference for a queued command.
    std::shared_ptr<void> pipeline_ref() const;
    // Runs on the control thread: the GStreamer half of each command.
    void execute(const PipelineCommand& cmd);
    static void apply_volume(double volume, const std::shared_ptr<void>& target);

    // Forget everything this player reports about a stream: playing /
    // paused / EOS / error, position, duration, their query memos, the seek
    // coalescer and the deferred start. stop() and cleanup() both end with
    // it, so a pipeline that is gone can never be described by them.
    // cleanup() used to skip it: is_playing() kept reading true through a
    // whole game session, and after the post-game initialize() the new,
    // empty playbin failed every query, so get_position()/get_duration()
    // served the torn-down stream's last values — Controller::update_state
    // then saw "duration 40.1, position 1.68" and raised video_active for a
    // stream that did not exist (Pi 5, 2026-11-13).
    void forget_stream();

    // Every pipeline state change, seek and volume change runs here, off
    // the render thread (pipeline_control.h). Declared LAST: destroyed
    // first, so its thread is joined while everything execute() touches
    // still exists.
    PipelineControl control_{
        [this](const PipelineCommand& cmd) { execute(cmd); },
        &GstPlayer::apply_volume};
};

} // namespace video
