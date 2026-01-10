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
    , bus_watch_id_(0)
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

    // Prefer hardware accelerated decoders when available
    promote_decoder("v4l2h264dec", "V4L2 H.264");
    promote_decoder("v4l2h265dec", "V4L2 H.265");
    promote_decoder("v4l2vp8dec", "V4L2 VP8");
    promote_decoder("v4l2vp9dec", "V4L2 VP9");

    // Create playbin with RAII wrapper
    auto playbin = make_element("playbin", "playbin");
    if (!playbin) {
        LOG_ERROR("Failed to create playbin element");
        return false;
    }

    // Configure audio sink (playbin takes ownership)
    auto audio_sink = make_element("pulsesink", "audio-sink");
    if (audio_sink) {
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

    // Set appsink properties
    g_object_set(G_OBJECT(appsink_raw),
        "emit-signals", TRUE,
        "sync", TRUE,
        "max-buffers", 1,
        "drop", TRUE,
        nullptr);

    // Set the sink bin as playbin's video-sink (playbin takes ownership)
    g_object_set(G_OBJECT(playbin.get()), "video-sink", video_sink_bin.release(), nullptr);

    // Add bus watch
    auto bus = get_bus(playbin.get());
    if (bus) {
        bus_watch_id_ = gst_bus_add_watch(bus.get(), bus_call, this);
        // bus is auto-released by RAII
    }

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
            // Handle EOS (e.g. auto loop or stop)
            player->is_playing_ = false;
            player->is_paused_ = false;
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

                    // Inspect pipeline to see what decoder is used
                    GstIterator* it = gst_bin_iterate_recurse(GST_BIN(player->pipeline_));
                    GValue item = G_VALUE_INIT;
                    bool done = false;
                    while (!done) {
                        switch (gst_iterator_next(it, &item)) {
                            case GST_ITERATOR_OK: {
                                GstElement* element = GST_ELEMENT(g_value_get_object(&item));
                                gchar* name = gst_element_get_name(element);
                                GstElementFactory* factory = gst_element_get_factory(element);

                                // Check if it looks like a decoder
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

            if (dropped > 0) {
                LOG_WARN("QoS: Dropped frames: {}, Jitter: {}", dropped, jitter);
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

    // Check current state after a short delay
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    GstState current, pending;
    gst_element_get_state(pipeline_, &current, &pending, GST_CLOCK_TIME_NONE);
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
        gst_element_seek_simple(pipeline_, GST_FORMAT_TIME, static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), seek_pos);
    }
}

void GstPlayer::seek_absolute(double timestamp) {
    if (!initialized_) return;
    gint64 seek_pos = static_cast<gint64>(timestamp * GST_SECOND);
    gst_element_seek_simple(pipeline_, GST_FORMAT_TIME, static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), seek_pos);
}

void GstPlayer::stop() {
    if (!initialized_) return;

    // Try to send EOS event first to gracefully stop playback
    if (playbin_) {
        gst_element_send_event(playbin_, gst_event_new_eos());
    }

    // Wait a bit for EOS to be processed
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Force state change to NULL
    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("Failed to set pipeline to NULL state in stop()");
    }

    // Wait for state change with timeout
    GstState current_state, pending_state;
    ret = gst_element_get_state(pipeline_, &current_state, &pending_state, 1000000000); // 1 second timeout
    // We don't log success/failure here to avoid spam

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
    return 0.0;
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

    // Poll current pipeline state
    GstState current_state, pending_state;
    GstStateChangeReturn ret = gst_element_get_state(pipeline_, &current_state, &pending_state, GST_CLOCK_TIME_NONE);

    if (ret == GST_STATE_CHANGE_SUCCESS || ret == GST_STATE_CHANGE_NO_PREROLL) {
        // Update our cached state
        bool was_playing = is_playing_;
        bool now_playing = (current_state == GST_STATE_PLAYING);

        is_playing_ = now_playing;
        is_paused_ = (current_state == GST_STATE_PAUSED);

        // If we just started playing, inspect the pipeline for decoders
        if (!was_playing && now_playing) {
            LOG_DEBUG("Pipeline now playing! Inspecting elements...");

            // Give pipeline a moment to fully initialize decoders
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            // Inspect pipeline to see what decoder is used
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
                                LOG_DEBUG("  Found decoder element: {} (Factory: {})", name, factory_name);
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

void GstPlayer::cleanup() {
    if (bus_watch_id_ > 0) {
        g_source_remove(bus_watch_id_);
        bus_watch_id_ = 0;
    }
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
