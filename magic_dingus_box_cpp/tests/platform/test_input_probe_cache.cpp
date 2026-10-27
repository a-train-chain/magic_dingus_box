// platform::InputProbeCache — the hotplug rescan's memory of nodes it
// already turned down.
//
// The bug these pin: every 3 s the kiosk reopened each /dev/input node no
// scan adopts (HDMI jack switches, the power button), and each evdev close()
// stalls ~20–47 ms on a Pi 4B — a ~300 ms frozen frame every 3 s of video.
// The cache must skip exactly those nodes and nothing else: a pad replugged
// onto the same eventN still has to be probed.

#include <catch2/catch_test_macros.hpp>

#include "platform/input_probe_cache.h"

using platform::InputNodeId;
using platform::InputProbeCache;

namespace {

InputNodeId node(uint64_t rdev, uint64_t ino, int64_t sec, int64_t nsec = 0) {
    InputNodeId id;
    id.rdev = rdev;
    id.ino = ino;
    id.ctime_sec = sec;
    id.ctime_nsec = nsec;
    return id;
}

}  // namespace

TEST_CASE("an unseen node is probed", "[input_probe_cache]") {
    InputProbeCache cache;
    CHECK(cache.should_probe("/dev/input/event2", node(0xd42, 500, 100)));
}

TEST_CASE("a rejected node is not probed again", "[input_probe_cache]") {
    InputProbeCache cache;
    const auto jack = node(0xd42, 500, 100);
    cache.remember_rejected("/dev/input/event2", jack);
    CHECK_FALSE(cache.should_probe("/dev/input/event2", jack));
    // Other nodes are unaffected.
    CHECK(cache.should_probe("/dev/input/event4", node(0xd44, 501, 100)));
}

TEST_CASE("a node recreated on the same path is probed again",
          "[input_probe_cache]") {
    InputProbeCache cache;
    cache.remember_rejected("/dev/input/event5", node(0xd45, 600, 100));

    SECTION("new inode, same major:minor (unplug + replug)") {
        CHECK(cache.should_probe("/dev/input/event5", node(0xd45, 601, 100)));
    }
    SECTION("new ctime (udev permission fix-up after hotplug)") {
        CHECK(cache.should_probe("/dev/input/event5", node(0xd45, 600, 100, 1)));
        CHECK(cache.should_probe("/dev/input/event5", node(0xd45, 600, 101)));
    }
    SECTION("different device number") {
        CHECK(cache.should_probe("/dev/input/event5", node(0xd46, 600, 100)));
    }
}

TEST_CASE("remembering again replaces the old identity",
          "[input_probe_cache]") {
    InputProbeCache cache;
    const auto old_node = node(0xd45, 600, 100);
    const auto new_node = node(0xd45, 601, 200);
    cache.remember_rejected("/dev/input/event5", old_node);
    cache.remember_rejected("/dev/input/event5", new_node);
    CHECK(cache.size() == 1);
    CHECK_FALSE(cache.should_probe("/dev/input/event5", new_node));
    CHECK(cache.should_probe("/dev/input/event5", old_node));
}
