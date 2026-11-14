#pragma once

// The Channel Guide's schedule: every video playlist is a TV channel that has
// a deterministic daily lineup. Local civil boundaries and elapsed programme
// durations are calculated from injected time facts. Pure logic, no I/O,
// unit-tested on the Mac (tests/app/test_channel_schedule.cpp).
//
// Daily lineups reset at local 4 AM; movie channels air local 6 AM–1 AM.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "app_state.h"
#include "media_info.h"

namespace app {

enum class LineupOrder { Rotate, Shuffle };
inline constexpr int kLineupDayStartH = 4;
inline constexpr int kMovieDayStartH = 6;
inline constexpr int kMovieOnAirH = 19;

// Optional time-zone rules, including offsets at past/future boundaries. An
// empty lookup uses the supplied fixed offset. The kiosk supplies warmed
// localtime_r; tests supply pure transition tables. No time-zone I/O here.
using UtcOffsetAt = std::function<int(double)>;

struct ScheduledItem {
    int playlist = 0;  // index into state.playlists
    int item = 0;      // index into that playlist's items
    double length_s = 0;
};

struct Channel {
    int number = 0;  // equals the row index in the UI playlist list
    std::vector<ScheduledItem> items;
    double total_s = 0;
    std::string identity;
    LineupOrder order = LineupOrder::Rotate;
    int day_start_h = kLineupDayStartH;
    int on_air_h = 24;
};

// AppState::last_channel_id of channel 00, the Master Shuffle mix (its row
// has no playlist file, so it has no playlist_identity of its own).
inline constexpr const char* kShuffleChannelId = "shuffle";

inline constexpr const char* kMovieChannelPrefix = "movies:";
// Lexical identity check, available even in builds without Media Browser.
inline bool is_movie_playlist(const Playlist& playlist) {
    return playlist.path.rfind(kMovieChannelPrefix, 0) == 0;
}

// Lexical only (no filesystem): strips a leading "./" and "data/", giving the
// media_info.json key form, e.g. "media/x.mp4".
std::string media_key_of_item(const PlaylistItem& item);

// The shortest item that is scheduled. A sub-second item would fill the
// guide's lineup with thousands of slots per layout, and could not confirm
// its own playback (Controller::update_state wants a position a second
// before the end) before its slot was over.
inline constexpr double kMinScheduledItemS = 3.0;

// One channel per row of ui_playlists. Row 0 is the Master Shuffle mix: every
// scheduled non-movie item of rows 1..N exactly once, in written row order.
// day_order applies the daily rotation/shuffle without changing these indexes.
// Row 0's own items are ignored. Only local/video items with a known length of at least
// kMinScheduledItemS are scheduled: a trimmed item (start/end) is end - start
// long; otherwise the length is the duration in `info`, less the start trim.
// Anything else is left out.
std::vector<Channel> build_channels(const std::vector<Playlist>& ui_playlists,
                                    const MediaInfoMap& info);

struct OnAir {
    int index = 0;         // index into Channel::items
    double offset_s = 0;   // seconds into that item
    double starts_at = 0;  // UTC seconds the item started
    double ends_at = 0;    // UTC seconds the item ends (possibly cut)
    double day_ends_at = 0;  // civil broadcast close: live playback cuts even if it lags
};

// A permutation of Channel::items indexes. Portable FNV-1a seed + mt19937
// Fisher-Yates; Rotate holds the written succession for every pass that day.
std::vector<int> day_order(const Channel&, int64_t day, int pass);

// What is airing at now_utc. nullopt for an empty or off-air channel.
std::optional<OnAir> on_air(const Channel& channel, double now_utc, int utc_offset_s,
                            const UtcOffsetAt& utc_offset_at = {});

// Every item airing at any point in [from_utc, to_utc), the first one
// possibly already under way.
std::vector<OnAir> lineup(const Channel& channel, double from_utc, double to_utc,
                          int utc_offset_s, const UtcOffsetAt& utc_offset_at = {});

// A tune or an item boundary joins the on-air item only with at least this
// much of it left; otherwise the item after it, from its start. The boundary
// fires up to 0.5 s before an item's end (decide_auto_advance), so rejoining
// the ending item would put the load inside its own end window with the
// once-per-item guard set: held at EOS for good.
inline constexpr double kMinJoinLeftS = 2.0;

// The airing right after `a` on `channel`, from its start. `a` must be one
// of the channel's airings. nullopt when an off-air interval follows.
std::optional<OnAir> airing_after(const Channel& channel, const OnAir& a,
                                    int utc_offset_s, const UtcOffsetAt& utc_offset_at = {});

// Skip a programme already watched, including a repeat across shuffled
// passes. At most one complete following pass; exhausted channels return
// no candidate. Playback and the on-demand panel share this bounded rule.
template <typename Matches>
std::optional<OnAir> skip_matching_programmes(const Channel& channel,
                                               std::optional<OnAir> candidate,
                                               Matches matches, int utc_offset_s,
                                               const UtcOffsetAt& utc_offset_at = {}) {
    for (size_t n = 0; candidate && n <= channel.items.size(); ++n) {
        if (!matches(channel.items[candidate->index])) return candidate;
        const int offset = utc_offset_at ? utc_offset_at(candidate->ends_at) : utc_offset_s;
        candidate = airing_after(channel, *candidate, offset, utc_offset_at);
        if (candidate) candidate->offset_s = 0;
    }
    return std::nullopt;
}

// What a tune joins on `channel` at `now_utc`: the on-air item at its
// scheduled offset, or — with under kMinJoinLeftS of it left — the item after
// it, from its start. nullopt: nothing is scheduled. The ONE join rule: the
// tune (PlaylistPlayback) and the guide's "joins at m:ss" both read it.
std::optional<OnAir> join_point(const Channel& channel, double now_utc, int utc_offset_s,
                                 const UtcOffsetAt& utc_offset_at = {});

// The airing a channel load last loaded (PlaylistPlayback keeps it).
struct LoadedAiring {
    int index = -1;       // into Channel::items
    ScheduledItem item;   // what that index held when it was loaded
    double starts_at = 0;
    double ends_at = 0;
    // Offset at the load: a zone change rejoins live once even when a
    // whole number of short passes leaves the old airing apparently valid.
    std::optional<int> utc_offset_s;
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
// edit, the back-fill) re-phases that day's airings, and the live airing then
// starting before the loaded one's old end is not playback running ahead.
std::optional<OnAir> boundary_join(const Channel& channel, double now_utc,
                                   const std::optional<LoadedAiring>& last, int utc_offset_s,
                                   const UtcOffsetAt& utc_offset_at = {});

// Next civil day start while off air; nullopt when on air or empty.
std::optional<double> back_on_air_at(const Channel&, double now_utc, int utc_offset_s,
                                     const UtcOffsetAt& utc_offset_at = {});

}  // namespace app
