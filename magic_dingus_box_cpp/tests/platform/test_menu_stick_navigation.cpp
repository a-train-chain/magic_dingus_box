#include <catch2/catch_test_macros.hpp>
#include "platform/menu_stick_navigation.h"

using platform::MenuStickNavigation;

TEST_CASE("Dense analog reports produce one deliberate menu step", "[menu_stick]") {
    MenuStickNavigation stick;
    REQUIRE(stick.update(-32768, 32767, 26000, 1000) == 1);
    for (int ms = 1001; ms < 1400; ++ms) {
        REQUIRE(stick.update(-32768, 32767, 26000 + ms % 100, ms) == 0);
        REQUIRE(stick.poll(ms) == 0);
    }
    REQUIRE(stick.poll(1400) == 1);
}

TEST_CASE("Held menu stick repeats without reports at D-pad slow and fast cadence", "[menu_stick]") {
    MenuStickNavigation stick;
    REQUIRE(stick.update(-32768, 32767, -25000, 0) == -1);
    REQUIRE(stick.poll(399) == 0);
    REQUIRE(stick.poll(400) == -1);
    REQUIRE(stick.poll(524) == 0);
    REQUIRE(stick.poll(525) == -1);
    REQUIRE(stick.poll(1375) == -1);
    REQUIRE(stick.poll(1399) == 0);
    REQUIRE(stick.poll(1425) == -1);
    REQUIRE(stick.poll(1474) == 0);
    REQUIRE(stick.poll(1475) == -1);
    // A delayed frame emits one step, never a catch-up burst.
    REQUIRE(stick.poll(10000) == -1);
    REQUIRE(stick.poll(10000) == 0);
}

TEST_CASE("Neutral cancels stick repeats and reversal starts its own delay", "[menu_stick]") {
    MenuStickNavigation stick;
    REQUIRE(stick.update(-100, 100, 100, 0) == 1);
    REQUIRE(stick.update(-100, 100, -100, 200) == -1);
    REQUIRE(stick.poll(599) == 0);
    REQUIRE(stick.poll(600) == -1);
    REQUIRE(stick.update(-100, 100, 0, 650) == 0);
    REQUIRE(stick.poll(1000) == 0);
    REQUIRE(stick.update(-100, 100, -100, 1010) == -1);
    REQUIRE(stick.poll(1409) == 0);
}

TEST_CASE("Hysteresis tolerates threshold jitter without extra steps", "[menu_stick]") {
    MenuStickNavigation stick;
    REQUIRE(stick.update(-100, 100, 49, 0) == 0);
    REQUIRE(stick.update(-100, 100, 50, 10) == 1);
    REQUIRE(stick.update(-100, 100, 49, 20) == 0);
    REQUIRE(stick.update(-100, 100, 36, 30) == 0);
    REQUIRE(stick.update(-100, 100, 51, 40) == 0);
    REQUIRE(stick.poll(410) == 1);
    REQUIRE(stick.update(-100, 100, 35, 420) == 0);
    REQUIRE(stick.update(-100, 100, 49, 430) == 0);
    REQUIRE(stick.poll(1000) == 0);
    REQUIRE(stick.update(-100, 100, 50, 1010) == 1);
    REQUIRE(stick.update(-100, 100, -49, 1020) == 0);
    REQUIRE(stick.update(-100, 100, -50, 1030) == -1);
    REQUIRE(stick.update(-100, 100, -36, 1040) == 0);
    REQUIRE(stick.update(-100, 100, -35, 1050) == 0);
    REQUIRE(stick.poll(2000) == 0);
}

TEST_CASE("Menu sticks normalize signed unsigned and wide axis ranges", "[menu_stick]") {
    struct Range { int32_t lo, hi, centre, positive, negative; };
    for (const auto r : {Range{-32768,32767,0,20000,-20000},
                         Range{0,255,127,200,50}, Range{0,1023,511,800,200},
                         Range{100,1100,600,900,300},
                         Range{INT32_MIN,INT32_MAX,0,INT32_MAX,INT32_MIN}}) {
        MenuStickNavigation stick;
        REQUIRE(stick.update(r.lo, r.hi, r.centre, 0) == 0);
        REQUIRE(stick.update(r.lo, r.hi, r.positive, 1) == 1);
        REQUIRE(stick.update(r.lo, r.hi, r.negative, 2) == -1);
    }
}

TEST_CASE("Independent devices and axes do not reset one another", "[menu_stick]") {
    MenuStickNavigation first_x, first_y, second_x;
    REQUIRE(first_x.update(-100,100,80,0) == 1);
    REQUIRE(second_x.update(-100,100,-80,100) == -1);
    REQUIRE(first_y.update(-100,100,-80,200) == -1);
    REQUIRE(first_x.poll(400) == 1);
    REQUIRE(second_x.poll(400) == 0);
    REQUIRE(first_y.poll(400) == 0);
    REQUIRE(second_x.poll(500) == -1);
    REQUIRE(first_y.poll(600) == -1);
    first_x.reset();
    REQUIRE(first_x.poll(1000) == 0);
    REQUIRE(second_x.poll(1000) == -1);
}

TEST_CASE("Capture disconnect resync and remap reset cancel pending stick repeats", "[menu_stick]") {
    MenuStickNavigation stick;
    REQUIRE(stick.update(-100,100,80,0) == 1);
    stick.reset();
    stick.reset();
    REQUIRE(stick.poll(5000) == 0);
    REQUIRE(stick.update(-100,100,80,5001) == 1);
    REQUIRE(stick.poll(5400) == 0);
    REQUIRE(stick.poll(5401) == 1);
}

TEST_CASE("Degenerate ranges cancel a prior hold", "[menu_stick]") {
    MenuStickNavigation stick;
    REQUIRE(stick.update(-100,100,80,0) == 1);
    REQUIRE(stick.update(0,0,0,100) == 0);
    REQUIRE(stick.poll(1000) == 0);
    REQUIRE(stick.update(100,-100,0,1100) == 0);
}
