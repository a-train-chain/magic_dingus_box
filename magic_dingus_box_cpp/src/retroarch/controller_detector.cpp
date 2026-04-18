#include "controller_detector.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace retroarch {

namespace {

// Read a single line from a sysfs file, trimmed.
// Returns empty string on any error.
std::string read_sysfs_line(const std::filesystem::path& p) {
    std::ifstream f(p);
    if (!f.good()) return {};
    std::string line;
    std::getline(f, line);
    // Trim trailing whitespace/newline (fs files usually end with \n)
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    return line;
}

// Parse a 4-char lowercase hex string into a uint16_t.
// Returns 0 on any parse failure.
uint16_t parse_hex4(const std::string& s) {
    if (s.size() != 4) return 0;
    try {
        size_t pos = 0;
        unsigned long v = std::stoul(s, &pos, 16);
        if (pos != 4 || v > 0xFFFF) return 0;
        return static_cast<uint16_t>(v);
    } catch (...) {
        return 0;
    }
}

// Match a (vendor, product) pair to a known controller type.
ControllerType match_vid_pid(uint16_t vid, uint16_t pid) {
    if (vid == 0x0e6d && pid == 0x111d) return ControllerType::N64_ADAPTER;
    if (vid == 0x0079 && pid == 0x0006) return ControllerType::PS_STYLE_DRAGONRISE;
    return ControllerType::UNKNOWN;
}

} // anonymous namespace

ControllerType detect_primary_controller() {
    namespace fs = std::filesystem;

    // Enumerate /dev/input/js* lexicographically.
    std::vector<fs::path> js_nodes;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/dev/input", ec)) {
        if (ec) break;
        const auto& name = entry.path().filename().string();
        if (name.rfind("js", 0) == 0 && name.size() > 2) {
            js_nodes.push_back(entry.path());
        }
    }
    std::sort(js_nodes.begin(), js_nodes.end());

    if (js_nodes.empty()) {
        std::cout << "controller_detector: no /dev/input/js* found" << std::endl;
        return ControllerType::UNKNOWN;
    }

    for (const auto& node : js_nodes) {
        const std::string basename = node.filename().string();  // e.g. "js0"
        const fs::path id_dir = fs::path("/sys/class/input") / basename / "device" / "id";

        std::string vid_s = read_sysfs_line(id_dir / "vendor");
        std::string pid_s = read_sysfs_line(id_dir / "product");

        uint16_t vid = parse_hex4(vid_s);
        uint16_t pid = parse_hex4(pid_s);

        ControllerType t = match_vid_pid(vid, pid);
        std::cout << "controller_detector: " << node.string()
                  << " vid=" << vid_s << " pid=" << pid_s
                  << " -> " << controller_type_name(t) << std::endl;

        if (t != ControllerType::UNKNOWN) {
            return t;
        }
    }

    // Nothing matched; return UNKNOWN (caller will fall back to default mapping)
    return ControllerType::UNKNOWN;
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
