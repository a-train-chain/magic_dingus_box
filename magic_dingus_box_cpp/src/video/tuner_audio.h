#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace video {

// Render-thread input is the snow's start time, or null once the picture locks.
// Only the worker touches Pulse/GStreamer, including on a rapid second tune.
class TunerAudio {
public:
    using Clock = std::chrono::steady_clock;
    using Start = std::optional<Clock::time_point>;

    TunerAudio();
    ~TunerAudio();
    void update(Start start, int volume);

private:
    void run();
    Start last_start_;
    int last_volume_ = 0;
    std::mutex mutex_;
    std::condition_variable changed_;
    Start requested_;
    int volume_ = 0;
    unsigned revision_ = 0;
    bool quitting_ = false;
    std::thread worker_;
};

}  // namespace video
