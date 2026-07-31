#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace media_browser::ui {

// Fire-and-track worker pool for screen background fetches. Each worker
// flips its done flag as its LAST act, so reap() joins only workers
// whose join is guaranteed instant — it never blocks the render thread.
// (This is the per-worker done-flag pattern ProwlarrClient adopted in
// the 2026-07-31 hardening, extracted for reuse.)
//
// Why it exists: Browse/Search held plain vector<std::thread> members
// joined only at destruction. Every finished page fetch, lookup, or
// library poll left its thread object — stack, kernel task — pinned for
// the PROCESS lifetime. A browsing session leaks one per fetch, on a
// kiosk that runs for months.
class WorkerPool {
public:
    template <typename Fn>
    void spawn(Fn&& fn) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        workers_.push_back(Worker{
            done,
            std::thread([done, f = std::forward<Fn>(fn)]() mutable {
                f();
                done->store(true, std::memory_order_release);
            })});
    }

    // Render-thread tick: join + drop every finished worker (instant).
    void reap() {
        for (auto it = workers_.begin(); it != workers_.end();) {
            if (it->done->load(std::memory_order_acquire)) {
                if (it->thread.joinable()) it->thread.join();
                it = workers_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Shutdown path: join everything, finished or not. Callers must have
    // invalidated the workers' publish targets first (generation bump)
    // exactly as they did with the raw vectors.
    void join_all() {
        for (auto& w : workers_) {
            if (w.thread.joinable()) w.thread.join();
        }
        workers_.clear();
    }

    std::size_t size() const { return workers_.size(); }

    ~WorkerPool() { join_all(); }

private:
    struct Worker {
        std::shared_ptr<std::atomic<bool>> done;
        std::thread thread;
    };
    std::vector<Worker> workers_;
};

}  // namespace media_browser::ui
