#pragma once

// The Channel Guide's schedule: every video playlist is a TV channel that has
// been "on air" since a fixed wall-clock epoch, so what is playing right now
// (and what comes next) is a pure function of the time. Pure logic, no I/O,
// unit-tested on the Mac (tests/app/test_channel_schedule.cpp).
//
// Design: docs/superpowers/specs/2026-11-12-channel-guide-design.md.

#include <optional>
#include <string>
#include <vector>

#include "app_state.h"
#include "media_info.h"

namespace app {

// 2026-01-01T00:00:00Z as Unix seconds. Every channel's first item starts here.
constexpr double kScheduleEpochUtc = 1767225600.0;

struct ScheduledItem {
    int playlist = 0;  // index into state.playlists
    int item = 0;      // index into that playlist's items
    double length_s = 0;
};

struct Channel {
    int number = 0;  // equals the row index in the UI playlist list
    std::vector<ScheduledItem> items;
    double total_s = 0;
};

// Lexical only (no filesystem): strips a leading "./" and "data/", giving the
// media_info.json key form, e.g. "media/x.mp4".
std::string media_key_of_item(const PlaylistItem& item);

// One channel per row of ui_playlists. Row 0 is the Master Shuffle mix: every
// scheduled item of rows 1..N exactly once, in a deterministic order (a
// hand-written Fisher-Yates over std::mt19937 — std::shuffle differs between
// libc++ and libstdc++, and the Mac and the Pi must agree). Row 0's own items
// are ignored. Only local/video items with a known length are scheduled: a
// trimmed item (start/end) is end - start long; otherwise the length is the
// duration in `info`. Anything else is left out.
std::vector<Channel> build_channels(const std::vector<Playlist>& ui_playlists,
                                    const MediaInfoMap& info);

struct OnAir {
    int index = 0;         // index into Channel::items
    double offset_s = 0;   // seconds into that item
    double starts_at = 0;  // UTC seconds the item started
    double ends_at = 0;    // UTC seconds the item ends
};

// What is airing at now_utc (the schedule loops, and extends before the epoch).
// nullopt for an empty channel.
std::optional<OnAir> on_air(const Channel& channel, double now_utc);

// Every item airing at any point in [from_utc, to_utc), the first one
// possibly already under way.
std::vector<OnAir> lineup(const Channel& channel, double from_utc, double to_utc);

}  // namespace app
