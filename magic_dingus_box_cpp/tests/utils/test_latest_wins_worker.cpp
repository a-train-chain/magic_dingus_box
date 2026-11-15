// utils::LatestWinsWorker — the one background thread behind every file
// write the render loop must not wait for (kiosk_status.json,
// settings.json, the phone keyboard queue).
//
// The loop pings the systemd watchdog (WatchdogSec=10). These pin the
// contract that keeps a saturated SD card off it: submit() returns at
// once whatever the job in flight is doing, a newer job replaces one that
// has not started, a failure is reported to the owner, and teardown /
// flush complete the final job.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "utils/latest_wins_worker.h"

using utils::LatestWinsWorker;
using namespace std::chrono_literals;

namespace {

// A gate a job can be parked on, standing in for a card stuck behind a
// writeback queue.
struct Gate {
    std::mutex mu;
    std::condition_variable cv;
    bool open = false;
    bool entered = false;

    void wait_in_job() {
        std::unique_lock<std::mutex> lk(mu);
        entered = true;
        cv.notify_all();
        cv.wait(lk, [this] { return open; });
    }
    void wait_entered() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [this] { return entered; });
    }
    void release() {
        std::lock_guard<std::mutex> lk(mu);
        open = true;
        cv.notify_all();
    }
};

}  // namespace

TEST_CASE("latest_wins_worker: submit never waits for a stalled job, and "
          "only the newest pending job runs",
          "[utils][latest_wins]") {
    Gate gate;
    std::mutex mu;
    std::vector<int> ran;
    LatestWinsWorker w;

    w.submit([&] {
        gate.wait_in_job();
        std::lock_guard<std::mutex> lk(mu);
        ran.push_back(1);
        return true;
    });
    gate.wait_entered();  // the worker is now stuck "on the card"

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 2; i <= 50; ++i) {
        w.submit([&, i] {
            std::lock_guard<std::mutex> lk(mu);
            ran.push_back(i);
            return true;
        });
    }
    CHECK(std::chrono::steady_clock::now() - t0 < 500ms);
    CHECK(w.busy());

    // A bounded flush gives up while the job is stuck — it must not hold a
    // shutdown hostage.
    CHECK_FALSE(w.flush_until(std::chrono::steady_clock::now() + 20ms));

    gate.release();
    w.flush();
    CHECK_FALSE(w.busy());
    // The job in flight, then ONLY the newest — not a 49-job backlog.
    REQUIRE(ran == std::vector<int>{1, 50});
    CHECK(w.flush_until(std::chrono::steady_clock::now() + 1s));
}

TEST_CASE("latest_wins_worker: a failure is reported once, and a later "
          "success clears it",
          "[utils][latest_wins]") {
    LatestWinsWorker w;
    CHECK_FALSE(w.take_failure());  // nothing ran yet

    w.submit([] { return false; });
    w.flush();
    CHECK(w.take_failure());
    CHECK_FALSE(w.take_failure());  // consumed

    w.submit([] { return false; });
    w.flush();
    w.submit([] { return true; });
    w.flush();
    // The newest job is what is on disk; the older failure no longer matters.
    CHECK_FALSE(w.take_failure());
}

TEST_CASE("latest_wins_worker: a throwing job is a failure, not a crash",
          "[utils][latest_wins]") {
    LatestWinsWorker w;
    w.submit([]() -> bool { throw std::runtime_error("disk on fire"); });
    w.flush();
    CHECK(w.take_failure());

    // The worker survived and still runs jobs.
    std::atomic<bool> ran{false};
    w.submit([&] { ran = true; return true; });
    w.flush();
    CHECK(ran.load());
}

TEST_CASE("latest_wins_worker: destruction runs the pending job",
          "[utils][latest_wins]") {
    std::atomic<int> ran{0};
    {
        LatestWinsWorker w;
        w.submit([&] { ++ran; return true; });
    }  // no flush(): teardown must not drop the final write
    CHECK(ran.load() == 1);
}

TEST_CASE("latest_wins_worker: an owner that never submits is idle",
          "[utils][latest_wins]") {
    LatestWinsWorker w;
    CHECK_FALSE(w.busy());
    w.flush();
    CHECK(w.flush_until(std::chrono::steady_clock::now()));
}
