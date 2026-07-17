// tests/retroarch/test_game_quiet_mode.cpp
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "app/game_quiet_mode.h"

TEST_CASE("GameQuietMode applies pause then resume in order",
          "[quiet_mode]") {
    std::vector<int> order;   // 1 = pause ran, 2 = resume ran
    std::mutex order_mutex;

    app::GameQuietMode quiet({
        [&] { std::lock_guard<std::mutex> l(order_mutex); order.push_back(1); },
        [&] { std::lock_guard<std::mutex> l(order_mutex); order.push_back(2); },
    });

    quiet.request_pause();
    quiet.wait_until_idle();
    quiet.request_resume();
    quiet.wait_until_idle();

    std::lock_guard<std::mutex> l(order_mutex);
    REQUIRE(order == std::vector<int>{1, 2});
}

TEST_CASE("GameQuietMode never leaves services paused after a fast "
          "pause->resume flip", "[quiet_mode]") {
    std::atomic<int> pauses{0};
    std::atomic<int> resumes{0};

    app::GameQuietMode quiet({
        [&] {
            // Simulate the slow docker-stop path.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            pauses.fetch_add(1);
        },
        [&] { resumes.fetch_add(1); },
    });

    quiet.request_pause();
    // Flip back before the 50ms pause action can possibly finish.
    quiet.request_resume();
    quiet.wait_until_idle();

    // Either both ran (pause was in flight, resume corrected it) or
    // neither ran (coalesced before the worker picked it up). What must
    // NEVER happen is pause-without-resume.
    REQUIRE(pauses.load() == resumes.load());
}

TEST_CASE("GameQuietMode destructor applies the last pending request",
          "[quiet_mode]") {
    std::atomic<int> resumes{0};
    {
        app::GameQuietMode quiet({
            [] {},
            [&] { resumes.fetch_add(1); },
        });
        quiet.request_pause();
        quiet.wait_until_idle();
        quiet.request_resume();
        // No wait_until_idle — destructor must flush it.
    }
    REQUIRE(resumes.load() == 1);
}
