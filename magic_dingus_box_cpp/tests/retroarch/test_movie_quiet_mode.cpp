// tests/retroarch/test_movie_quiet_mode.cpp
// Lives next to test_game_quiet_mode.cpp (same test binary) — the movie
// contention guard's off-render-thread executor.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "app/movie_quiet_mode.h"

using app::MovieQuietMode;
using Mode = MovieQuietMode::Mode;
using Consent = MovieQuietMode::Consent;

namespace {
struct Log {
    std::mutex m;
    std::vector<std::string> ev;
    void add(std::string s) { std::lock_guard<std::mutex> l(m); ev.push_back(std::move(s)); }
    std::vector<std::string> get() { std::lock_guard<std::mutex> l(m); return ev; }
};
}  // namespace

TEST_CASE("MovieQuietMode: resume undoes exactly what the pause reported",
          "[movie_quiet]") {
    Log log;
    std::vector<Consent> resumed;
    MovieQuietMode q({
        [&](Mode m) {
            log.add(m == Mode::Trickle ? "pause-trickle" : "pause-full");
            Consent c;
            if (m == Mode::Trickle) c.alt_limited = true;
            else { c.qbit_paused = false; /* qBit refused */ c.services_paused = true; }
            return c;
        },
        [&](const Consent& c) { log.add("resume"); resumed.push_back(c); },
    });

    q.request_pause(Mode::FullPause);
    q.request_resume();  // may coalesce if the pause had not started
    q.wait_until_idle();
    // Either nothing happened, or pause+resume both ran.
    auto ev = log.get();
    REQUIRE((ev.empty() || ev == std::vector<std::string>{"pause-full", "resume"}));

    q.request_pause(Mode::FullPause);
    q.wait_until_idle();
    q.request_resume();
    q.wait_until_idle();
    REQUIRE(!resumed.empty());
    const Consent& c = resumed.back();
    REQUIRE(c.services_paused);
    REQUIRE_FALSE(c.qbit_paused);   // pause_all failed -> never resume_all
    REQUIRE_FALSE(c.alt_limited);
}

TEST_CASE("MovieQuietMode: a resume never overtakes an in-flight pause",
          "[movie_quiet]") {
    Log log;
    std::atomic<bool> pause_started{false};
    MovieQuietMode q({
        [&](Mode) {
            pause_started = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            log.add("pause-done");
            Consent c; c.qbit_paused = true; return c;
        },
        [&](const Consent&) { log.add("resume"); },
    });
    q.request_pause(Mode::FullPause);
    while (!pause_started) std::this_thread::yield();
    q.request_resume();  // pause is mid-flight: must queue behind it
    q.wait_until_idle();
    REQUIRE(log.get() == std::vector<std::string>{"pause-done", "resume"});
}

TEST_CASE("MovieQuietMode: resume with nothing paused calls nothing",
          "[movie_quiet]") {
    std::atomic<int> resumes{0};
    MovieQuietMode q({
        [](Mode) { return Consent{}; },  // every step failed
        [&](const Consent&) { resumes++; },
    });
    q.request_resume();
    q.request_pause(Mode::Trickle);
    q.wait_until_idle();
    q.request_resume();
    q.wait_until_idle();
    REQUIRE(resumes.load() == 0);
}

TEST_CASE("MovieQuietMode: requests return immediately while an action blocks",
          "[movie_quiet]") {
    std::atomic<bool> release{false};
    MovieQuietMode q({
        [&](Mode) {
            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            Consent c; c.services_paused = true; return c;
        },
        [](const Consent&) {},
    });
    const auto t0 = std::chrono::steady_clock::now();
    q.request_pause(Mode::FullPause);
    q.request_resume();
    q.request_pause(Mode::Trickle);
    const auto dt = std::chrono::steady_clock::now() - t0;
    REQUIRE(dt < std::chrono::milliseconds(50));
    REQUIRE_FALSE(q.wait_until_idle_for(std::chrono::milliseconds(20)));
    release = true;
    REQUIRE(q.wait_until_idle_for(std::chrono::seconds(5)));
}

TEST_CASE("MovieQuietMode: destructor drains a pending resume",
          "[movie_quiet]") {
    std::atomic<int> resumes{0};
    {
        MovieQuietMode q({
            [](Mode) { Consent c; c.alt_limited = true; return c; },
            [&](const Consent&) { resumes++; },
        });
        q.request_pause(Mode::Trickle);
        q.wait_until_idle();
        q.request_resume();
    }
    REQUIRE(resumes.load() == 1);
}

TEST_CASE("MovieQuietMode: a throwing action does not wedge the queue",
          "[movie_quiet]") {
    std::atomic<int> pauses{0};
    MovieQuietMode q({
        [&](Mode) -> Consent {
            if (pauses++ == 0) throw std::runtime_error("boom");
            return Consent{};
        },
        [](const Consent&) {},
    });
    q.request_pause(Mode::FullPause);
    q.wait_until_idle();
    q.request_resume();
    q.request_pause(Mode::FullPause);
    REQUIRE(q.wait_until_idle_for(std::chrono::seconds(5)));
}
