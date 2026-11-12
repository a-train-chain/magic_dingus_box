#pragma once

// Moves the phone keyboard queue's file work off the render loop.
//
// The web admin appends one JSON line per phone keystroke to
// data/text_input_queue.jsonl under flock(LOCK_EX); the kiosk reads the
// file and truncates it under the same lock. That used to happen on the
// render loop, once per frame while anything was queued:
//   - a BLOCKING flock — the loop waited for as long as the web side held
//     the lock, and it holds it across a write + flush that can itself sit
//     in the kernel when the SD card is saturated;
//   - an ftruncate per drain — a journaled file write, the same class of
//     call that stalled kiosk_status.json for ~6 s at a time and got the
//     kiosk SIGABRTed by the watchdog (see status_writer.h).
//
// Now the loop only stats the file (cached, no I/O) and, when it is not
// empty, asks this worker to drain it; what the worker read is picked up
// by take() on a later frame, normally the next one. The file protocol is
// unchanged — lock, read, truncate, unlock — so the web side needs no
// change and cannot be starved (the worker blocks on the lock like any
// other holder; nothing spins).
//
// THREADING: request(), take() and wait_idle() are for the loop thread.

#include <functional>
#include <mutex>
#include <string>
#include <utility>

#include "../utils/latest_wins_worker.h"

namespace app {

class TextQueueDrainer {
public:
    // Locks, reads and truncates the queue at `path`; returns the bytes it
    // took ("" when there were none, or when any step failed — a failed
    // truncate must return "" so the same keystrokes are not delivered
    // twice). Runs on the worker thread.
    using DrainFn = std::function<std::string(const std::string& path)>;

    // Starts a drain of `path` unless one is already queued or running
    // (that one, or the request the next frame makes, covers it). Never
    // touches the file and never waits.
    void request(const std::string& path, const DrainFn& drain) {
        if (worker_.busy()) return;
        worker_.submit([this, path, drain] {
            std::string got = drain(path);
            if (!got.empty()) {
                std::lock_guard<std::mutex> lk(mu_);
                drained_ += got;
            }
            return true;
        });
    }

    // Everything drained since the last call, in arrival order (one worker,
    // one drain at a time). Empty when nothing arrived.
    std::string take() {
        std::lock_guard<std::mutex> lk(mu_);
        return std::exchange(drained_, std::string());
    }

    // Blocks until no drain is queued or running. Tests only.
    void wait_idle() { worker_.flush(); }

private:
    std::mutex mu_;        // guards drained_; never held across file I/O
    std::string drained_;
    // LAST member: destroyed (joined) first, while mu_ and drained_, which
    // its jobs use, are still alive.
    utils::LatestWinsWorker worker_;
};

}  // namespace app
