#include "app/movie_quiet_mode.h"

namespace app {

MovieQuietMode::MovieQuietMode(Actions actions)
    : actions_(std::move(actions)), worker_([this] { run(); }) {}

MovieQuietMode::~MovieQuietMode() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void MovieQuietMode::request_pause(Mode mode) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(Cmd{true, mode});
    }
    cv_.notify_all();
}

void MovieQuietMode::request_resume() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!queue_.empty() && queue_.back().pause) {
            // The pause this resume would undo has not started: cancel it.
            queue_.pop_back();
        } else {
            queue_.push_back(Cmd{false, Mode::FullPause});
        }
    }
    cv_.notify_all();
}

void MovieQuietMode::wait_until_idle() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return idle_locked(); });
}

bool MovieQuietMode::wait_until_idle_for(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] { return idle_locked(); });
}

void MovieQuietMode::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (queue_.empty()) break;  // stop_ set and nothing pending
        const Cmd cmd = queue_.front();
        queue_.pop_front();
        busy_ = true;
        lock.unlock();
        // Actions are best-effort; an exception must neither kill the
        // process (std::terminate from a worker) nor wedge the queue.
        try {
            if (cmd.pause) {
                if (actions_.pause) {
                    const Consent c = actions_.pause(cmd.mode);
                    // Two pauses without a resume between them (never
                    // expected) accumulate, so one resume undoes both.
                    held_.alt_limited     |= c.alt_limited;
                    held_.qbit_paused     |= c.qbit_paused;
                    held_.services_paused |= c.services_paused;
                }
            } else {
                const Consent c = held_;
                held_ = Consent{};
                if (c.any() && actions_.resume) actions_.resume(c);
            }
        } catch (...) {
        }
        lock.lock();
        busy_ = false;
        cv_.notify_all();
    }
}

}  // namespace app
