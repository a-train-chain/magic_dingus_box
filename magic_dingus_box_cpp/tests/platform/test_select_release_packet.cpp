#include <catch2/catch_test_macros.hpp>

#include "platform/select_release_packet.h"
#include "app/guide_select_hold.h"

using platform::InputAction;
using platform::InputEvent;
using platform::SelectReleasePacket;
using Action = app::GuideSelectInput::Action;
using Clock = app::GuideSelectInput::Clock;

namespace {
InputEvent select(bool pressed, uint64_t source = 11) {
    InputEvent event{InputAction::SELECT, 0, pressed};
    event.source_id = source;
    return event;
}
}

TEST_CASE("Synthetic Select release cancels before later-poll ENODEV", "[select-packet]") {
    // Real native order: down/SYN0, up/SYN1, EAGAIN, ENODEV next poll.
    SelectReleasePacket packet;
    app::GuideSelectInput gesture;
    const auto start = Clock::time_point{};
    gesture.press(11, start);
    std::vector<InputEvent> output;
    packet.append(select(false), output);
    // A poll boundary before SYN must not dispatch the candidate release.
    REQUIRE(output.empty());
    const bool interrupted = packet.finish(1, output);
    if (interrupted) gesture.source_lost(11);
    for (const auto& event : output)
        REQUIRE(gesture.release(event.source_id, start + std::chrono::milliseconds(150)) == Action::None);
    REQUIRE(interrupted);
    REQUIRE(output.empty());
    REQUIRE_FALSE(gesture.owns(11));
    REQUIRE(gesture.tick(start + std::chrono::milliseconds(600)) == Action::None);
    // Synthetic reset packets may come from a source that stays connected.
    gesture.press(11, start + std::chrono::milliseconds(610));
    REQUIRE(gesture.release(11, start + std::chrono::milliseconds(650)) == Action::Tap);
    gesture.source_lost(11); // ENODEV later is harmless.
    gesture.press(12, start + std::chrono::milliseconds(700));
    REQUIRE(gesture.release(12, start + std::chrono::milliseconds(800)) == Action::Tap);
}

TEST_CASE("Normal Select packet delivers a quick tap", "[select-packet]") {
    SelectReleasePacket packet;
    std::vector<InputEvent> output;
    packet.append(select(true), output);
    packet.append(select(false), output);
    REQUIRE(output.empty());
    REQUIRE_FALSE(packet.finish(0, output));
    REQUIRE(output.size() == 2);
    REQUIRE(output[0].pressed);
    REQUIRE_FALSE(output[1].pressed);
    app::GuideSelectInput gesture;
    gesture.press(output[0].source_id, Clock::time_point{});
    REQUIRE(gesture.release(output[1].source_id, Clock::time_point{} + std::chrono::milliseconds(100)) == Action::Tap);
}

TEST_CASE("Normal packet preserves ordinary action ordering beside Select", "[select-packet]") {
    SelectReleasePacket packet;
    std::vector<InputEvent> output;
    packet.append(select(true), output);
    packet.append({InputAction::ROTATE, 1, false}, output);
    packet.append(select(false), output);
    REQUIRE_FALSE(packet.finish(0, output));
    REQUIRE(output.size() == 3);
    REQUIRE(output[0].pressed);
    REQUIRE(output[1].action == InputAction::ROTATE);
    REQUIRE(output[1].delta == 1);
    REQUIRE_FALSE(output[2].pressed);
}

TEST_CASE("Repeat-only nonzero SYN preserves Select hold", "[select-packet]") {
    SelectReleasePacket packet;
    app::GuideSelectInput gesture;
    const auto start = Clock::time_point{};
    gesture.press(11, start);
    // EV_KEY value 2 is filtered before action mapping enters this packet.
    std::vector<InputEvent> output;
    REQUIRE_FALSE(packet.finish(1, output));
    REQUIRE(output.empty());
    REQUIRE(gesture.tick(start + std::chrono::milliseconds(500)) == Action::Hold);
}

TEST_CASE("Interrupted packet preserves ordinary actions and other sources", "[select-packet]") {
    SelectReleasePacket packet;
    std::vector<InputEvent> output;
    app::GuideSelectInput gesture;
    const auto start = Clock::time_point{};
    gesture.press(12, start);
    packet.append(select(true), output);
    packet.append({InputAction::ROTATE, -1, false}, output);
    packet.append(select(false), output);
    REQUIRE(packet.finish(7, output));
    gesture.source_lost(11);
    REQUIRE(output.size() == 1);
    REQUIRE(output[0].action == InputAction::ROTATE);
    REQUIRE(output[0].delta == -1);
    REQUIRE(gesture.release(12, start + std::chrono::milliseconds(100)) == Action::Tap);
}

TEST_CASE("Packet classification resets between multiple packets in one poll", "[select-packet]") {
    SelectReleasePacket packet;
    std::vector<InputEvent> output;
    packet.append(select(false), output);
    REQUIRE(packet.finish(1, output));
    packet.append(select(true), output);
    REQUIRE_FALSE(packet.finish(1, output)); // No release: preserve the press.
    packet.append(select(false), output);
    REQUIRE_FALSE(packet.finish(0, output));
    REQUIRE(output.size() == 2);
    REQUIRE(output[0].pressed);
    REQUIRE_FALSE(output[1].pressed);
}

TEST_CASE("Discarded partial packet cannot leak a release after resync", "[select-packet]") {
    SelectReleasePacket packet;
    std::vector<InputEvent> output;
    packet.append(select(false), output);
    packet.discard();
    REQUIRE_FALSE(packet.finish(0, output));
    REQUIRE(output.empty());
    packet.append(select(true), output);
    packet.append(select(false), output);
    REQUIRE_FALSE(packet.finish(0, output));
    REQUIRE(output.size() == 2);
}

TEST_CASE("Oversized unfinished packet preserves ordinary actions and cancels Select", "[select-packet]") {
    SelectReleasePacket packet;
    std::vector<InputEvent> output;
    bool interrupted = false;
    interrupted |= packet.append(select(true), output);
    for (int i = 0; i < 1024; ++i)
        interrupted |= packet.append({InputAction::ROTATE, i, false}, output);
    REQUIRE(interrupted);
    REQUIRE(output.size() == 1024);
    for (int i = 0; i < 1024; ++i) {
        REQUIRE(output[i].action == InputAction::ROTATE);
        REQUIRE(output[i].delta == i);
    }
    REQUIRE(packet.finish(0, output));
    output.clear();
    packet.append(select(true), output);
    packet.append(select(false), output);
    REQUIRE_FALSE(packet.finish(0, output));
    REQUIRE(output.size() == 2);
}
