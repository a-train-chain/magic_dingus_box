#pragma once

// Channel Guide style: "nothing playing" is never a resting state. Every
// path that leaves the main screen with no video — power-on (the intro ends,
// there is no intro file, it fails to load, it times out), the post-game
// ready edge, leaving the Media Browser, closing Settings, a failed item's
// scheduled slot ending, a give-up — is caught by ONE rule evaluated every
// frame, not a hook per path (a hook per path is how one gets missed).
//
// Pure logic, no AppState, no I/O; unit-tested on the Mac
// (tests/app/test_auto_tune.cpp). main.cpp gathers the inputs once per
// frame and, on a result, calls PlaylistPlayback::tune(channel,
// /*keep_overlay=*/false): the video simply comes back, the guide is not
// opened. List style never tunes.
//
// Design: docs/superpowers/specs/2026-11-12-channel-guide-design.md,
// "When nothing would be playing".

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace app {

struct AutoTuneInput {
    bool guide_style = false;        // Menu Style is Channel Guide
    bool intro_complete = false;     // the boot intro is over, whichever way
    // The media info store's first read attempt is over, in any outcome
    // (MediaInfoStore::first_read_finished), and the channels were rebuilt
    // from what it read. Before that only trimmed items have a length: on
    // the no-intro / failed-intro power-on paths a tune could pick another
    // channel and overwrite the one last watched. A read that failed lifts
    // it too (the pre-gate behaviour), so an unreadable file never stops
    // the box tuning on its own.
    bool media_info_read = false;
    bool on_main_screen = false;     // no Media Browser, Settings or keyboard up
    bool video_active = false;       // a video plays, or the next item loads
    bool switch_pending = false;     // a switch or tune has not got its stream up
    bool loading_game = false;       // a game is launching
    bool post_game_pending = false;  // the post-game display restore is not done
    double now_utc = 0.0;            // wall clock, UTC seconds
    double wait_until_utc = 0.0;     // a failed item's slot ends (0 = no wait)
    int last_channel = -1;           // the last channel watched; -1 = none
    std::vector<bool> tunable;       // per channel (= UI row): has a schedule
    std::vector<int> gave_up;        // channels given up on, newest last
};

// The gates alone: guide style, the intro over, the media info store read,
// on the main screen, no video, no switch pending, no game loading, the
// post-game restore done, and now_utc >= wait_until_utc. Reads only those
// fields, so main.cpp fills
// last_channel / tunable / gave_up (an identity scan) only when it is true —
// never on the frames a video plays.
bool auto_tune_gates_open(const AutoTuneInput& in);

// The channel to tune now, or nullopt. Tunes only when auto_tune_gates_open.
// A channel can be picked when it has a schedule and was not given up on.
// The target, in order:
//   - the last channel watched;
//   - after a give-up, the next channel up from the most recent given-up one,
//     wrapping (re-tuning the channel that just failed repeatedly would loop);
//     when none is left, nothing — every channel that could play failed;
//   - channel 01;
//   - the lowest channel;
//   - else nothing.
std::optional<int> decide_auto_tune(const AutoTuneInput& in);

// ── An orphan stream (defence in depth) ─────────────────────────────────
// The rule above reads "video_active, or the playing indexes set" as
// "something plays". A stream the kiosk believes is up (video_active) that
// no channel owns (the indexes unset) and that is not playing is neither,
// and nothing ends it: it never reaches EOS, the stall watchdog only
// watches a stream that reports playing, and the rule never tunes over it.
// Pi 5, 2026-11-13 13:02: after a game the re-initialized player reported
// the torn-down stream's last position and duration (fixed at the cause,
// GstPlayer::cleanup), and the box sat on a black guide window until a
// channel button was pressed. Whatever produces that state next, it is not
// a resting state: once it has lasted OrphanStreamGuard::kBoundS, main.cpp
// stops the stream and resets the main UI (controller.stop(), then
// app::stop_to_menu), and the rule tunes on the same frame. List style
// never stops anything here (main.cpp only evaluates this in guide style;
// `guide_style` stays in the struct so the predicate's own List-style gate
// is unit-tested).
//
// Edge also covered: a web-admin playlist reload that empties the playing
// playlist leaves current_item_index -1 with current_playlist_index still
// set. If the viewer had paused that video in guide style, the guard stops
// it after the bound and re-tunes. That deliberately overrides reload's
// "never interrupted" rule: a stream with no channel behind it must not rest.
struct OrphanStreamInput {
    bool guide_style = false;
    bool intro_complete = false;     // the intro plays with no indexes by design
    // Not under the Media Browser (its playback has no indexes by design),
    // Settings or the keyboard — the rule's own on_main_screen.
    bool on_main_screen = false;
    bool video_active = false;       // AppState::video_active itself
    bool indexes_set = false;        // current_playlist_index and current_item_index >= 0
    bool playing = false;            // the player reports playing
    bool switch_pending = false;     // a switch or tune has not got its stream up
    bool loading_game = false;
    bool post_game_pending = false;  // the post-game display restore is not done
};

bool is_orphan_stream(const OrphanStreamInput& in);

// Times an orphan. Long enough that a real stop's video_active debounce
// (Controller::update_state holds it through a few negative reads) never
// trips it; short enough to read as a hiccup, not a hang.
class OrphanStreamGuard {
public:
    static constexpr double kBoundS = 2.0;
    // Once per frame, `now_s` = steady clock seconds. True on the frame an
    // orphan has lasted kBoundS: the caller stops it. A frame that is not an
    // orphan restarts the count, and so does a stop — an orphan that
    // survives it gets a whole new bound, never a stop per frame.
    bool should_stop(bool orphan, double now_s);

private:
    std::optional<double> since_;
};

// main.cpp's memo of AutoTuneInput::last_channel. Resolving the last channel
// watched to its row scans the playlists building an identity string per
// row; in the resting states the gates stay open every frame (no channel has
// a schedule, every channel was given up), so it is resolved again only when
// the id or the channel list (its rebuild count: in guide style every
// playlist reload rebuilds it, and so does entering guide style) changed. The
// comparison allocates nothing; the id is copied only when it changed.
class LastChannelRowCache {
public:
    template <class Resolve>
    int get(const std::string& id, uint64_t channels_generation, Resolve&& resolve) {
        if (!valid_ || channels_generation != generation_ || id != id_) {
            row_ = std::forward<Resolve>(resolve)(id);
            id_ = id;
            generation_ = channels_generation;
            valid_ = true;
        }
        return row_;
    }

private:
    bool valid_ = false;
    std::string id_;
    uint64_t generation_ = 0;
    int row_ = -1;
};

}  // namespace app
