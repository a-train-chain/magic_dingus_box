#include "app/movie_quiet_mode.h"

namespace app {

MovieQuietMode::MovieQuietMode(Actions actions)
    : actions_(std::move(actions)), busy_(static_cast<bool>(actions_.recover)), worker_([this] { run(); }) {}

MovieQuietMode::~MovieQuietMode() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void MovieQuietMode::request_pause(Mode mode, Claimant claimant) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& claim = claims_[static_cast<size_t>(claimant)];
        bool first = true, stronger = true;
        for (const auto& c : claims_) if (c) { first = false; if (*c >= mode) stronger = false; }
        if (claim && *claim >= mode) return;
        claim = mode;
        if (first || stronger) queue_.push_back(Cmd{true, mode, {}});
    }
    cv_.notify_all();
}

void MovieQuietMode::request_pause(std::function<Mode()> selector, Claimant claimant) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& claim = claims_[static_cast<size_t>(claimant)];
        if (claim) return;
        // Unknown strength stays Trickle for arbitration: a subsequent
        // explicit FullPause must still enqueue an escalation.
        claim = Mode::Trickle;
        queue_.push_back(Cmd{true, Mode::FullPause, std::move(selector)});
    }
    cv_.notify_all();
}

void MovieQuietMode::request_resume(Claimant claimant) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& claim = claims_[static_cast<size_t>(claimant)];
        if (!claim) return;
        claim.reset();
        for (const auto& c : claims_) if (c) return;
        // Cancel all unstarted escalations. An already-started pause still
        // owns consent and must be undone in FIFO order.
        while (!queue_.empty() && queue_.back().pause) queue_.pop_back();
        if (applied_) queue_.push_back(Cmd{false, Mode::FullPause, {}});
    }
    cv_.notify_all();
}

bool MovieQuietMode::effective_full_pause() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (held_full_) return true;
    for (const auto& c : queue_) if (c.pause && (c.select_mode || c.mode == Mode::FullPause)) return true;
    return busy_full_;
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
    int recovery_attempts = actions_.recover ? 12 : 0;
    auto recover = [&] {
        busy_ = true; lock.unlock();
        bool done = false;
        try { done = actions_.recover(); } catch (...) {}
        lock.lock(); busy_ = false; cv_.notify_all();
        if (done) recovery_attempts = 0; else --recovery_attempts;
    };
    if (recovery_attempts) recover();
    while (true) {
        if (recovery_attempts && !applied_) {
            if (!cv_.wait_for(lock, std::chrono::seconds(10), [this] { return stop_ || !queue_.empty(); })) {
                recover(); continue;
            }
        } else cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (queue_.empty()) break;  // stop_ set and nothing pending
        const Cmd cmd = queue_.front();
        queue_.pop_front();
        busy_ = true;
        busy_full_ = cmd.pause && (cmd.select_mode || cmd.mode == Mode::FullPause);
        if (cmd.pause) applied_ = true;
        lock.unlock();
        // Actions are best-effort; an exception must neither kill the
        // process (std::terminate from a worker) nor wedge the queue.
        try {
            if (cmd.pause) {
                if (actions_.pause) {
                    const Mode mode = cmd.select_mode ? cmd.select_mode() : cmd.mode;
                    bool need_pause;
                    { std::lock_guard<std::mutex> guard(mutex_);
                      need_pause = !worker_paused_ || (mode == Mode::FullPause && !held_full_); }
                    const Consent c = need_pause ? actions_.pause(mode) : Consent{};
                    { std::lock_guard<std::mutex> guard(mutex_);
                      worker_paused_ = true; held_full_ |= mode == Mode::FullPause; }
                    // Escalation accumulates consent: the last release
                    // restores both trickle and full-pause changes.
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
        if (!cmd.pause) {
            // A failed restoration retains its durable action journal, but
            // must not make a later session think a pause is still applied.
            applied_ = false;
            worker_paused_ = false;
            held_full_ = false;
        }
        busy_ = false;
        busy_full_ = false;
        cv_.notify_all();
    }
}

}  // namespace app
