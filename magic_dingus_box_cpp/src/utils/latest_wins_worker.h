#pragma once

// One background thread that runs the LATEST job it was handed.
//
// For file I/O the render loop must never wait for. The loop pings the
// systemd watchdog (WatchdogSec=10), and with the SD card saturated by a
// Content Manager upload an ext4 create/rename/fsync/truncate was sampled
// at up to ~6 s per call (2026-10-26, 1 GB Pi 4B — five watchdog SIGABRTs
// in fifteen minutes from kiosk_status.json alone). The owner builds the
// bytes on its own thread, wraps the blocking part in a Job and submits
// it; a job the worker has not started yet is REPLACED by a newer one
// (latest wins — every user here writes a whole-file snapshot or does an
// idempotent drain, so only the newest request matters and a stalled card
// never builds a backlog).
//
// Users: app::StatusWriter (kiosk_status.json), app::SettingsPersistence
// (settings.json), app::TextQueueDrainer (the phone keyboard queue).
//
// THREADING: submit(), take_failure(), busy(), flush*() and the destructor
// belong to ONE owner thread. Jobs run on the worker thread, one at a
// time, in submission order.

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

namespace utils {

class LatestWinsWorker {
public:
    // The blocking work. Returns false on failure. An exception counts as
    // a failure (it must not take the kiosk down from a worker thread).
    using Job = std::function<bool()>;

    LatestWinsWorker() = default;

    // Runs the job in hand and the latest pending one, then stops: a
    // normal teardown still completes its final write. Unbounded — an
    // exit path that must not wait (main()'s _exit) calls flush_until()
    // first and never reaches this.
    ~LatestWinsWorker() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    LatestWinsWorker(const LatestWinsWorker&) = delete;
    LatestWinsWorker& operator=(const LatestWinsWorker&) = delete;

    // Queues `job`, replacing one the worker has not started. Never runs
    // it on the caller's thread and never waits for the job in flight.
    // The thread starts on the first submit (an owner that never submits
    // costs no thread). If it cannot be started the job runs inline — the
    // pre-worker behavior — rather than being dropped.
    void submit(Job job) {
        std::unique_lock<std::mutex> lk(mu_);
        if (!thread_.joinable()) {
            try {
                thread_ = std::thread([this] { loop(); });
            } catch (const std::system_error&) {
                lk.unlock();
                const bool ok = run(job);
                lk.lock();
                last_failed_ = !ok;
                return;
            }
        }
        pending_ = std::move(job);
        has_pending_ = true;
        lk.unlock();
        cv_.notify_all();
    }

    // True when the most recently COMPLETED job failed; clears the flag.
    // The owner decides what a retry means (resubmit the same bytes, or
    // build fresh ones).
    bool take_failure() {
        std::lock_guard<std::mutex> lk(mu_);
        const bool failed = last_failed_;
        last_failed_ = false;
        return failed;
    }

    // A job is queued or running.
    bool busy() const {
        std::lock_guard<std::mutex> lk(mu_);
        return has_pending_ || running_;
    }

    // Blocks until nothing is queued or running. Tests and teardown only —
    // NEVER the render loop.
    void flush() {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return !has_pending_ && !running_; });
    }

    // flush() with a deadline: false when a job is still queued or running
    // at `deadline` (a wedged card must not hold a shutdown past systemd's
    // stop timeout).
    bool flush_until(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_until(lk, deadline,
                              [this] { return !has_pending_ && !running_; });
    }

private:
    static bool run(const Job& job) {
        try {
            return job();
        } catch (...) {
            return false;
        }
    }

    void loop() {
        std::unique_lock<std::mutex> lk(mu_);
        for (;;) {
            cv_.wait(lk, [this] { return has_pending_ || stop_; });
            if (!has_pending_) break;  // stopping, and the last job is done
            Job job = std::move(pending_);
            pending_ = nullptr;
            has_pending_ = false;
            running_ = true;
            // The job runs WITHOUT the lock: it is the part that can take
            // seconds, and submit() must be able to replace pending_ the
            // whole time.
            lk.unlock();
            const bool ok = run(job);
            job = nullptr;  // release captures before reporting idle
            lk.lock();
            running_ = false;
            last_failed_ = !ok;
            cv_.notify_all();  // flush()
        }
    }

    mutable std::mutex mu_;
    std::condition_variable cv_;
    Job pending_;
    bool has_pending_ = false;
    bool running_ = false;
    bool last_failed_ = false;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace utils
