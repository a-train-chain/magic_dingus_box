#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include "platform/sequence_detector.h"

using namespace platform;
using seq_clock = std::chrono::steady_clock;

static seq_clock::time_point t(int ms) {
    return seq_clock::time_point(std::chrono::milliseconds(ms));
}

TEST_CASE("SequenceDetector: full correct sequence unlocks", "[sequence]") {
    SequenceDetector d;
    REQUIRE(d.feed(SeqInput::BTN1_BTN3_CHORD, t(0)) == SeqResult::PROGRESS);
    REQUIRE(d.feed(SeqInput::BTN2_PRESS, t(500)) == SeqResult::PROGRESS);
    REQUIRE(d.feed(SeqInput::BTN2_PRESS, t(1000)) == SeqResult::PROGRESS);
    REQUIRE(d.feed(SeqInput::BTN2_PRESS, t(1500)) == SeqResult::PROGRESS);
    REQUIRE(d.feed(SeqInput::ROTARY_CLICK, t(2000)) == SeqResult::UNLOCKED);
}

TEST_CASE("SequenceDetector: after unlock, fresh sequence needed", "[sequence]") {
    SequenceDetector d;
    d.feed(SeqInput::BTN1_BTN3_CHORD, t(0));
    d.feed(SeqInput::BTN2_PRESS, t(100));
    d.feed(SeqInput::BTN2_PRESS, t(200));
    d.feed(SeqInput::BTN2_PRESS, t(300));
    REQUIRE(d.feed(SeqInput::ROTARY_CLICK, t(400)) == SeqResult::UNLOCKED);
    // After unlock, the detector auto-resets. Another RCLICK should NOT unlock.
    REQUIRE(d.feed(SeqInput::ROTARY_CLICK, t(500)) == SeqResult::NO_MATCH);
}

TEST_CASE("SequenceDetector: wrong input resets silently", "[sequence]") {
    SequenceDetector d;
    REQUIRE(d.feed(SeqInput::BTN1_BTN3_CHORD, t(0)) == SeqResult::PROGRESS);
    REQUIRE(d.feed(SeqInput::BTN4_PRESS, t(100)) == SeqResult::NO_MATCH);
    // Must restart from the chord
    REQUIRE(d.feed(SeqInput::BTN2_PRESS, t(200)) == SeqResult::NO_MATCH);
    REQUIRE(d.feed(SeqInput::BTN1_BTN3_CHORD, t(300)) == SeqResult::PROGRESS);
}

TEST_CASE("SequenceDetector: timeout between events resets", "[sequence]") {
    SequenceDetector d;
    REQUIRE(d.feed(SeqInput::BTN1_BTN3_CHORD, t(0)) == SeqResult::PROGRESS);
    // 2001ms > TIMEOUT_MS (2000) — next event resets + is not the chord,
    // so it's NO_MATCH.
    REQUIRE(d.feed(SeqInput::BTN2_PRESS, t(2001)) == SeqResult::NO_MATCH);
}

TEST_CASE("SequenceDetector: exactly-at-timeout is allowed", "[sequence]") {
    SequenceDetector d;
    REQUIRE(d.feed(SeqInput::BTN1_BTN3_CHORD, t(0)) == SeqResult::PROGRESS);
    REQUIRE(d.feed(SeqInput::BTN2_PRESS, t(2000)) == SeqResult::PROGRESS);
}

TEST_CASE("SequenceDetector: progress reflects step count", "[sequence]") {
    SequenceDetector d;
    REQUIRE(d.progress() == 0);
    d.feed(SeqInput::BTN1_BTN3_CHORD, t(0));
    REQUIRE(d.progress() == 1);
    d.feed(SeqInput::BTN2_PRESS, t(100));
    REQUIRE(d.progress() == 2);
    d.feed(SeqInput::BTN4_PRESS, t(200));  // wrong
    REQUIRE(d.progress() == 0);
}

TEST_CASE("SequenceDetector: explicit reset clears state", "[sequence]") {
    SequenceDetector d;
    d.feed(SeqInput::BTN1_BTN3_CHORD, t(0));
    d.feed(SeqInput::BTN2_PRESS, t(100));
    REQUIRE(d.progress() == 2);
    d.reset();
    REQUIRE(d.progress() == 0);
}
