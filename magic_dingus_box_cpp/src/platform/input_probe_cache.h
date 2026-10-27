#pragma once

// platform::InputProbeCache — remembers the /dev/input nodes a hotplug scan
// already opened and turned down, so the next scan skips them instead of
// opening them again.
//
// Why it exists: InputManager rescans /dev/input every 3 s on the render
// loop (reprobe_phone_remote + rescan_devices) to pick up pads plugged in
// after boot. Each scan skipped only nodes it had ADOPTED, so every node
// nothing adopts — the vc4-hdmi "HDMI Jack" switches every Pi has, the
// gpio-shutdown power button — was opened and closed by all three scans,
// every pass. close() on an evdev fd waits out an RCU grace period: 20–47 ms
// each on a Pi 4B, nine of them back to back, a ~300 ms frozen frame every
// 3 s during video (measured with strace, 2026-10-27).
//
// A node is identified by what stat() reports, not by its path: a replugged
// device can come back on the same eventN (and the same major:minor) but
// udev creates a fresh node with a new inode, and its permission fix-up
// bumps ctime — either difference makes the node probe-worthy again, so a
// pad plugged in later is still seen. Pure bookkeeping; the stat() call
// lives in input_manager.cpp so this is checkable on the Mac.

#include <cstdint>
#include <map>
#include <string>

namespace platform {

struct InputNodeId {
    uint64_t rdev = 0;
    uint64_t ino = 0;
    int64_t ctime_sec = 0;
    int64_t ctime_nsec = 0;

    bool operator==(const InputNodeId& o) const {
        return rdev == o.rdev && ino == o.ino && ctime_sec == o.ctime_sec &&
               ctime_nsec == o.ctime_nsec;
    }
    bool operator!=(const InputNodeId& o) const { return !(*this == o); }
};

class InputProbeCache {
public:
    // True unless this exact node was already probed and rejected.
    bool should_probe(const std::string& path, const InputNodeId& id) const {
        auto it = rejected_.find(path);
        return it == rejected_.end() || it->second != id;
    }

    // Record a node the scan read successfully and decided not to adopt.
    // Never call this for a node that failed to open or read — that may be
    // a transient (udev still fixing permissions) and must be retried.
    void remember_rejected(const std::string& path, const InputNodeId& id) {
        rejected_[path] = id;
    }

    // Number of remembered nodes (bounded by the eventN paths that exist).
    size_t size() const { return rejected_.size(); }

private:
    std::map<std::string, InputNodeId> rejected_;
};

}  // namespace platform
