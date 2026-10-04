#pragma once

// Short-lived memo for a GStreamer pipeline query (position / duration).
// Pure logic, header-only, unit-tested on the Mac (tests/video).
//
// One kiosk frame used to issue up to seven position/duration queries
// against the same pipeline: GstPlayer::update_position() (2), then
// Controller::update_state() (2), Controller::status_text() (2) and the
// playback-error policy (1). Each query walks the playbin to its sinks
// under element locks. GstPlayer::update_position() runs first every frame
// and always queries; the getters reuse that answer while it is younger
// than the TTL (half a 60 Hz frame), so the rest of the frame costs
// nothing and the next frame always re-queries. Seeks and stop()
// invalidate, so a getter never serves a pre-seek answer.
//
// Lock-free and safe to read from any thread: value and stamp are separate
// atomics, so a racing reader can at worst pair a fresh stamp with the
// previous value — a position a few ms old, which every caller tolerates.

#include <atomic>
#include <chrono>
#include <cstdint>

namespace video {

class QueryCache {
public:
    // Half a 60 Hz frame: never spans two frames' update_position().
    static constexpr std::int64_t kTtlNs = 8'000'000;

    static std::int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // True when a value stored at most kTtlNs before `now` is available.
    bool fresh(std::int64_t now) const {
        const std::int64_t stamp = stamp_ns_.load(std::memory_order_acquire);
        return stamp != kNever && now >= stamp && now - stamp < kTtlNs;
    }

    void store(std::int64_t now) {
        stamp_ns_.store(now, std::memory_order_release);
    }

    void invalidate() { stamp_ns_.store(kNever, std::memory_order_release); }

private:
    static constexpr std::int64_t kNever = INT64_MIN;
    std::atomic<std::int64_t> stamp_ns_{kNever};
};

}  // namespace video
