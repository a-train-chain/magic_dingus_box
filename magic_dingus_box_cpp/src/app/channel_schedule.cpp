#include "channel_schedule.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <random>

#include "playlist_reload.h"

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
        if (row == 0) {
            c.identity = kShuffleChannelId;
            c.order = LineupOrder::Shuffle;
            continue;  // filled below from the non-movie rows
        }
        const bool movie = is_movie_playlist(ui_playlists[row]);
        c.identity = movie ? ui_playlists[row].path : playlist_identity(ui_playlists[row]);
        if (movie) {
            c.order = LineupOrder::Shuffle;
            c.day_start_h = kMovieDayStartH;
            c.on_air_h = kMovieOnAirH;
        }
        const auto& items = ui_playlists[row].items;
        for (size_t i = 0; i < items.size(); ++i) {
            if (!is_local_video(items[i])) continue;
            const double len = item_length(items[i], info);
            if (!std::isfinite(len) || !(len >= kMinScheduledItemS)) continue;
            c.items.push_back({static_cast<int>(row), static_cast<int>(i), len});
        }
        finish(c);
    }

    if (!channels.empty()) {
        Channel& mix = channels[0];
        for (size_t row = 1; row < channels.size(); ++row) {
            if (is_movie_playlist(ui_playlists[row])) continue;
            mix.items.insert(mix.items.end(), channels[row].items.begin(),
                             channels[row].items.end());
        }
        finish(mix);
    }
    return channels;
}

namespace {

constexpr double kDayS = 86400.0;

uint32_t seed_for(const std::string& identity, int64_t day, int pass) {
    const std::string key = identity + "|" + std::to_string(day) + "|" + std::to_string(pass);
    uint32_t seed = 2166136261u;
    for (unsigned char byte : key) seed = (seed ^ byte) * 16777619u;
    return seed;
}

int offset_at(double utc, int fallback, const UtcOffsetAt& lookup) {
    return lookup ? lookup(utc) : fallback;
}

// Resolve a civil boundary using offsets on both sides, not just now's
// offset. At a fold use its FIRST occurrence; in a gap use the first valid
// instant after it. Adjacent-day probes cover DST and date-line changes.
// All lookups are injected, in-memory facts; never call tzset/mktime here.
double civil_to_utc(double local, int fallback, const UtcOffsetAt& lookup) {
    if (!lookup) return local - fallback;
    const std::array<int, 4> offsets = {
        fallback, lookup(local - 2 * kDayS), lookup(local), lookup(local + 2 * kDayS)};
    std::optional<double> first;
    double low = local - offsets[0];
    double high = low;
    for (int offset : offsets) {
        const double candidate = local - offset;
        low = std::min(low, candidate);
        high = std::max(high, candidate);
        if (candidate + lookup(candidate) == local && (!first || candidate < *first)) {
            first = candidate;
        }
    }
    if (first) return *first;
    // A skipped wall-clock boundary lies inside a forward transition. The
    // two candidate UTC instants bracket the gap; find its first whole
    // second, where the civil clock first reaches/passes the desired time.
    while (high - low > 1.0) {
        const double middle = std::floor(low + (high - low) / 2.0);
        if (middle + lookup(middle) >= local) high = middle;
        else low = middle;
    }
    return high;
}

struct DayWindow {
    int64_t day;
    double starts_at;
    double ends_at;
    double next_start;
};

std::optional<DayWindow> day_window(const Channel& channel, double utc,
                                   int offset, const UtcOffsetAt& lookup) {
    if (!std::isfinite(utc) || channel.items.empty() ||
        !std::isfinite(channel.total_s) || channel.total_s <= 0 ||
        channel.on_air_h <= 0) return std::nullopt;
    const double local = utc + offset_at(utc, offset, lookup);
    const double day_number = std::floor((local - channel.day_start_h * 3600.0) / kDayS);
    // Avoid an undefined float-to-integer conversion for nonsensical clocks.
    if (!std::isfinite(day_number) || std::fabs(day_number) > 1e9) return std::nullopt;
    int64_t day = static_cast<int64_t>(day_number);
    auto start = [&](int64_t d) {
        return civil_to_utc(d * kDayS + channel.day_start_h * 3600.0, offset, lookup);
    };
    double begins = start(day);
    double next = start(day + 1);
    // A repeated hour containing the boundary belongs to the new day after
    // its first occurrence; its civil clock can temporarily look like yesterday.
    if (utc < begins) { --day; next = begins; begins = start(day); }
    if (utc >= next) { ++day; begins = next; next = start(day + 1); }
    const double ends = channel.on_air_h >= 24 ? next :
        civil_to_utc(day * kDayS + (channel.day_start_h + channel.on_air_h) * 3600.0,
                     offset, lookup);
    return DayWindow{day, begins, ends, next};
}

}  // namespace

std::vector<int> day_order(const Channel& channel, int64_t day, int pass) {
    std::vector<int> indexes(channel.items.size());
    std::iota(indexes.begin(), indexes.end(), 0);
    if (indexes.empty()) return indexes;
    if (channel.order == LineupOrder::Rotate) {
        const size_t start = seed_for(channel.identity, day, 0) % indexes.size();
        std::rotate(indexes.begin(), indexes.begin() + start, indexes.end());
    } else {
        std::mt19937 rng(seed_for(channel.identity, day, pass));
        for (size_t k = indexes.size() - 1; k > 0; --k) {
            const size_t j = rng() % (k + 1);
            std::swap(indexes[k], indexes[j]);
        }
    }
    return indexes;
}

std::optional<OnAir> on_air(const Channel& channel, double now_utc,
                            int utc_offset_s, const UtcOffsetAt& lookup) {
    const auto day = day_window(channel, now_utc, utc_offset_s, lookup);
    if (!day || now_utc >= day->ends_at) return std::nullopt;
    const double elapsed = now_utc - day->starts_at;
    int pass = static_cast<int>(std::floor(elapsed / channel.total_s));
    // Fractional media lengths meet a much larger Unix timestamp here.
    // Compare the same rounded absolute seams that OnAir returns, otherwise
    // its own ends_at can resolve back into the ending item by ~0.1 us.
    if (now_utc >= day->starts_at + (pass + 1) * channel.total_s) ++pass;
    if (pass > 0 && now_utc < day->starts_at + pass * channel.total_s) --pass;
    const auto order = day_order(channel, day->day, pass);
    double accumulated = 0;
    for (int index : order) {
        const double length = channel.items[index].length_s;
        const double start = day->starts_at + (pass * channel.total_s + accumulated);
        const double end = day->starts_at + (pass * channel.total_s + accumulated + length);
        if (now_utc < end) {
            return OnAir{index, std::max(0.0, now_utc - start), start,
                         std::min(end, day->ends_at), day->ends_at};
        }
        accumulated += length;
    }
    return std::nullopt;
}

std::optional<OnAir> airing_after(const Channel& channel, const OnAir& airing,
                                  int utc_offset_s, const UtcOffsetAt& lookup) {
    return on_air(channel, airing.ends_at, utc_offset_s, lookup);
}

std::optional<double> back_on_air_at(const Channel& channel, double now_utc,
                                     int utc_offset_s, const UtcOffsetAt& lookup) {
    const auto day = day_window(channel, now_utc, utc_offset_s, lookup);
    if (!day || now_utc < day->ends_at) return std::nullopt;
    return day->next_start;
}

std::vector<OnAir> lineup(const Channel& channel, double from_utc, double to_utc,
                          int utc_offset_s, const UtcOffsetAt& lookup) {
    std::vector<OnAir> out;
    if (!std::isfinite(from_utc) || !std::isfinite(to_utc) || to_utc <= from_utc) return out;
    double cursor = from_utc;
    while (cursor < to_utc) {
        const auto airing = on_air(channel, cursor, utc_offset_s, lookup);
        if (!airing) {
            const auto back = back_on_air_at(channel, cursor, utc_offset_s, lookup);
            if (!back || *back <= cursor) break;
            cursor = *back;
            continue;
        }
        if (airing->ends_at <= cursor) break;  // malformed lengths must never loop
        out.push_back(*airing);
        cursor = airing->ends_at;
    }
    return out;
}

std::optional<OnAir> join_point(const Channel& channel, double now_utc,
                                int utc_offset_s, const UtcOffsetAt& lookup) {
    const auto airing = on_air(channel, now_utc, utc_offset_s, lookup);
    if (!airing || airing->ends_at - now_utc >= kMinJoinLeftS) return airing;
    return airing_after(channel, *airing, utc_offset_s, lookup);
}

std::optional<OnAir> boundary_join(const Channel& channel, double now_utc,
                                   const std::optional<LoadedAiring>& last,
                                   int utc_offset_s, const UtcOffsetAt& lookup) {
    const auto live = join_point(channel, now_utc, utc_offset_s, lookup);
    if (!live || !last) return live;
    if (last->utc_offset_s && *last->utc_offset_s != offset_at(now_utc, utc_offset_s, lookup)) {
        return live;
    }
    const int n = static_cast<int>(channel.items.size());
    if (last->index < 0 || last->index >= n) return live;
    const ScheduledItem& held = channel.items[last->index];
    if (held.playlist != last->item.playlist || held.item != last->item.item) return live;
    constexpr double kSlackS = 0.5;
    // Probe AT the start: a day-boundary cut can leave a final airing shorter
    // than the old 0.25-second probe. A changed daily lineup/length drops the
    // guard, while an unchanged crop-only rebuild retains it.
    const auto still = on_air(channel, last->starts_at, utc_offset_s, lookup);
    if (!still || still->index != last->index ||
        std::fabs(still->starts_at - last->starts_at) > kSlackS ||
        std::fabs(still->ends_at - last->ends_at) > kSlackS) return live;
    if (live->starts_at >= last->ends_at - kSlackS) return live;
    return airing_after(channel, *still, utc_offset_s, lookup);
}

}  // namespace app
