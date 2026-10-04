#include "platform_profile.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
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
            // Same gate keyed on the core, for items whose system string
            // is blank/nonstandard. Normalized form (normalize_emulator_core).
            p.unsupported_emulator_cores = {"mupen64plus_next_libretro",
                                            "parallel_n64_libretro",
                                            "flycast_libretro"};
            // 1.5 GB envelope board: posters get the smallest budget
            // (see header — ~230 w185 grid posters).
            p.artwork_cache_budget_bytes = 64u * 1024u * 1024u;
            // Experimental zero-copy video policy gate (header). Still
            // needs MDB_VIDEO_ZERO_COPY=1 to do anything.
            p.video_dmabuf_import_candidate = true;
            break;
        case PiModel::Pi5:
            p.has_analog_audio = false;
            p.gpiochip_labels = {"pinctrl-rp1"};
            p.rotary_events_per_detent = 1;  // measured on hardware
            // Profile DEFAULT only — service_quiet_mode() overrides this
            // per session when MemAvailable is under the floor. The
            // static skip trusted a 2026-07-26 measurement (1122MB free
            // during playback) that the stack outgrew within two weeks.
            p.pause_services_during_movie = false;
            // Movie playback trickles torrents at the qBit alternative
            // speed limits instead of pausing the swarm — the Pi 5 has
            // the IO/CPU headroom, so downloads keep moving during films.
            // Pi 4 / Unknown keep the full pause (struct default false).
            p.trickle_torrents_during_video = true;
            p.artwork_cache_budget_bytes = 128u * 1024u * 1024u;
            // No hardware H.264 decoder -> no dmabuf frames to import.
            p.video_dmabuf_import_candidate = false;
            break;
        case PiModel::Unknown:
            // Conservative: no analog jack assumed (HDMI always exists on
            // supported boards), but try every known header-chip label so
            // GPIO still comes up on an unrecognized future board.
            p.has_analog_audio = false;
            p.gpiochip_labels = {"pinctrl-rp1", "pinctrl-bcm2711",
                                 "pinctrl-bcm2835"};
            p.artwork_cache_budget_bytes = 128u * 1024u * 1024u;
            p.video_dmabuf_import_candidate = false;
            break;
    }
    return p;
}

PlatformProfile detect_platform(const std::string& model_path) {
    std::ifstream f(model_path, std::ios::binary);
    if (!f.is_open()) {
        // Dev-VM impersonation (dev/pisim/README.md): with no device tree
        // to read, MDB_PI_MODEL_OVERRIDE supplies a model string so a Linux
        // VM can exercise the Pi 4B / Pi 5 profile paths. Honored ONLY when
        // the real file is absent — a stray env var can never re-profile an
        // actual board.
        if (const char* o = std::getenv("MDB_PI_MODEL_OVERRIDE"); o && *o) {
            return profile_for(parse_pi_model(o));
        }
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

std::string normalize_emulator_core(const std::string& emulator_core) {
    std::string out;
    out.reserve(emulator_core.size() + 9);
    for (char c : emulator_core) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '"' ||
            c == '\'' || c == '\0') {
            continue;
        }
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    if (out.size() > 3 && out.compare(out.size() - 3, 3, ".so") == 0) {
        out.resize(out.size() - 3);
    }
    if (out.empty() || out == "auto") return out;
    static const std::string kSuffix = "_libretro";
    if (out.size() < kSuffix.size() ||
        out.compare(out.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
        out += kSuffix;
    }
    return out;
}

bool supports_emulator_core(const PlatformProfile& profile,
                            const std::string& emulator_core) {
    const std::string key = normalize_emulator_core(emulator_core);
    if (key.empty() || key == "auto") return true;
    return std::find(profile.unsupported_emulator_cores.begin(),
                     profile.unsupported_emulator_cores.end(),
                     key) == profile.unsupported_emulator_cores.end();
}

bool supports_game_item(const PlatformProfile& profile,
                        const std::string& emulator_system,
                        const std::string& emulator_core) {
    return supports_game_system(profile, emulator_system) &&
           supports_emulator_core(profile, emulator_core);
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

ServiceQuietMode service_quiet_mode(const PlatformProfile& profile,
                                    long mem_available_kib) {
    if (profile.pause_services_during_movie) {
        return ServiceQuietMode::FullPause;
    }
    // Negative (unreadable) falls below any positive floor and lands on
    // FullPause — the conservative side.
    if (mem_available_kib < kServiceQuietMemFloorKiB) {
        return ServiceQuietMode::FullPause;
    }
    return ServiceQuietMode::Trickle;
}

long parse_mem_available_kib(const std::string& meminfo_text) {
    static const std::string kKey = "MemAvailable:";
    std::istringstream in(meminfo_text);
    std::string line;
    while (std::getline(in, line)) {
        // Line-anchored: /proc/meminfo keys start in column 0.
        if (line.rfind(kKey, 0) != 0) continue;
        const char* p = line.c_str() + kKey.size();
        char* end = nullptr;
        long v = std::strtol(p, &end, 10);
        if (end == p) return -1;  // "MemAvailable:" with no number
        return v;
    }
    return -1;
}

long read_mem_available_kib(const std::string& meminfo_path) {
    std::ifstream f(meminfo_path);
    if (!f) return -1;
    std::stringstream ss;
    ss << f.rdbuf();
    return parse_mem_available_kib(ss.str());
}

} // namespace platform
