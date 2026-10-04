#include "udev_wake.h"

#include <algorithm>
#include <filesystem>
#include <iostream>

#include "../utils/subprocess.h"

namespace platform::udev {

namespace fs = std::filesystem;

bool is_virtual_device_path(const std::string& resolved_sysfs_path) {
    return resolved_sysfs_path.find("/devices/virtual/") != std::string::npos;
}

std::vector<std::string> real_input_nodes(const std::string& class_dir,
                                          const std::string& prefix) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(class_dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        std::error_code rec;
        const fs::path resolved = fs::canonical(it->path(), rec);
        // An unresolvable entry is a node that just vanished (unplug race):
        // nothing to wake.
        if (rec) continue;
        if (is_virtual_device_path(resolved.string())) continue;
        out.push_back(it->path().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> trigger_argv(const std::vector<std::string>& paths) {
    if (paths.empty()) return {};
    std::vector<std::string> argv = {"sudo", "udevadm", "trigger", "--action=change"};
    argv.insert(argv.end(), paths.begin(), paths.end());
    return argv;
}

void wake_input_devices() {
    // udevadm trigger only queues uevents and returns; 5 s is generous and
    // exists so a wedged udevd can never hang the launch/return path.
    constexpr std::chrono::milliseconds kTimeout{5000};
    for (const char* prefix : {"js", "event"}) {
        const auto argv = trigger_argv(real_input_nodes("/sys/class/input", prefix));
        if (argv.empty()) continue;
        const auto r = utils::subprocess::run(argv, kTimeout);
        if (!r.ok()) {
            std::cerr << "udev wake (" << prefix << "*): udevadm trigger "
                      << (r.timed_out ? "timed out" : "exited " + std::to_string(r.exit_code))
                      << std::endl;
        }
    }
}

}  // namespace platform::udev
