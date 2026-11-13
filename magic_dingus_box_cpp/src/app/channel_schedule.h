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

// AppState::last_channel_id of channel 00, the Master Shuffle mix (its row
// has no playlist file, so it has no playlist_identity of its own).
inline constexpr const char* kShuffleChannelId = "shuffle";

// Lexical only (no filesystem): strips a leading "./" and "data/", giving the
// media_info.json key form, e.g. "media/x.mp4".
std::string media_key_of_item(const PlaylistItem& item);

// The shortest item that is scheduled. A sub-second item would fill the
// guide's lineup with thousands of slots per layout, and could not confirm
// its own playback (Controller::update_state wants a position a second
// before the end) before its slot was over.
inline constexpr double kMinScheduledItemS = 3.0;

// One channel per row of ui_playlists. Row 0 is the Master Shuffle mix: every
// scheduled item of rows 1..N exactly once, in a deterministic order (a
// hand-written Fisher-Yates over std::mt19937 — std::shuffle differs between
// libc++ and libstdc++, and the Mac and the Pi must agree). Row 0's own items
// are ignored. Only local/video items with a known length of at least
// kMinScheduledItemS are scheduled: a trimmed item (start/end) is end - start
// long; otherwise the length is the duration in `info`, less the start trim.
// Anything else is left out.
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

// A tune or an item boundary joins the on-air item only with at least this
// much of it left; otherwise the item after it, from its start. The boundary
// fires up to 0.5 s before an item's end (decide_auto_advance), so rejoining
// the ending item would put the load inside its own end window with the
// once-per-item guard set: held at EOS for good.
inline constexpr double kMinJoinLeftS = 2.0;

// The airing right after `a` on `channel`, from its start. `a` must be one
// of the channel's airings (its index in range).
OnAir airing_after(const Channel& channel, const OnAir& a);

// What a tune joins on `channel` at `now_utc`: the on-air item at its
// scheduled offset, or — with under kMinJoinLeftS of it left — the item after
// it, from its start. nullopt: nothing is scheduled. The ONE join rule: the
// tune (PlaylistPlayback) and the guide's "joins at m:ss" both read it.
std::optional<OnAir> join_point(const Channel& channel, double now_utc);

// The airing a channel load last loaded (PlaylistPlayback keeps it).
struct LoadedAiring {
    int index = -1;       // into Channel::items
    ScheduledItem item;   // what that index held when it was loaded
    double starts_at = 0;
    double ends_at = 0;
};

// What an item boundary (the loaded item's natural end) joins: join_point,
// except that it never goes back to the airing last loaded, or to one
// before it. Playback can be AHEAD of the schedule at a natural end — the
// start seek snaps to a keyframe (KEY_UNIT | SNAP_NEAREST: on the Pi 5 it
// measured the one BEFORE the target, but nothing guarantees the side; web
// transcodes carry one every 2 s since 2026-11, older files and copied-in
// uploads ~8-10 s), a knob seek
// forward skipped part of the item, a file ends before its stored length —
// and then the on-air item is still the one that just ended: rejoining it
// would play its tail again. Instead the airing right after the loaded one,
// from its start (it has not started yet: the on-air airing is the loaded
// one or an earlier one). Once the schedule is past the loaded airing this
// is plain join_point. `last` is not trusted — plain join_point — when the
// channel no longer holds that item at that index, or no longer airs it at
// that time: a rebuild that changed the channel's cycle (an upload, a trim
// edit, the back-fill) re-phases every airing, and the live airing then
// starting before the loaded one's old end is not playback running ahead.
std::optional<OnAir> boundary_join(const Channel& channel, double now_utc,
                                   const std::optional<LoadedAiring>& last);

}  // namespace app
