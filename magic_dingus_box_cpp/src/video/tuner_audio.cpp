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

using Samples = std::chrono::duration<int64_t, std::ratio<1, app::tuner_snow::sample_rate>>;

GstBuffer* snow_chunk(app::tuner_snow::ReceiverNoise& noise, Samples offset,
                      int count, int first, int volume) {
    using namespace app::tuner_snow;
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, count * sizeof(int16_t), nullptr);
    GstMapInfo map{};
    if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        if (buffer) gst_buffer_unref(buffer);
        return nullptr;
    }
    noise.write(reinterpret_cast<int16_t*>(map.data), count, first, volume);
    gst_buffer_unmap(buffer, &map);
    GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(offset.count(), GST_SECOND, sample_rate);
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
        auto end = *requested_ + duration;
        ReceiverNoise noise(handled);
        lock.unlock();
        const auto state = gst_element_set_state(pipe.get(), GST_STATE_PLAYING);
        const auto opened = Clock::now();
        Samples sent{0};
        lock.lock();
        while (state != GST_STATE_CHANGE_FAILURE && !quitting_ && requested_ && *requested_ <= end) {
            handled = revision_;
            end = *requested_ + duration;
            const auto now = Clock::now();
            if (now >= end) break;
            // A few short buffers cover scheduler jitter, but leave the tail
            // editable when another detent arrives. Only the worker waits.
            constexpr auto ahead = std::chrono::milliseconds(30);
            const auto queued_until = opened + std::chrono::duration_cast<Clock::duration>(sent);
            const auto available = std::chrono::duration_cast<Samples>(end - queued_until);
            if (available.count() < 1 || queued_until >= now + ahead) {
                const auto wake = available.count() < 1 ? end : queued_until - ahead;
                changed_.wait_until(lock, wake, [&] { return quitting_ || handled != revision_; });
                continue;
            }
            sent = std::max(sent, std::chrono::duration_cast<Samples>(now - opened));
            const auto position = opened + std::chrono::duration_cast<Clock::duration>(sent);
            const int count = static_cast<int>(std::min<Samples>(
                Samples{sample_rate / 100}, std::chrono::duration_cast<Samples>(end - position)).count());
            if (count <= 0) break;
            const int first = static_cast<int>(std::chrono::duration_cast<Samples>(position - *requested_).count());
            const int gain = volume_;
            lock.unlock();
            GstBuffer* buffer = snow_chunk(noise, sent, count, first, gain);
            const bool pushed = buffer &&
                gst_app_src_push_buffer(GST_APP_SRC(src.get()), buffer) == GST_FLOW_OK;
            lock.lock();
            if (!pushed) break;
            sent += Samples{count};
        }
        lock.unlock();
        // Flush queued samples on lock/cancel and release the audio device
        // between tunes (also needed for the RetroArch handover).
        gst_element_set_state(pipe.get(), GST_STATE_NULL);
        lock.lock();
    }
}

}  // namespace video
