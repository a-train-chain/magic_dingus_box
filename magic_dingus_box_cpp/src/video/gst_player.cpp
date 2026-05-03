#include "gst_player.h"
#include "gst_helpers.h"
#include "../utils/logger.h"
#include <iostream>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <chrono>

namespace video {

GstPlayer::GstPlayer()
    : pipeline_(nullptr)
    , playbin_(nullptr)
    , appsink_(nullptr)
    , initialized_(false)
    , is_playing_(false)
    , is_paused_(false)
    , duration_(0.0)
    , position_(0.0)
    , decoder_inspect_frames_(0)
{
}

GstPlayer::~GstPlayer() {
    cleanup();
}

bool GstPlayer::initialize(const std::string& /*hwdec*/) {
    if (initialized_) return true;

    LOG_DEBUG("Initializing GStreamer...");

    // Initialize GStreamer with RAII error handling
    GError* init_error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &init_error)) {
        GErrorPtr error(init_error);  // RAII cleanup
        LOG_ERROR("Failed to initialize GStreamer: {}", error ? error->message : "unknown error");
        return false;
    }

    // Helper to promote hardware decoders
    auto promote_decoder = [](const char* name, const char* friendly) {
        GstRegistry* registry = gst_registry_get();
        GstPluginFeature* feature = gst_registry_lookup_feature(registry, name);
        if (feature) {
            gst_plugin_feature_set_rank(feature, GST_RANK_PRIMARY + 100);
            LOG_DEBUG("Promoted decoder: {} ({})", friendly, name);
            gst_object_unref(feature);
        } else {
            LOG_DEBUG("Decoder not found: {} ({})", friendly, name);
        }
    };

    // Helper to disable a decoder by setting its rank to NONE so
    // playbin skips it during auto-plugging. Used for known-broken
    // hardware decoders that advertise support but fail at negotiation.
    auto disable_decoder = [](const char* name, const char* reason) {
        GstRegistry* registry = gst_registry_get();
        GstPluginFeature* feature = gst_registry_lookup_feature(registry, name);
        if (feature) {
            gst_plugin_feature_set_rank(feature, GST_RANK_NONE);
            LOG_INFO("Disabled decoder '{}' ({})", name, reason);
            gst_object_unref(feature);
        }
    };

    // Prefer hardware accelerated decoders when available
    promote_decoder("v4l2h264dec", "V4L2 H.264");
    promote_decoder("v4l2h265dec", "V4L2 H.265");
    promote_decoder("v4l2vp8dec", "V4L2 VP8");
    promote_decoder("v4l2vp9dec", "V4L2 VP9");

    // Pi 4 hardware HEVC decode exists and works — the VideoCore VI
    // has a dedicated HEVC block (Main/Main10 up to 4096x4096) and
    // FFmpeg's `-hwaccel drm` path uses it successfully (~16x realtime
    // on 1080p content). VLC, Kodi, and mpv all reach it.
    //
    // GStreamer cannot. The Pi's `rpivid` kernel driver only outputs
    // proprietary tiled pixel formats — `NC12` (8-bit) and `NC30`
    // (10-bit), Broadcom's "SAND" layout — and mainline GStreamer's
    // v4l2codecs element doesn't understand them. When playbin auto-
    // plugs v4l2slh265dec, the driver responds with NC12, downstream
    // elements report "format UNKNOWN," and the negotiation fails.
    // Symptoms: "Driver does not support the selected stream" /
    // "Unsupported pixel format" / "GstPlayer::play() - state change
    // to PLAYING failed."
    //
    // The fix is in flight upstream:
    //   - GStreamer MR gstreamer/gstreamer#9247 adds SAND format
    //     support to v4l2codecs. Targets GStreamer 1.26 in Debian
    //     Trixie. Will NOT be backported to 1.22 in Bookworm.
    //   - Confirmed by GStreamer maintainer Nicolas Dufresne (Feb 2026).
    //   - See raspberrypi/linux#7137 for our specific failure mode.
    //
    // Until that lands and the kiosk is on a Trixie-based image, we
    // can't reach hardware HEVC through GStreamer at all — there's
    // no dtoverlay or kernel config that solves it for our pipeline.
    // Disabling v4l2slh265dec here forces playbin to fall back to
    // avdec_h265 (libav, software-decoded). For our typical content
    // (720p/1080p 8-bit Main profile at ~3-5 Mbps) software decode
    // uses ~30-50% of one A72 core — sustainable. The frame-rate
    // limiter in main.cpp drops kiosk render to 30 FPS during MB
    // movie playback, which gives the libav decode threads more
    // headroom on the shared 4 cores.
    //
    // Reaching hardware HEVC would require shelling out to FFmpeg
    // (LibreELEC's path) and piping decoded frames into our existing
    // GL renderer — significant refactor for a feature that becomes
    // free when GStreamer 1.26 lands. Not worth the complexity.
    disable_decoder("v4l2slh265dec",
                    "Pi 4 V4L2 stateless HEVC: SAND-format mismatch with mainline GStreamer "
                    "(MR gstreamer/gstreamer#9247 pending, Trixie-only)");

    // Create playbin with RAII wrapper
    auto playbin = make_element("playbin", "playbin");
    if (!playbin) {
        LOG_ERROR("Failed to create playbin element");
        return false;
    }

    // Configure audio sink (playbin takes ownership)
    auto audio_sink = make_element("pulsesink", "audio-sink");
    if (audio_sink) {
        // Set specific PulseAudio device if configured (bypasses default sink)
        if (!pulse_device_.empty()) {
            g_object_set(G_OBJECT(audio_sink.get()), "device", pulse_device_.c_str(), nullptr);
            LOG_DEBUG("Set pulsesink device to: {}", pulse_device_);
        }
        g_object_set(G_OBJECT(playbin.get()), "audio-sink", audio_sink.release(), nullptr);
        LOG_DEBUG("Configured pulsesink for audio");
    } else {
        LOG_WARN("Failed to create pulsesink, trying autoaudiosink");
        audio_sink = make_element("autoaudiosink", "audio-sink");
        if (audio_sink) {
            g_object_set(G_OBJECT(playbin.get()), "audio-sink", audio_sink.release(), nullptr);
        }
    }

    // Create video sink bin with RAII wrapper
    auto video_sink_bin = make_bin("video-sink-bin");
    if (!video_sink_bin) {
        LOG_ERROR("Failed to create video sink bin");
        return false;
    }

    // Create videoconvert element
    auto videoconvert = make_element("videoconvert", "videoconvert");
    if (!videoconvert) {
        LOG_ERROR("Failed to create videoconvert");
        return false;
    }

    // Create appsink element
    auto appsink = make_element("appsink", "video-sink");
    if (!appsink) {
        LOG_ERROR("Failed to create appsink");
        return false;
    }

    // Store raw pointers before transferring ownership to bin
    GstElement* videoconvert_raw = videoconvert.get();
    GstElement* appsink_raw = appsink.get();

    // Add elements to the sink bin (bin takes ownership)
    gst_bin_add_many(GST_BIN(video_sink_bin.get()),
                     videoconvert.release(),
                     appsink.release(),
                     nullptr);

    // Link videoconvert to appsink
    if (!gst_element_link(videoconvert_raw, appsink_raw)) {
        LOG_ERROR("Failed to link videoconvert to appsink");
        return false;  // video_sink_bin will clean up its children
    }

    // Create ghost pad for the sink bin
    auto sink_pad = get_static_pad(videoconvert_raw, "sink");
    if (!sink_pad) {
        LOG_ERROR("Failed to get sink pad from videoconvert");
        return false;
    }
    gst_element_add_pad(video_sink_bin.get(), gst_ghost_pad_new("sink", sink_pad.get()));
    // sink_pad is auto-released by RAII

    // Configure appsink caps - prefer YUV for performance, fallback to RGBA
    GstCaps* caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "I420",
        nullptr);
    gst_caps_append(caps, gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "NV12",
        nullptr));
    gst_caps_append(caps, gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "RGBA",
        nullptr));

    g_object_set(G_OBJECT(appsink_raw), "caps", caps, nullptr);
    gst_caps_unref(caps);

    // Set appsink properties.
    //
    // max-buffers tuning (1 → 5): the appsink is the leaf consumer of
    // the GStreamer pipeline; its buffer queue acts as a small reservoir
    // between the v4l2h264dec decoder thread and the kiosk's render
    // loop. With max-buffers=1 the decoder had to produce in lockstep
    // with the renderer — any render-thread preemption (a Docker
    // container waking up, a poster-cache GL upload, a window-manager
    // tick on a debug build) stalled the decoder for one frame interval
    // and surfaced as visible stutter. Bumping to 5 gives ~200 ms of
    // absorbing capacity at 24 fps; well below human-perceptible
    // latency for movie playback (no interactive feedback to be off-by)
    // but enough to smooth over typical scheduling jitter on the Pi 4B.
    //
    // drop=TRUE retained: if the renderer falls behind by MORE than
    // 200 ms (catastrophic stall, not normal jitter), drop the oldest
    // frames rather than backpressure the decoder into stalling. Pairs
    // with the systemd Nice=-10 + cgroup priority — together those
    // make >200 ms preemption near-impossible in practice.
    //
    // sync=TRUE retained: clock-synced rendering so audio + video stay
    // in lockstep. Without this, the appsink would deliver frames as
    // fast as the decoder produced them and audio would drift.
    g_object_set(G_OBJECT(appsink_raw),
        "emit-signals", TRUE,
        "sync", TRUE,
        "max-buffers", 5,
        "drop", TRUE,
        nullptr);

    // playbin buffer-size / buffer-duration tuning. These properties
    // only take effect when the source URI is a streaming protocol
    // (HTTP/RTSP/etc.) — playbin then inserts a queue2 between the
    // source and the demuxer, sized by these. For local file:// URIs
    // they're inert (the file source is naturally low-latency). Set
    // here as future-proofing for any later streaming-source path:
    //   - 16 MB byte budget (default -1 = "use queue2 default" which
    //     varies but is typically 2 MB — too small for sustained
    //     1080p over flaky residential links)
    //   - 5 s duration budget (default -1 = unlimited; capping at 5s
    //     bounds memory growth on long streams)
    constexpr gint kPlaybinBufferBytes = 16 * 1024 * 1024;     // 16 MB
    constexpr gint64 kPlaybinBufferNs  = 5LL * GST_SECOND;     // 5 s
    g_object_set(G_OBJECT(playbin.get()),
        "buffer-size",     kPlaybinBufferBytes,
        "buffer-duration", kPlaybinBufferNs,
        nullptr);

    // Set the sink bin as playbin's video-sink (playbin takes ownership)
    g_object_set(G_OBJECT(playbin.get()), "video-sink", video_sink_bin.release(), nullptr);

    // No gst_bus_add_watch — that requires a running GLib main loop, which
    // the kiosk doesn't have. Bus messages (EOS / ERROR / state-changed /
    // duration-changed / QoS) are drained from update_state() each frame
    // via gst_bus_pop, then dispatched through bus_call(). See update_state()
    // for the polling site and the rationale block there.

    // Transfer ownership to class members
    playbin_ = playbin.release();
    pipeline_ = playbin_;  // pipeline acts as the playbin
    appsink_ = appsink_raw;

    initialized_ = true;
    LOG_DEBUG("GStreamer initialization complete");
    return true;
}

gboolean GstPlayer::bus_call(GstBus* /*bus*/, GstMessage* msg, gpointer data) {
    GstPlayer* player = static_cast<GstPlayer*>(data);

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            LOG_DEBUG("End of stream");
            player->is_playing_ = false;
            player->is_paused_ = false;
            if (player->eos_callback_) {
                player->eos_callback_();
            }
            break;

        case GST_MESSAGE_ERROR: {
            gchar* debug = nullptr;
            GError* error = nullptr;
            gst_message_parse_error(msg, &error, &debug);
            GErrorPtr error_ptr(error);  // RAII cleanup
            LOG_ERROR("GStreamer Error: {}", error ? error->message : "unknown");
            if (debug) {
                LOG_DEBUG("GStreamer debug: {}", debug);
                g_free(debug);
            }
            player->is_playing_ = false;
            break;
        }

        case GST_MESSAGE_STATE_CHANGED: {
            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(player->pipeline_)) {
                LOG_DEBUG("Pipeline state: {} -> {}",
                          gst_element_state_get_name(old_state),
                          gst_element_state_get_name(new_state));

                if (new_state == GST_STATE_PLAYING) {
                    LOG_DEBUG("Pipeline entering PLAYING state");
                    player->is_playing_ = true;
                    player->is_paused_ = false;

                    // Inspect pipeline to see what decoder is used (only once per pipeline)
                    if (!player->decoder_inspected_) {
                        player->decoder_inspected_ = true;
                        GstIterator* it = gst_bin_iterate_recurse(GST_BIN(player->pipeline_));
                        GValue item = G_VALUE_INIT;
                        bool done = false;
                        while (!done) {
                            switch (gst_iterator_next(it, &item)) {
                                case GST_ITERATOR_OK: {
                                    GstElement* element = GST_ELEMENT(g_value_get_object(&item));
                                    gchar* name = gst_element_get_name(element);
                                    GstElementFactory* factory = gst_element_get_factory(element);

                                    if (factory) {
                                        const gchar* factory_name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
                                        if (factory_name && (strstr(factory_name, "dec") || strstr(factory_name, "avdec"))) {
                                            LOG_DEBUG("  Decoder: {} ({})", name, factory_name);
                                        }
                                    }

                                    g_free(name);
                                    g_value_reset(&item);
                                    break;
                                }
                                case GST_ITERATOR_RESYNC:
                                    gst_iterator_resync(it);
                                    break;
                                case GST_ITERATOR_ERROR:
                                case GST_ITERATOR_DONE:
                                    done = true;
                                    break;
                            }
                        }
                        gst_iterator_free(it);
                    }

                } else if (new_state == GST_STATE_PAUSED) {
                    player->is_paused_ = true;
                } else if (new_state == GST_STATE_READY || new_state == GST_STATE_NULL) {
                    player->is_playing_ = false;
                    player->is_paused_ = false;
                }
            }
            break;
        }
        
        case GST_MESSAGE_DURATION_CHANGED: {
            gint64 duration = 0;
            if (gst_element_query_duration(player->pipeline_, GST_FORMAT_TIME, &duration)) {
                player->duration_ = static_cast<double>(duration) / GST_SECOND;
            }
            break;
        }

        case GST_MESSAGE_QOS: {
            gint64 jitter;
            gdouble proportion;
            gint quality;
            GstFormat format;
            guint64 processed, dropped;
            (void)processed; // Unused

            gst_message_parse_qos_stats(msg, &format, &processed, &dropped);
            gst_message_parse_qos_values(msg, &jitter, &proportion, &quality);

            // Rate-limit QoS-drop warnings so file switches don't spam the
            // log. GStreamer emits one QOS message per "late" frame and each
            // one carries the RUNNING TOTAL of dropped frames in the
            // session, so without rate-limiting a playlist auto-advance
            // produces 5000-10000 log lines all in the same millisecond
            // (the new file's clock interprets every old timestamp as
            // "in the past" and dumps them as drops).
            //
            // Strategy:
            //   - Only log when the delta since the last logged value is
            //     >= 30 frames (~1 second @ 30 fps). Real cold-start
            //     drops accumulate slowly enough to surface; pipeline
            //     rebasing bursts collapse into 1-2 lines instead of
            //     thousands.
            //   - AND only log at most once per 500 ms wall clock. Belt
            //     and suspenders against any other GStreamer behavior
            //     that wants to emit messages at full pipeline tick rate.
            constexpr guint64 kDropDeltaThreshold = 30;
            constexpr int kMinIntervalMs = 500;
            const auto now = std::chrono::steady_clock::now();
            const auto since_last_ms = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                    now - player->last_qos_log_time_).count();
            const guint64 delta = (dropped > player->last_qos_logged_dropped_)
                ? (dropped - player->last_qos_logged_dropped_)
                : 0;
            if (delta >= kDropDeltaThreshold &&
                since_last_ms >= kMinIntervalMs) {
                LOG_WARN("QoS: Dropped frames: {} (+{} since last), Jitter: {}",
                         dropped, delta, jitter);
                player->last_qos_logged_dropped_ = dropped;
                player->last_qos_log_time_ = now;
            }
            break;
        }

        default:
            break;
    }
    return TRUE;
}

bool GstPlayer::load_file(const std::string& path, double start, double /*end*/, bool /*loop*/) {
    if (!initialized_) return false;

    stop();
    decoder_inspected_ = false;  // Re-inspect decoder for new media

    // Reset the QoS rate-limiter — the new file's pipeline starts with
    // a fresh dropped-frames counter (running total starts at 0), so
    // any "dropped" reading after this is genuinely from the new media.
    // Without this reset, a transient cold-start drop on the new file
    // would be hidden if the previous file's last-logged dropped count
    // was already higher.
    last_qos_logged_dropped_ = 0;
    last_qos_log_time_ = std::chrono::steady_clock::time_point{};

    std::string uri = "file://" + path;
    // Handle absolute paths
    if (path.find("://") != std::string::npos) {
        uri = path;
    } else if (path[0] != '/') {
        // Relative path - make absolute (GStreamer needs absolute URI usually)
        char* real_path = realpath(path.c_str(), nullptr);
        if (real_path) {
            uri = "file://" + std::string(real_path);
            free(real_path);
        }
    }

    LOG_DEBUG("GstPlayer::load_file() - setting URI: {}", uri);
    g_object_set(G_OBJECT(playbin_), "uri", uri.c_str(), nullptr);

    // Start playback
    play();

    LOG_DEBUG("GstPlayer::load_file - Current volume: {}%", get_volume());

    if (start > 0.0) {
        seek_absolute(start);
    }

    return true;
}

void GstPlayer::play() {
    if (!initialized_) return;

    LOG_DEBUG("GstPlayer::play() called - setting state to PLAYING");
    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    LOG_DEBUG("GstPlayer::play() state change return: {}", static_cast<int>(ret));

    // Non-blocking state check with 3-second timeout
    GstState current, pending;
    GstStateChangeReturn result = gst_element_get_state(pipeline_, &current, &pending, 3 * GST_SECOND);
    if (result == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("GstPlayer::play() - state change to PLAYING failed");
    }
    LOG_DEBUG("GstPlayer::play() current state: {}, pending: {}",
              gst_element_state_get_name(current),
              gst_element_state_get_name(pending));
}

void GstPlayer::pause() {
    if (!initialized_) return;
    gst_element_set_state(pipeline_, GST_STATE_PAUSED);
}

void GstPlayer::toggle_pause() {
    if (is_paused_) play();
    else pause();
}

void GstPlayer::seek(double seconds) {
    if (!initialized_) return;
    gint64 pos = 0;
    if (gst_element_query_position(pipeline_, GST_FORMAT_TIME, &pos)) {
        gint64 seek_pos = pos + static_cast<gint64>(seconds * GST_SECOND);
        if (seek_pos < 0) seek_pos = 0;
        // Clamp upper bound to (duration - 1s) so a seek can't push past
        // the end. Otherwise a high-velocity rotary scrub past the file's
        // last frame fires EOS, which our PlaybackScreen treats as
        // natural end-of-stream and returns to Detail — looks like a
        // crash to the user. Treat duration_ <= 0 as "unknown" and
        // skip the upper clamp in that case.
        double dur = duration_.load();
        if (dur > 1.0) {
            gint64 max_pos = static_cast<gint64>((dur - 1.0) * GST_SECOND);
            if (seek_pos > max_pos) seek_pos = max_pos;
        }
        // KEY_UNIT + direction-aware snap. Resolves the "+5s forward
        // scrub goes backward" bug differently from origin/main's
        // ACCURATE approach: rather than paying ACCURATE's
        // decode-from-previous-keyframe latency on every scrub
        // (visible as a ~200ms hitch on 1080p H.264), we keep KEY_UNIT
        // for the fast keyframe seek but tell GStreamer which
        // direction to snap. SNAP_NEAREST picked whichever keyframe
        // was visually closer to the target — on sparse-keyframe
        // 1080p that's often the keyframe just BEHIND the current
        // position, so a forward scrub appeared to go backward.
        //
        // SNAP_AFTER on positive seeks forces the snap to the next
        // keyframe forward; SNAP_BEFORE on negatives forces the
        // previous keyframe backward. Net effect is "scrub direction
        // matches input direction" — same correctness fix as the
        // ACCURATE approach, without the per-seek decode hitch.
        GstSeekFlags snap = (seconds >= 0)
            ? GST_SEEK_FLAG_SNAP_AFTER
            : GST_SEEK_FLAG_SNAP_BEFORE;
        gst_element_seek_simple(pipeline_, GST_FORMAT_TIME,
            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                       GST_SEEK_FLAG_KEY_UNIT |
                                       snap),
            seek_pos);
    }
}

void GstPlayer::seek_absolute(double timestamp) {
    if (!initialized_) return;
    gint64 seek_pos = static_cast<gint64>(timestamp * GST_SECOND);
    if (seek_pos < 0) seek_pos = 0;
    // Same upper-bound clamp as seek() — prevents over-shooting from
    // firing EOS (which PlaybackScreen treats as natural end-of-stream).
    double dur = duration_.load();
    if (dur > 1.0) {
        gint64 max_pos = static_cast<gint64>((dur - 1.0) * GST_SECOND);
        if (seek_pos > max_pos) seek_pos = max_pos;
    }
    // KEY_UNIT for fast keyframe seeks (matches the relative seek()
    // approach above; see that function's comment for the full
    // rationale on KEY_UNIT vs ACCURATE). Unlike relative seeks,
    // absolute seeks don't carry an intrinsic direction — they're
    // typically chapter / "go to position" operations from a UI
    // scrub, where the user already sees the target timestamp and
    // just wants the playhead to land there. SNAP_NEAREST is the
    // right default: pick whichever keyframe is visually closest to
    // the requested timestamp, regardless of slightly-before/after.
    gst_element_seek_simple(pipeline_, GST_FORMAT_TIME,
        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                   GST_SEEK_FLAG_KEY_UNIT |
                                   GST_SEEK_FLAG_SNAP_NEAREST),
        seek_pos);
}

void GstPlayer::stop() {
    if (!initialized_) return;

    // Set pipeline to NULL (stops playback and releases stream resources)
    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("Failed to set pipeline to NULL state in stop()");
    }

    // Wait for state change with timeout
    GstState current_state, pending_state;
    ret = gst_element_get_state(pipeline_, &current_state, &pending_state, GST_SECOND); // 1 second timeout
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_WARN("Pipeline did not reach NULL state within timeout");
    }

    // Force update of our internal state immediately
    is_playing_ = false;
    is_paused_ = false;
    position_ = 0.0;
    duration_ = 0.0;
}

bool GstPlayer::is_playing() const {
    return is_playing_;
}

bool GstPlayer::is_paused() const {
    return is_paused_;
}

double GstPlayer::get_position() const {
    if (!initialized_) return 0.0;
    gint64 pos = 0;
    if (gst_element_query_position(pipeline_, GST_FORMAT_TIME, &pos)) {
        return static_cast<double>(pos) / GST_SECOND;
    }
    // Position queries fail intermittently during seek/state transitions.
    // Returning 0 here would feed back into Controller::update_state and
    // flip state.video_active to false (since should_be_active checks
    // position > 0), causing the seek bar to disappear during scrubbing
    // and the renderer to skip frames. Fall back to the last cached
    // value (mirrors get_duration()'s behavior).
    return position_;
}

double GstPlayer::get_duration() const {
    if (!initialized_) return 0.0;
    gint64 dur = 0;
    if (gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &dur)) {
        return static_cast<double>(dur) / GST_SECOND;
    }
    return duration_;
}

void GstPlayer::set_volume(double volume) {
    if (!initialized_) return;
    // GStreamer volume is 0.0 to 1.0 (or more for boost)
    g_object_set(G_OBJECT(playbin_), "volume", volume / 100.0, nullptr);
}

double GstPlayer::get_volume() const {
    if (!initialized_) return 0.0;
    gdouble vol = 0.0;
    g_object_get(G_OBJECT(playbin_), "volume", &vol, nullptr);
    return vol * 100.0;
}

void GstPlayer::update_state() {
    if (!initialized_ || !pipeline_) return;

    // Drain the GStreamer bus. Historically we used gst_bus_add_watch, which
    // dispatches messages via the default GLib main context. The kiosk has
    // no g_main_loop_run on the render thread, so the watch never fired and
    // EOS / ERROR / state-changed / duration-changed messages were silently
    // swallowed — symptoms included intro EOS callbacks not firing on the
    // bus path (the controller compensated via position polling) and
    // mid-playback errors logging nothing. Polling here every frame on the
    // render thread delivers all the same messages without needing a main
    // loop. The bus_call() handler is unchanged; it's just dispatched from
    // a polling site now instead of GLib's watch system.
    GstBus* bus = gst_element_get_bus(pipeline_);
    if (bus) {
        GstMessage* msg;
        while ((msg = gst_bus_pop(bus)) != nullptr) {
            // bus_call returns gboolean for GLib main-loop convention; ignored here.
            (void)bus_call(bus, msg, this);
            gst_message_unref(msg);
        }
        gst_object_unref(bus);
    }

    // Poll current pipeline state (non-blocking to avoid stalling render loop)
    GstState current_state, pending_state;
    GstStateChangeReturn ret = gst_element_get_state(pipeline_, &current_state, &pending_state, 0);

    if (ret == GST_STATE_CHANGE_SUCCESS || ret == GST_STATE_CHANGE_NO_PREROLL || ret == GST_STATE_CHANGE_ASYNC) {
        // Update our cached state
        bool was_playing = is_playing_;
        bool now_playing = (current_state == GST_STATE_PLAYING);

        // Hold is_playing_ steady through seek transitions. A FLUSH seek
        // takes the pipeline PLAYING → PAUSED → PLAYING, and during the
        // brief PAUSED window this poll would report not-playing. With
        // ret=ASYNC and pending=PLAYING, GStreamer is telling us "I'm
        // mid-transition back to PLAYING" — treat that as still playing
        // so state.video_active doesn't flicker (which would hide the
        // seek bar and skip video render frames).
        if (!now_playing && ret == GST_STATE_CHANGE_ASYNC
            && pending_state == GST_STATE_PLAYING) {
            now_playing = true;
        }

        is_playing_ = now_playing;
        is_paused_ = (current_state == GST_STATE_PAUSED && !now_playing);

        // Log when playback starts (decoder inspection deferred to avoid blocking render loop)
        if (!was_playing && now_playing) {
            LOG_DEBUG("Pipeline now playing");
            decoder_inspect_frames_ = 30; // Inspect after ~30 frames to let decoders settle
        }

        // Deferred decoder inspection (non-blocking, runs after pipeline has settled)
        // Guarded by decoder_inspected_ to avoid duplicate inspection (bus_call also inspects)
        if (decoder_inspect_frames_ > 0 && !decoder_inspected_) {
            decoder_inspect_frames_--;
            if (decoder_inspect_frames_ == 0) {
                decoder_inspected_ = true;
                GstIterator* it = gst_bin_iterate_recurse(GST_BIN(pipeline_));
                GValue item = G_VALUE_INIT;
                bool done = false;
                while (!done) {
                    switch (gst_iterator_next(it, &item)) {
                        case GST_ITERATOR_OK: {
                            GstElement* element = GST_ELEMENT(g_value_get_object(&item));
                            gchar* name = gst_element_get_name(element);
                            GstElementFactory* factory = gst_element_get_factory(element);

                            if (factory) {
                                const gchar* factory_name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
                                if (factory_name && (strstr(factory_name, "dec") || strstr(factory_name, "avdec") ||
                                                   strstr(factory_name, "v4l2") || strstr(factory_name, "omx") ||
                                                   strstr(factory_name, "mmal"))) {
                                    LOG_DEBUG("  Decoder: {} ({})", name, factory_name);
                                }
                            }

                            g_free(name);
                            g_value_reset(&item);
                            break;
                        }
                        case GST_ITERATOR_RESYNC:
                            gst_iterator_resync(it);
                            break;
                        case GST_ITERATOR_ERROR:
                        case GST_ITERATOR_DONE:
                            done = true;
                            break;
                    }
                }
                gst_iterator_free(it);
            }
        }
    }

    // Update position/duration periodically
    update_position();
}

void GstPlayer::update_position() {
    if (!initialized_ || !pipeline_) return;

    // Update position and duration
    gint64 pos, dur;
    if (gst_element_query_position(pipeline_, GST_FORMAT_TIME, &pos)) {
        position_ = static_cast<double>(pos) / GST_SECOND;
    }
    if (gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &dur)) {
        duration_ = static_cast<double>(dur) / GST_SECOND;
    }
}

void GstPlayer::set_audio_device(const std::string& pulse_device) {
    pulse_device_ = pulse_device;

    // If pipeline is already running, update the pulsesink device property
    if (initialized_ && playbin_) {
        GstElement* audio_sink = nullptr;
        g_object_get(G_OBJECT(playbin_), "audio-sink", &audio_sink, nullptr);
        if (audio_sink) {
            g_object_set(G_OBJECT(audio_sink), "device", pulse_device.c_str(), nullptr);
            gst_object_unref(audio_sink);
            LOG_DEBUG("Updated pulsesink device to: {}", pulse_device);
        }
    }
}

void GstPlayer::cleanup() {
    // No bus watch to remove — bus messages are now drained via polling
    // in update_state(). When the render loop stops calling update_state(),
    // any remaining bus messages will be dropped with the bus when the
    // pipeline is set to GST_STATE_NULL below.
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        playbin_ = nullptr;
        appsink_ = nullptr;
    }
    initialized_ = false;
}

} // namespace video
