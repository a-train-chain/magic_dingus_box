#include "app/game_quiet_mode.h"

namespace app {

GameQuietMode::GameQuietMode(Actions actions)
    : actions_(std::move(actions)), worker_([this] { run(); }) {}

GameQuietMode::~GameQuietMode() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void GameQuietMode::request_pause() { set_desired(true); }
void GameQuietMode::request_resume() { set_desired(false); }

void GameQuietMode::set_desired(bool paused) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        desired_paused_ = paused;
    }
    cv_.notify_all();
}

void GameQuietMode::wait_until_idle() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return desired_paused_ == applied_paused_; });
}

void GameQuietMode::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        cv_.wait(lock, [this] {
            return stop_ || desired_paused_ != applied_paused_;
        });
        if (desired_paused_ == applied_paused_) {
            break;  // stop_ set and nothing pending
        }
        const bool target = desired_paused_;
        lock.unlock();
        if (target) {
            if (actions_.pause) actions_.pause();
        } else {
            if (actions_.resume) actions_.resume();
        }
        lock.lock();
        applied_paused_ = target;
        cv_.notify_all();
    }
}

}  // namespace app
