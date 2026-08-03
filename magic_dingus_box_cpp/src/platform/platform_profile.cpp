#include "platform_profile.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace platform {

namespace {

// Device-tree strings carry a trailing NUL; files may add a newline.
std::string strip_trailing_junk(const std::string& s) {
    std::string out = s;
    while (!out.empty() &&
           (out.back() == '\0' || out.back() == '\n' ||
            out.back() == '\r' || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Extract the sink name (second tab-separated field) from each line of
// `pactl list short sinks` and return the first one the predicate accepts.
template <typename Pred>
std::optional<std::string> first_sink_matching(const std::string& pactl_out,
                                               Pred accept) {
    std::istringstream iss(pactl_out);
    std::string line;
    while (std::getline(iss, line)) {
        size_t first_tab = line.find('\t');
        if (first_tab == std::string::npos) continue;
        size_t second_tab = line.find('\t', first_tab + 1);
        std::string name = (second_tab == std::string::npos)
            ? line.substr(first_tab + 1)
            : line.substr(first_tab + 1, second_tab - first_tab - 1);
        if (!name.empty() && accept(to_lower(name))) {
            return name;
        }
    }
    return std::nullopt;
}

} // namespace

PiModel parse_pi_model(const std::string& device_tree_model) {
    // Prefix-with-trailing-space so "Raspberry Pi 4 Model B" matches but
    // e.g. "Raspberry Pi 400" (no jack, different product) does not.
    const std::string m = strip_trailing_junk(device_tree_model);
    if (m.rfind("Raspberry Pi 4 ", 0) == 0) return PiModel::Pi4;
    if (m.rfind("Raspberry Pi 5 ", 0) == 0) return PiModel::Pi5;
    return PiModel::Unknown;
}

PlatformProfile profile_for(PiModel model) {
    PlatformProfile p;
    p.model = model;
    switch (model) {
        case PiModel::Pi4:
            p.has_analog_audio = true;
            p.gpiochip_labels = {"pinctrl-bcm2711"};
            p.rotary_events_per_detent = 2;  // long-shipped default
            // Pi 5-only systems (see header). Tokens must already be in
            // normalize_game_system() form.
            p.unsupported_game_systems = {"n64", "dreamcast"};
            break;
        case PiModel::Pi5:
            p.has_analog_audio = false;
            p.gpiochip_labels = {"pinctrl-rp1"};
            p.rotary_events_per_detent = 1;  // measured on hardware
            // 1122MB free during 1080p playback with the stack running
            // — pausing buys nothing and causes the post-movie flicker.
            p.pause_services_during_movie = false;
            // Movie playback trickles torrents at the qBit alternative
            // speed limits instead of pausing the swarm — the Pi 5 has
            // the IO/CPU headroom, so downloads keep moving during films.
            // Pi 4 / Unknown keep the full pause (struct default false).
            p.trickle_torrents_during_video = true;
            break;
        case PiModel::Unknown:
            // Conservative: no analog jack assumed (HDMI always exists on
            // supported boards), but try every known header-chip label so
            // GPIO still comes up on an unrecognized future board.
            p.has_analog_audio = false;
            p.gpiochip_labels = {"pinctrl-rp1", "pinctrl-bcm2711",
                                 "pinctrl-bcm2835"};
            break;
    }
    return p;
}

PlatformProfile detect_platform(const std::string& model_path) {
    std::ifstream f(model_path, std::ios::binary);
    if (!f.is_open()) {
        return profile_for(PiModel::Unknown);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return profile_for(parse_pi_model(ss.str()));
}

std::string normalize_game_system(const std::string& emulator_system) {
    std::string out;
    out.reserve(emulator_system.size());
    for (char c : emulator_system) {
        if (c == ' ' || c == '"' || c == '\'' || c == '\0') continue;
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool supports_game_system(const PlatformProfile& profile,
                          const std::string& emulator_system) {
    const std::string key = normalize_game_system(emulator_system);
    if (key.empty()) return true;
    return std::find(profile.unsupported_game_systems.begin(),
                     profile.unsupported_game_systems.end(),
                     key) == profile.unsupported_game_systems.end();
}

std::optional<std::string> find_hdmi_sink(const std::string& pactl_short_sinks) {
    return first_sink_matching(pactl_short_sinks, [](const std::string& name) {
        return name.find("hdmi") != std::string::npos;
    });
}

std::optional<std::string> find_analog_sink(const std::string& pactl_short_sinks) {
    return first_sink_matching(pactl_short_sinks, [](const std::string& name) {
        if (name.find("hdmi") != std::string::npos) return false;
        // Pi 4 3.5mm jack sink is the bcm2835 "mailbox" card; USB DACs and
        // I2S HATs enumerate as "...analog-stereo".
        return name.find("mailbox") != std::string::npos ||
               name.find("analog") != std::string::npos;
    });
}

std::optional<std::string> resolve_sink(const std::string& pactl_short_sinks,
                                        SinkChoice want) {
    auto hdmi = find_hdmi_sink(pactl_short_sinks);
    auto analog = find_analog_sink(pactl_short_sinks);
    if (want == SinkChoice::Analog) {
        return analog ? analog : hdmi;
    }
    return hdmi ? hdmi : analog;
}

bool is_path_mounted(const std::string& proc_mounts_content,
                     const std::string& mount_point) {
    // /proc/mounts lines are "<device> <mount point> <fstype> <opts> ...".
    // Compare the second field exactly — a prefix match would let
    // /mnt/ssd2 satisfy a query for /mnt/ssd.
    std::istringstream iss(proc_mounts_content);
    std::string line;
    while (std::getline(iss, line)) {
        std::istringstream ls(line);
        std::string device, mp;
        if (!(ls >> device >> mp)) continue;
        if (mp == mount_point) return true;
    }
    return false;
}

bool is_storage_mounted(const std::string& mount_point,
                        const std::string& proc_mounts_path) {
    std::ifstream f(proc_mounts_path);
    if (!f.is_open()) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    return is_path_mounted(ss.str(), mount_point);
}

int pick_gpiochip(const std::vector<std::string>& chip_labels,
                  const std::vector<std::string>& wanted_labels) {
    for (const auto& wanted : wanted_labels) {
        auto it = std::find(chip_labels.begin(), chip_labels.end(), wanted);
        if (it != chip_labels.end()) {
            return static_cast<int>(it - chip_labels.begin());
        }
    }
    return -1;
}

} // namespace platform
