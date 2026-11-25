#include "tuner_audio.h"
#include "app/tuner_snow.h"
#include "gst_helpers.h"
#include <gst/app/gstappsrc.h>

namespace video {

TunerAudio::TunerAudio() : worker_(&TunerAudio::run, this) {}

TunerAudio::~TunerAudio() {
    {
        std::lock_guard lock(mutex_);
        quitting_ = true;
    }
    changed_.notify_one();
    worker_.join();
}

void TunerAudio::update(Start start, int volume) {
    using namespace app::tuner_snow;
    if (start && (volume <= 0 || Clock::now() >= *start + duration)) start.reset();
    if (!start) volume = 0;
    if (start == last_start_ && volume == last_volume_) return;
    last_start_ = start;
    last_volume_ = volume;
    {
        std::lock_guard lock(mutex_);
        requested_ = start;
        volume_ = volume;
        ++revision_;
    }
    changed_.notify_one();
}

namespace {

GstBuffer* remaining_snow(TunerAudio::Clock::time_point start, unsigned seed, int volume) {
    using namespace app::tuner_snow;
    constexpr int total = duration.count() * sample_rate / 1000;
    const double age = std::chrono::duration<double>(TunerAudio::Clock::now() - start).count();
    if (age >= std::chrono::duration<double>(duration).count()) return nullptr;
    const int first = std::clamp(static_cast<int>(age * sample_rate), 0, total);
    const int count = total - first;
    if (!count) return nullptr;
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, count * sizeof(int16_t), nullptr);
    GstMapInfo map{};
    if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        if (buffer) gst_buffer_unref(buffer);
        return nullptr;
    }
    write_audio(reinterpret_cast<int16_t*>(map.data), count, first, seed, volume);
    gst_buffer_unmap(buffer, &map);
    GST_BUFFER_PTS(buffer) = 0;
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(count, GST_SECOND, sample_rate);
    return buffer;
}

}  // namespace

void TunerAudio::run() {
    using namespace app::tuner_snow;
    if (!gst_is_initialized()) return;
    GError* error = nullptr;
    GstPtr<GstElement> pipe(gst_parse_launch(
        "appsrc name=snow is-live=true format=time caps=audio/x-raw,format=S16LE,rate=48000,channels=1,layout=interleaved "
        "! pulsesink client-name=ChannelTuner sync=false async=false buffer-time=10000 latency-time=5000 volume=1.0",
        &error));
    GErrorPtr parse_error(error);
    if (parse_error || !pipe) {
        g_warning("Tuner audio unavailable: %s", error ? error->message : "no pipeline");
        return;
    }
    GstPtr<GstElement> src(gst_bin_get_by_name(GST_BIN(pipe.get()), "snow"));
    if (!src) return;
    unsigned handled = 0;
    std::unique_lock lock(mutex_);
    while (!quitting_) {
        changed_.wait(lock, [&] { return quitting_ || handled != revision_; });
        if (quitting_) break;
        handled = revision_;
        if (!requested_) continue;
        const auto start = *requested_;
        const int gain = volume_;
        const auto end = start + duration;
        lock.unlock();
        const auto state = gst_element_set_state(pipe.get(), GST_STATE_PLAYING);
        GstBuffer* buffer = state == GST_STATE_CHANGE_FAILURE ? nullptr : remaining_snow(start, handled, gain);
        lock.lock();
        if (buffer) {
            if (quitting_ || handled != revision_) {
                gst_buffer_unref(buffer);
            } else {
                // The appsrc queue accepts one finite buffer without blocking.
                gst_app_src_push_buffer(GST_APP_SRC(src.get()), buffer); // takes ownership
                changed_.wait_until(lock, end, [&] { return quitting_ || handled != revision_; });
            }
        }
        lock.unlock();
        // Flush queued samples on lock/cancel and release the audio device
        // between tunes (also needed for the RetroArch handover).
        gst_element_set_state(pipe.get(), GST_STATE_NULL);
        lock.lock();
    }
}

}  // namespace video
