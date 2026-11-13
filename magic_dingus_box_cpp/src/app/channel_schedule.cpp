#include "channel_schedule.h"

#include <cmath>
#include <cstdint>
#include <random>

namespace app {

namespace {

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

bool is_local_video(const PlaylistItem& it) {
    return it.source_type.empty() || it.source_type == "local" || it.source_type == "video";
}

// Seconds for a scheduled item, or 0 when it has no usable length.
double item_length(const PlaylistItem& it, const MediaInfoMap& info) {
    if (it.end > 0.0) {
        const double len = it.end - it.start;
        return len > 0.0 ? len : 0.0;
    }
    auto found = info.find(media_key_of_item(it));
    if (found != info.end() && found->second.duration_s.has_value()) {
        const double len = *found->second.duration_s - it.start;
        return len > 0.0 ? len : 0.0;
    }
    return 0.0;
}

void finish(Channel& c) {
    c.total_s = 0;
    for (const auto& s : c.items) c.total_s += s.length_s;
}

}  // namespace

std::string media_key_of_item(const PlaylistItem& item) {
    std::string p = item.path;
    if (starts_with(p, "./")) p.erase(0, 2);
    if (starts_with(p, "data/")) p.erase(0, 5);
    return p;
}

std::vector<Channel> build_channels(const std::vector<Playlist>& ui_playlists,
                                    const MediaInfoMap& info) {
    std::vector<Channel> channels(ui_playlists.size());
    for (size_t row = 0; row < ui_playlists.size(); ++row) {
        Channel& c = channels[row];
        c.number = static_cast<int>(row);
        if (row == 0) continue;  // filled below from the others
        const auto& items = ui_playlists[row].items;
        for (size_t i = 0; i < items.size(); ++i) {
            if (!is_local_video(items[i])) continue;
            const double len = item_length(items[i], info);
            if (len <= 0.0) continue;
            c.items.push_back({static_cast<int>(row), static_cast<int>(i), len});
        }
        finish(c);
    }

    if (!channels.empty()) {
        Channel& mix = channels[0];
        for (size_t row = 1; row < channels.size(); ++row) {
            mix.items.insert(mix.items.end(), channels[row].items.begin(),
                             channels[row].items.end());
        }
        std::mt19937 rng(0x4D4442);
        for (size_t i = mix.items.size(); i > 1; --i) {
            const size_t k = i - 1;
            const size_t j = static_cast<size_t>(rng() % (k + 1));
            std::swap(mix.items[k], mix.items[j]);
        }
        finish(mix);
    }
    return channels;
}

std::optional<OnAir> on_air(const Channel& channel, double now_utc) {
    if (channel.items.empty() || channel.total_s <= 0.0) return std::nullopt;
    double pos = std::fmod(now_utc - kScheduleEpochUtc, channel.total_s);
    if (pos < 0.0) pos += channel.total_s;
    if (pos >= channel.total_s) pos = 0.0;  // fmod rounding at the seam
    const double cycle_start = now_utc - pos;

    double acc = 0.0;
    for (size_t i = 0; i < channel.items.size(); ++i) {
        const double len = channel.items[i].length_s;
        if (pos < acc + len || i + 1 == channel.items.size()) {
            OnAir a;
            a.index = static_cast<int>(i);
            a.offset_s = pos - acc;
            a.starts_at = cycle_start + acc;
            a.ends_at = a.starts_at + len;
            return a;
        }
        acc += len;
    }
    return std::nullopt;
}

std::vector<OnAir> lineup(const Channel& channel, double from_utc, double to_utc) {
    std::vector<OnAir> out;
    auto cur = on_air(channel, from_utc);
    if (!cur || to_utc <= from_utc) return out;
    OnAir a = *cur;
    const size_t n = channel.items.size();
    while (a.starts_at < to_utc) {
        out.push_back(a);
        const size_t next = (static_cast<size_t>(a.index) + 1) % n;
        OnAir b;
        b.index = static_cast<int>(next);
        b.offset_s = 0;
        b.starts_at = a.ends_at;
        b.ends_at = b.starts_at + channel.items[next].length_s;
        a = b;
    }
    return out;
}

}  // namespace app
