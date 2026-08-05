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
// The phone remote is a uinput virtual gamepad created by the Flask web
// service (web/remote/uinput_writer.py). The kernel gives it a js node like
// any other joystick, and whichever device enumerates first takes js0 -- so
// on a box where the web service wins that race, the phone remote becomes
// RetroArch's player 1 and every real pad is pushed to player 2.
//
// Observed on hardware 2026-08-04:
//   js0 = MagicDingus Phone Remote   vid=1 pid=1   -> no profile matches
//   js1 = SHANWAN Android Gamepad    vid=2563 pid=0526 -> the captured profile
//   input_player1_joypad_index = "0"
//   input_player1_a_btn = "nul"   (and every other player-1 bind)
//
// The kiosk's own UI reads evdev directly and is unaffected, so the pad works
// in menus and is dead in every game -- which is a confusing way to fail. It
// is also a RACE: a pad plugged in before the web service starts may win js0,
// so it works on some units and not others.
//
// Matched on NAME rather than vid/pid 1:1. The name is set by uinput_writer.py
// and is exact; filtering on vid==1&&pid==1 would be broader than intended and
// could exclude some other virtual-but-real input device later.
constexpr const char* kPhoneRemoteName = "MagicDingus Phone Remote";

bool is_phone_remote(const std::string& device_name) {
    return device_name == kPhoneRemoteName;
}

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

} // anonymous namespace

// Match a (vendor, product) pair to a known controller type. Declared in
// controller_detector.h (moved out of the anonymous namespace above) so
// resolve_mapping_for_pad() can reach it.
ControllerType match_vid_pid(uint16_t vid, uint16_t pid) {
    if (vid == 0x0e6d && pid == 0x111d) return ControllerType::N64_ADAPTER;
    if (vid == 0x0079 && pid == 0x0006) return ControllerType::PS_STYLE_DRAGONRISE;
    return ControllerType::UNKNOWN;
}

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

        const std::string dev_name =
            read_sysfs_line(fs::path("/sys/class/input") / basename / "device" / "name");
        if (is_phone_remote(dev_name)) {
            std::cout << "controller_detector: " << node.string()
                      << " is the phone remote -- skipping (not a game pad)"
                      << std::endl;
            continue;
        }

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

std::vector<DetectedPad> detect_connected_controllers() {
    namespace fs = std::filesystem;
    std::vector<DetectedPad> pads;

    // Same enumeration as detect_primary_controller(): walk /dev/input/js*
    // and sort lexicographically so port order is deterministic (js0, js1,
    // js2, ... -- not directory_iterator's unspecified OS order).
    std::vector<fs::path> js_nodes;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/dev/input", ec)) {
        if (ec) break;
        const auto& name = entry.path().filename().string();
        if (name.rfind("js", 0) == 0 && name.size() > 2) js_nodes.push_back(entry.path());
    }
    std::sort(js_nodes.begin(), js_nodes.end());

    int port = 0;
    for (const auto& node : js_nodes) {
        const std::string basename = node.filename().string();
        const fs::path id_dir = fs::path("/sys/class/input") / basename / "device" / "id";
        const std::string dev_name =
            read_sysfs_line(fs::path("/sys/class/input") / basename / "device" / "name");

        // Skip BEFORE the port counter advances, so real pads still start at
        // port 0 even when the phone remote holds a lower js node.
        if (is_phone_remote(dev_name)) {
            std::cout << "controller_detector: " << node.string()
                      << " is the phone remote -- skipping (ports unaffected)"
                      << std::endl;
            continue;
        }

        DetectedPad pad;
        pad.port = port++;
        pad.vid = parse_hex4(read_sysfs_line(id_dir / "vendor"));
        pad.pid = parse_hex4(read_sysfs_line(id_dir / "product"));
        pad.name = dev_name;
        std::cout << "controller_detector: " << node.string() << " vid="
                  << std::hex << pad.vid << " pid=" << pad.pid << std::dec
                  << " name=" << pad.name << std::endl;
        pads.push_back(pad);
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
