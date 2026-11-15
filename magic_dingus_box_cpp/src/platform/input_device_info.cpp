#include "input_device_info.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace platform {

namespace fs = std::filesystem;

namespace {

constexpr unsigned kInputPropAccelerometer = 0x06;

std::string read_line(const fs::path& p) {
    std::ifstream f(p);
    std::string line;
    if (f) std::getline(f, line);
    while (!line.empty() &&
           std::isspace(static_cast<unsigned char>(line.back()))) {
        line.pop_back();
    }
    return line;
}

uint16_t read_hex4(const fs::path& p) {
    const std::string s = read_line(p);
    if (s.empty()) return 0;
    try {
        size_t used = 0;
        const unsigned long v = std::stoul(s, &used, 16);
        if (used != s.size() || v > 0xffff) return 0;
        return static_cast<uint16_t>(v);
    } catch (...) {
        return 0;
    }
}

}  // namespace

bool is_joystick_device(const InputDeviceTraits& t) {
    if (!t.has_abs_x && !t.has_abs_hat0x) return false;
    // Same exclusions as the kernel's joydev_blacklist.
    if (t.has_btn_touch || t.has_btn_digi || t.accelerometer) return false;
    return true;
}

std::vector<uint16_t> parse_capability_bitmap(const std::string& text,
                                              unsigned word_bits) {
    if (word_bits == 0 || word_bits > 64) return {};
    std::vector<std::string> words;
    std::istringstream in(text);
    for (std::string w; in >> w;) words.push_back(w);

    std::vector<uint16_t> out;
    // words.back() is word 0 (bits 0..word_bits-1).
    for (size_t i = 0; i < words.size(); ++i) {
        const std::string& w = words[words.size() - 1 - i];
        if (w.size() > word_bits / 4) return {};
        unsigned long long v = 0;
        for (char c : w) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (!std::isxdigit(u)) return {};
            const unsigned nibble = std::isdigit(u) ? u - '0'
                                                    : std::tolower(u) - 'a' + 10;
            v = (v << 4) | nibble;
        }
        for (unsigned b = 0; b < word_bits; ++b) {
            if (v & (1ULL << b)) {
                const unsigned long code = i * word_bits + b;
                if (code > 0xffff) return {};
                out.push_back(static_cast<uint16_t>(code));
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<SysfsInputInfo> read_sysfs_input_info(const std::string& class_input_dir,
                                                    const std::string& node) {
    std::error_code ec;
    const fs::path input = fs::path(class_input_dir) / node / "device";
    if (!fs::is_directory(input, ec)) return std::nullopt;

    // BITS_PER_LONG of the running kernel. The kiosk is a 64-bit build on a
    // 64-bit kernel; a 32-bit userland could only run on a 32-bit kernel.
    const unsigned word_bits = sizeof(long) * 8;

    SysfsInputInfo info;
    info.name = read_line(input / "name");
    info.bustype = read_hex4(input / "id/bustype");
    info.vid = read_hex4(input / "id/vendor");
    info.pid = read_hex4(input / "id/product");
    info.key_codes = parse_capability_bitmap(read_line(input / "capabilities/key"),
                                             word_bits);
    info.abs_codes = parse_capability_bitmap(read_line(input / "capabilities/abs"),
                                             word_bits);
    const auto props = parse_capability_bitmap(read_line(input / "properties"),
                                               word_bits);
    info.accelerometer = std::find(props.begin(), props.end(),
                                   kInputPropAccelerometer) != props.end();

    const fs::path driver = fs::read_symlink(input / "device" / "driver", ec);
    if (!ec) info.driver = driver.filename().string();
    return info;
}

}  // namespace platform
