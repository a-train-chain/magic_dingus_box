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

namespace {

std::optional<std::vector<uint16_t>> parse_bitmap(const std::string& text,
                                              unsigned word_bits) {
    if (word_bits == 0 || word_bits > 64) return std::nullopt;
    std::vector<std::string> words;
    std::istringstream in(text);
    for (std::string w; in >> w;) words.push_back(w);

    if (words.empty()) return std::nullopt;
    std::vector<uint16_t> out;
    // words.back() is word 0 (bits 0..word_bits-1).
    for (size_t i = 0; i < words.size(); ++i) {
        const std::string& w = words[words.size() - 1 - i];
        if (w.size() > word_bits / 4) return std::nullopt;
        unsigned long long v = 0;
        for (char c : w) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (!std::isxdigit(u)) return std::nullopt;
            const unsigned nibble = std::isdigit(u) ? u - '0'
                                                    : std::tolower(u) - 'a' + 10;
            v = (v << 4) | nibble;
        }
        for (unsigned b = 0; b < word_bits; ++b) {
            if (v & (1ULL << b)) {
                const unsigned long code = i * word_bits + b;
                if (code > 0xffff) return std::nullopt;
                out.push_back(static_cast<uint16_t>(code));
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

std::vector<uint16_t> parse_capability_bitmap(const std::string& text,
                                              unsigned word_bits) {
    return parse_bitmap(text, word_bits).value_or(std::vector<uint16_t>{});
}

bool is_joystick_device(const SysfsInputInfo& info) {
    if (!info.capabilities_valid) return false;
    const auto has = [](const auto& codes, uint16_t code) {
        return std::binary_search(codes.begin(), codes.end(), code);
    };
    // Linux input-event-codes.h constants, kept portable for Mac suites.
    InputDeviceTraits traits;
    traits.has_abs_x = has(info.abs_codes, 0x00);      // ABS_X
    traits.has_abs_hat0x = has(info.abs_codes, 0x10);  // ABS_HAT0X
    traits.has_btn_touch = has(info.key_codes, 0x14a); // BTN_TOUCH
    traits.has_btn_digi = has(info.key_codes, 0x140);  // BTN_DIGI
    traits.accelerometer = info.accelerometer;
    return is_joystick_device(traits);
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
    auto keys = parse_bitmap(read_line(input / "capabilities/key"), word_bits);
    auto axes = parse_bitmap(read_line(input / "capabilities/abs"), word_bits);
    const auto props = parse_bitmap(read_line(input / "properties"), word_bits);
    info.capabilities_valid = keys.has_value() && axes.has_value() && props.has_value();
    if (keys) info.key_codes = std::move(*keys);
    if (axes) info.abs_codes = std::move(*axes);
    info.accelerometer = props && std::binary_search(props->begin(), props->end(),
                                                     kInputPropAccelerometer);

    const fs::path driver = fs::read_symlink(input / "device" / "driver", ec);
    if (!ec) info.driver = driver.filename().string();
    return info;
}

}  // namespace platform
