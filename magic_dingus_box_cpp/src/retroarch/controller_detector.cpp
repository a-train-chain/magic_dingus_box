#include "controller_detector.h"

#include "../platform/input_device_info.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace retroarch {

namespace {

// The uinput phone remote also has gamepad capabilities, but it must never
// consume a physical player's slot. Match its exact name, not its generic
// 0001:0001 identity (which other virtual devices could share).
constexpr const char* kPhoneRemoteName = "MagicDingus Phone Remote";

std::optional<unsigned> event_number(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    constexpr size_t prefix_size = 5;  // "event"
    if (name.compare(0, prefix_size, "event") != 0 || name.size() == prefix_size) {
        return std::nullopt;
    }
    unsigned number = 0;
    const char* begin = name.data() + prefix_size;
    const char* end = name.data() + name.size();
    const auto parsed = std::from_chars(begin, end, number);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
    return number;
}

} // anonymous namespace

ControllerType match_vid_pid(uint16_t vid, uint16_t pid) {
    if (vid == 0x0e6d && pid == 0x111d) return ControllerType::N64_ADAPTER;
    if (vid == 0x0079 && pid == 0x0006) return ControllerType::PS_STYLE_DRAGONRISE;
    return ControllerType::UNKNOWN;
}

ControllerType detect_primary_controller() {
    return detect_primary_controller("/dev/input", "/sys/class/input");
}

ControllerType detect_primary_controller(const std::string& input_dir,
                                         const std::string& class_input_dir) {
    // The legacy autoconfig decision must use exactly the same inventory as
    // per-port mapping resolution, including auxiliary/phone exclusions.
    for (const auto& pad : detect_connected_controllers(input_dir, class_input_dir)) {
        const auto type = match_vid_pid(pad.vid, pad.pid);
        if (type != ControllerType::UNKNOWN) return type;
    }
    return ControllerType::UNKNOWN;
}

std::vector<DetectedPad> detect_connected_controllers() {
    return detect_connected_controllers("/dev/input", "/sys/class/input");
}

std::vector<DetectedPad> detect_connected_controllers(const std::string& input_dir,
                                                     const std::string& class_input_dir) {
    namespace fs = std::filesystem;
    std::vector<DetectedPad> pads;
    std::vector<std::pair<unsigned, fs::path>> event_nodes;
    std::error_code ec;
    // Scan only the canonical eventN names, never js/by-id/by-path aliases.
    // Event-only Bluetooth pads do not require the legacy joydev module.
    for (fs::directory_iterator it(input_dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (const auto number = event_number(it->path())) {
            event_nodes.emplace_back(*number, it->path());
        }
    }
    std::sort(event_nodes.begin(), event_nodes.end());

    for (const auto& [number, node] : event_nodes) {
        (void)number;
        const auto info = platform::read_sysfs_input_info(class_input_dir,
                                                          node.filename().string());
        // A disconnect can remove sysfs files part-way through a scan. Do
        // not classify incomplete capability reads as a pad, especially if
        // the missing property/key file would have excluded an IMU/touchpad.
        if (!info || info->name.empty() || !platform::is_joystick_device(*info)) continue;
        if (info->name == kPhoneRemoteName) continue;
        if (!fs::exists(node, ec) || ec || fs::is_directory(node, ec) || ec) continue;

        DetectedPad pad;
        pad.port = static_cast<int>(pads.size());
        pad.vid = info->vid;
        pad.pid = info->pid;
        pad.name = info->name;
        pad.driver = info->driver;
        pad.bustype = info->bustype;
        pad.key_codes = info->key_codes;
        pad.abs_codes = info->abs_codes;
        std::cout << "controller_detector: " << node.string() << " vid="
                  << std::hex << pad.vid << " pid=" << pad.pid << std::dec
                  << " name=" << pad.name
                  << " driver=" << (pad.driver.empty() ? "?" : pad.driver)
                  << (pad.bustype == 0x05 ? " (Bluetooth)" : "") << std::endl;
        pads.push_back(std::move(pad));
    }
    return pads;
}

std::string controller_type_name(ControllerType t) {
    switch (t) {
        case ControllerType::N64_ADAPTER:         return "N64_ADAPTER";
        case ControllerType::PS_STYLE_DRAGONRISE: return "PS_STYLE_DRAGONRISE";
        case ControllerType::UNKNOWN:             return "UNKNOWN";
    }
    return "UNKNOWN";
}

} // namespace retroarch
