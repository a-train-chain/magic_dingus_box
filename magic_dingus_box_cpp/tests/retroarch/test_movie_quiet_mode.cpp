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

TEST_CASE("claims keep downloads paused through movie browser and game handovers", "[movie_quiet_mode]") {
    using Claimant = MovieQuietMode::Claimant;
    Log log;
    MovieQuietMode q({[&](Mode m) { log.add(m == Mode::Trickle ? "trickle" : "full"); Consent c; c.alt_limited = true; return c; },
                      [&](const Consent&) { log.add("resume"); }});
    q.request_resume(Claimant::MovieChannel);
    q.request_pause(Mode::Trickle, Claimant::MovieChannel); q.wait_until_idle();
    q.request_pause(Mode::Trickle); q.wait_until_idle();
    REQUIRE(log.get() == std::vector<std::string>{"trickle"});
    q.request_resume(); q.wait_until_idle();
    REQUIRE(log.get() == std::vector<std::string>{"trickle"});
    q.request_pause(Mode::FullPause, Claimant::Game); q.wait_until_idle();
    q.request_resume(Claimant::MovieChannel); q.wait_until_idle();
    REQUIRE(log.get() == std::vector<std::string>{"trickle", "full"});
    q.request_resume(Claimant::Game); q.wait_until_idle();
    REQUIRE(log.get() == std::vector<std::string>{"trickle", "full", "resume"});
}

TEST_CASE("a deferred trickle claim cannot swallow a later explicit full pause", "[movie_quiet_mode]") {
    Log log;
    std::mutex m; std::condition_variable cv;
    bool selecting = false, release = false;
    MovieQuietMode q({[&](Mode mode) { log.add(mode == Mode::FullPause ? "full" : "trickle"); Consent c; c.alt_limited = true; return c; },
                      [&](const Consent&) { log.add("resume"); }});
    q.request_pause([&] {
        std::unique_lock<std::mutex> lock(m); selecting = true; cv.notify_all();
        cv.wait(lock, [&] { return release; }); return Mode::Trickle;
    }, MovieQuietMode::Claimant::MovieChannel);
    { std::unique_lock<std::mutex> lock(m); cv.wait(lock, [&] { return selecting; }); }
    CHECK(q.effective_full_pause()); // pending is conservative
    q.request_pause(Mode::FullPause);
    { std::lock_guard<std::mutex> lock(m); release = true; } cv.notify_all();
    q.wait_until_idle(); CHECK(q.effective_full_pause());
    CHECK(log.get() == std::vector<std::string>{"trickle", "full"});
    q.request_resume(); q.wait_until_idle(); CHECK(q.effective_full_pause());
    q.request_resume(MovieQuietMode::Claimant::MovieChannel); q.wait_until_idle();
    CHECK_FALSE(q.effective_full_pause());
    CHECK(log.get() == std::vector<std::string>{"trickle", "full", "resume"});
}

TEST_CASE("releasing an unstarted deferred claim cancels its selector", "[movie_quiet_mode]") {
    std::mutex m; std::condition_variable cv;
    bool pausing = false, release = false;
    int selections = 0;
    MovieQuietMode q({[&](Mode) {
        std::unique_lock<std::mutex> lock(m); pausing = true; cv.notify_all();
        cv.wait(lock, [&] { return release; }); Consent c; c.alt_limited = true; return c;
    }, [](const Consent&) {}});
    q.request_pause(Mode::Trickle);
    { std::unique_lock<std::mutex> lock(m); cv.wait(lock, [&] { return pausing; }); }
    q.request_resume();
    q.request_pause([&] { ++selections; return Mode::FullPause; });
    q.request_resume();
    { std::lock_guard<std::mutex> lock(m); release = true; } cv.notify_all();
    q.wait_until_idle(); CHECK(selections == 0); CHECK_FALSE(q.effective_full_pause());
}

TEST_CASE("startup recovery is serialized before the first pause", "[movie_quiet_mode]") {
    Log log;
    MovieQuietMode::Actions actions;
    actions.recover = [&] { log.add("recover"); return true; };
    actions.pause = [&](Mode) { log.add("pause"); Consent c; c.qbit_paused = true; return c; };
    actions.resume = [&](const Consent&) { log.add("resume"); };
    MovieQuietMode q(std::move(actions));
    q.request_pause(Mode::FullPause); q.wait_until_idle();
    q.request_resume(); q.wait_until_idle();
    CHECK(log.get() == std::vector<std::string>{"recover", "pause", "resume"});
}

TEST_CASE("a throwing resume does not suppress the next session's full pause", "[movie_quiet_mode]") {
    int pauses = 0;
    MovieQuietMode q({[&](Mode) { ++pauses; Consent c; c.services_paused = true; return c; },
                      [](const Consent&) { throw std::runtime_error("resume failed"); }});
    q.request_pause(Mode::FullPause); q.wait_until_idle();
    q.request_resume(); q.wait_until_idle();
    q.request_pause(Mode::FullPause); q.wait_until_idle();
    CHECK(pauses == 2);
}

TEST_CASE("a late old mode selection cannot publish policy into a later session", "[movie_quiet_mode]") {
    std::mutex m; std::condition_variable cv;
    bool selecting = false, release = false;
    Log log;
    MovieQuietMode q({[&](Mode mode) { log.add(mode == Mode::FullPause ? "full" : "trickle"); Consent c; c.qbit_paused = true; return c; },
                      [&](const Consent&) { log.add("resume"); }});
    q.request_pause([&] {
        std::unique_lock<std::mutex> lock(m); selecting = true; cv.notify_all();
        cv.wait(lock, [&] { return release; }); return Mode::FullPause;
    });
    { std::unique_lock<std::mutex> lock(m); cv.wait(lock, [&] { return selecting; }); }
    q.request_resume();
    q.request_pause([] { return Mode::Trickle; });
    CHECK(q.effective_full_pause());
    { std::lock_guard<std::mutex> lock(m); release = true; } cv.notify_all();
    q.wait_until_idle();
    CHECK_FALSE(q.effective_full_pause());
    CHECK(log.get() == std::vector<std::string>{"full", "resume", "trickle"});
    q.request_resume(); q.wait_until_idle();
}
