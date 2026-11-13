#pragma once

// Main-menu playlist playback control: starting a playlist (or Master
// Shuffle) from SELECT, NEXT/PREV stepping, play/pause, natural-end
// auto-advance, the stuck-switch timeout, and what happens when an item
// fails (pipeline error or a stall the watchdog could not revive).
//
// Moved out of main.cpp's main loop verbatim — same order of side effects,
// same log lines. main.cpp calls each entry point at the exact place the
// inline code used to run; this class only owns the decisions and their
// sequencing. Every Controller/GstPlayer call goes through PlaylistTransport,
// so the whole thing runs on the Mac against a recording fake
// (tests/app/test_playlist_playback.cpp). The kiosk adapter is
// app/controller_transport.h.
//
// ── The playlist switch never blocks the render thread ───────────────────
// Switching playlists mid-playback used to stop the pipeline and then SLEEP
// on the render thread — 200 ms to let the old stream's buffers go, then up
// to 10 x 50 ms re-polls while the player still reported playing — so the
// picture, the menu and the phone remote froze for up to ~0.7 s per switch
// (and the stuck-switch timeout slept another 200 ms after its stop). The
// same waits now run as a per-frame state machine:
//
//   on_select(): ...state writes... -> stop() -> [pending: Settling]
//   tick_switch_timeout(), each frame:
//     Settling  until 200 ms after the stop
//     Polling   is_playing() && polls < 10 ? wait 50 ms more : finish
//     finish    "did not stop cleanly" warning if still playing -> load
//
// Side effects, their order, the poll count and the end states are the old
// blocking code's exactly; only the waiting is cooperative. The load runs
// from tick_switch_timeout() — the per-frame tick main.cpp already calls
// after controller.update_state — immediately before the stuck-switch
// timeout check, the same order as before (load, then the timeout check, in
// one main-loop iteration). The redraw gate keeps drawing throughout: it
// treats is_switching_playlist as video activity.
//
// RE-ENTRANCY while a switch is pending (the old code queued these inputs
// behind the freeze; there is nothing sensible to do with them against a
// stopped pipeline, so they are dropped, matching how NEXT/PREV were
// already ignored for the whole switch):
//   - SELECT, NEXT, PREV, PLAY/PAUSE: ignored until the load has run.
//   - Rotating the menu cursor: allowed; the switch loads the playlist that
//     was SELECTED (captured at the press), never the one now highlighted.
//   - Anything that takes the pipeline away first — Media Browser entry
//     (clears is_switching_playlist), a game session (resets
//     current_playlist_index on return), the intro — ABANDONS the pending
//     load: it never runs, the state is left to whoever took over, and the
//     stuck-switch timeout releases the flag exactly as it did before.
//   - Shutdown: the pending load is simply never run.
//   - The stuck-switch timeout never fires while the switch's own bounded
//     wait is in progress.
// After the stuck-switch timeout's recovery stop(), the old 200 ms settle
// sleep is a 200 ms window in which SELECT is ignored, so no new load can
// follow that stop sooner than it could before.
//
// ── Channel Guide style (set_channels non-null) ──────────────────────────
// Every video playlist is a live channel (app/channel_schedule.h). tune()
// rides the switch above (same stop, waits, re-entrancy rules and timeout;
// pending_.target is the channel) and joins the on-air item when the load
// runs. NEXT/PREV change channel. An item's natural end loads what is on the
// tuned channel NOW instead of the next index — never the airing that just
// ended, when playback ran ahead of it (app::boundary_join: the next one,
// from its start). A failed item is not skipped
// ahead: playback stops until its scheduled slot ends
// (AppState::guide_wait_until_utc), and a give-up marks the channel in
// guide_gave_up — cleared only once a channel PLAYS (kChannelPlaysS), so a
// library that loads but never decodes runs out of channels; main.cpp's
// nothing-playing rule re-tunes in both cases. A stream that never comes up
// is a failed item too: kChannelStartTimeout after its load returned, for a
// tune and a boundary alike.
// The List style (nullptr) never reaches any of this.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "app_state.h"
#include "channel_schedule.h"
#include "playback_stall_watchdog.h"
#include "../utils/result.h"
#include "../video/playback_error_policy.h"

namespace app {

// The Controller / GstPlayer operations playlist playback needs. One method
// per call the old inline code made, forwarding 1:1 in the kiosk.
class PlaylistTransport {
public:
    virtual ~PlaylistTransport() = default;

    // Controller
    virtual void stop() = 0;
    virtual void play() = 0;
    virtual void toggle_pause() = 0;
    virtual void seek(double seconds) = 0;
    virtual bool is_playing() const = 0;
    virtual bool is_paused() const = 0;
    virtual utils::Result<> load_playlist_item(AppState& state,
                                               const Playlist& playlist,
                                               int item_index,
                                               const std::string& playlist_directory) = 0;
    // load_playlist_item, joining the item offset_s seconds past its own
    // start (a Channel Guide tune: the item's scheduled position).
    virtual utils::Result<> load_playlist_item_at(AppState& state,
                                                  const Playlist& playlist,
                                                  int item_index,
                                                  const std::string& playlist_directory,
                                                  double offset_s) = 0;
    virtual void load_next_item(AppState& state, const std::string& playlist_directory) = 0;
    virtual void load_previous_item(AppState& state, const std::string& playlist_directory) = 0;
    virtual void play_random_global_video(AppState& state,
                                          const std::string& playlist_directory) = 0;
    virtual void master_shuffle_advance(AppState& state,
                                        const std::string& playlist_directory) = 0;
    virtual void master_shuffle_back(AppState& state,
                                     const std::string& playlist_directory) = 0;
    // Flip the playing video's crop setting (the Picture control). In the
    // kiosk: MediaInfoStore::set_crop for Controller::current_media_key —
    // in memory at once, written to disk on the store's own thread.
    virtual void toggle_picture_crop() = 0;

    // GstPlayer (the shared pipeline)
    virtual uint64_t stream_generation() const = 0;
    virtual bool has_error() const = 0;
    virtual double player_position() const = 0;

    // steady_clock::now() in the kiosk; a hand-advanced clock in the tests.
    // Every switch deadline and the stuck-switch timeout read this.
    virtual std::chrono::steady_clock::time_point now() const = 0;
    // Wall-clock UTC seconds (system_clock in the kiosk; hand-set in the
    // tests). Only the Channel Guide schedule reads it; waits stay steady.
    virtual double wall_now_utc() const = 0;
};

class PlaylistPlayback {
public:
    // playlist_directory is held by reference: the runtime playlist reload
    // may resolve it for the first time on a box that booted without one.
    PlaylistPlayback(AppState& state, PlaylistTransport& transport,
                     const std::string& playlist_directory);

    // ── Main-menu input (called on the PRESS edge only) ──────────────────
    // SELECT on the main menu: show the hidden UI, toggle the playing
    // video's crop when the cursor is on the Picture control, toggle the UI
    // over the playing playlist, or switch to / start the highlighted
    // playlist (index 0 = Master Shuffle).
    void on_select();
    void on_play_pause();
    // NEXT/PREV: step the playlist (or Master Shuffle history) while one
    // plays; seek +/-10 s otherwise. In Channel Guide style: channel up /
    // down (on_channel_step).
    void on_next();
    void on_prev();

    // ── Channel Guide style (live channels) ──────────────────────────────
    // The channel list (app/channel_schedule.h; channel N = UI row N), or
    // nullptr for the List style — the default, and today's behaviour on
    // every path. Held by pointer and read afresh at every tune and item
    // boundary, so a rebuilt list takes effect at the next one. Call it
    // again after EVERY rebuild (even into the same vector): a reload
    // renumbers the rows, so the tuned channel is re-found by
    // AppState::last_channel_id (-1 when its playlist is gone). The
    // guide_gave_up rows are NOT re-anchored — a reload can shift them —
    // and live until a channel proves it plays (kChannelPlaysS); only rows
    // past the new list's end are dropped. nullptr also forgets the tuned /
    // given-up channels.
    void set_channels(const std::vector<Channel>* channels);
    bool guide_style() const { return channels_ != nullptr; }
    // Tune `channel` live. The playlist switch above, unchanged — stop now,
    // then the settle / re-poll wait per frame, then the load — except that
    // the load joins whatever is on air when it RUNS (on_air at
    // wall_now_utc() then), so the wait does not put playback behind. Even
    // from a stopped pipeline, so every tune computes its offset the same
    // way. The load sets tuned_channel and last_channel_id ("shuffle" for
    // 00) whether or not the item loads — a failed item is the channel's
    // to resume (see act_on_failed_item); success hides the overlay unless
    // keep_overlay, or the viewer owns the guide when the load runs
    // (AppState::guide_opened_by_viewer). It never clears guide_gave_up
    // (kChannelPlaysS).
    // Dropped (like SELECT) in List style, for a channel with no schedule,
    // during the intro, and while a switch is pending or in flight.
    void tune(int channel, bool keep_overlay);
    // Channel up (+1) / down (-1): the next channel with a schedule,
    // wrapping (from nothing tuned, up starts at 00 and down at the last
    // channel). The overlay stays as it is — as it is when the load runs,
    // so a guide the viewer opens during the wait stays open.
    void on_channel_step(int delta);
    // The channel a tune() (or channel up/down) will load, from its stop
    // until the load runs; nullopt otherwise (no switch pending, or a List
    // SELECT's). tuned_channel only changes at the load, ~200 ms after the
    // press: main.cpp moves the guide's cursor onto this as the step is
    // accepted, and a press on its row closes the guide.
    std::optional<int> pending_tune() const {
        if (!switch_pending() || !pending_.tune) return std::nullopt;
        return pending_.target;
    }
    // A tune joins app::join_point (the on-air item, or the next one from its
    // start with under this much left); an item's natural end joins
    // app::boundary_join, which never goes back to the airing that just
    // ended (playback can be ahead of the schedule there). Both rules live
    // in app/channel_schedule.h, shared with the guide's view.
    static constexpr double kMinJoinLeftS = app::kMinJoinLeftS;
    // A channel list older than the playlists it points into (a rebuild
    // that was missed) stops playback and holds the nothing-playing rule
    // off for this long, so it can never become a stop-and-tune cycle every
    // ~200 ms (guide_wait_until_utc).
    static constexpr double kStaleScheduleWaitS = 2.0;
    // A channel has proven it plays — and guide_gave_up is cleared, so the
    // channels given up on get another try — once its stream is this far
    // past the first position it reported while playing, or an item reached
    // its natural end. A load alone proves nothing: a library whose files
    // all load but fail to decode must still run every channel out instead
    // of cycling. A load whose stream never comes up within
    // kChannelStartTimeout is a failed item, like a failed load.
    static constexpr double kChannelPlaysS = 5.0;

    // ── Per-frame ticks, in main-loop order ──────────────────────────────
    // After controller.update_state: advance a pending playlist switch
    // (load the new playlist once the old stream has settled — see the
    // header comment), then clear is_switching_playlist if a switch has
    // been stuck for more than 2 s.
    void tick_switch_timeout();
    // Pipeline error on a playlist item -> skip it (or give up after a run
    // of failures).
    void tick_pipeline_error();
    // Natural end of the current item -> next item / Master Shuffle pick.
    // A hold at the end (already advanced / playback not confirmed) is
    // logged once per hold, not once per frame.
    void tick_auto_advance();
    // The playback stall watchdog: restart a pipeline that silently
    // stopped, or give up on the item after repeated restarts. `now_sec` is
    // steady_clock seconds.
    void tick_stall_watchdog(double now_sec);

    // ── Playlist switch state ────────────────────────────────────────────
    // True from a mid-playback switch's stop() until its load has run (or
    // the switch was abandoned).
    bool switch_pending() const { return pending_.phase != SwitchPhase::Idle; }
    // The waits, unchanged from the old blocking code.
    static constexpr std::chrono::milliseconds kSwitchSettle{200};
    static constexpr std::chrono::milliseconds kSwitchRepoll{50};
    static constexpr int kSwitchMaxRepolls = 10;
    static constexpr std::chrono::milliseconds kSwitchTimeout{2000};
    static constexpr std::chrono::milliseconds kTimeoutSettle{200};
    // Channel Guide: how long a channel load — a tune's or an item
    // boundary's — has to bring its stream up, counted from when the load
    // call RETURNS (not from the press: the settle, the re-polls and
    // load_item_media's own blocking settle are not the stream's time).
    // Only past it does the load count as a failed item
    // (tick_pipeline_error), and a failed item darkens the channel for the
    // rest of its slot — so the bound is generous: media lives on the SD
    // card, and a Content Manager upload stalls it for seconds at a time.
    // The List switch timeout (kSwitchTimeout) is not this: it releases a
    // tune's switch flag at 2 s as before, but neither fails the item nor
    // stops a stream still within this bound.
    static constexpr std::chrono::milliseconds kChannelStartTimeout{8000};

    // ── Who owns the pipeline ────────────────────────────────────────────
    // True while a main-menu playlist item owns the shared pipeline (not
    // the intro, a switch, a game launch, or Media Browser playback). Also
    // read by main.cpp each frame for the overlay's Picture control
    // (app/overlay_nav.h, PictureFacts::owns_pipeline).
    bool owns_pipeline() const;

    // ── Failed-item handling (exposed for the tests) ─────────────────────
    int failure_budget() const;
    void act_on_failed_item(video::PlaybackErrorPolicy::Decision d, const char* why);

private:
    AppState& state_;
    PlaylistTransport& transport_;
    const std::string& playlist_directory_;
    const std::vector<Channel>* channels_ = nullptr;

    // A mid-playback switch between its stop() and its load.
    enum class SwitchPhase { Idle, Settling, Polling };
    struct PendingSwitch {
        SwitchPhase phase = SwitchPhase::Idle;
        int target = -1;  // the selected_index SELECT was pressed on (a tune: the channel)
        std::chrono::steady_clock::time_point next_check{};
        int repolls = 0;  // 50 ms re-polls used so far
        bool tune = false;          // a Channel Guide tune, not a SELECT
        bool keep_overlay = false;  // tune(): leave the overlay as it is
    };
    PendingSwitch pending_;
    // The player's stream generation when the switching flag was raised.
    // A different generation that is playing = the switch's new item is
    // up (see tick_switch_timeout).
    uint64_t switch_start_generation_ = 0;
    // Ends the 200 ms settle after the stuck-switch timeout's stop();
    // SELECT is ignored until then. Default (epoch) = no settle.
    std::chrono::steady_clock::time_point timeout_settle_until_{};

    // One step of the pending switch's wait; runs the load when it is due.
    void advance_pending_switch();
    // Why a pending switch can no longer load (nullptr = it still can).
    const char* pending_switch_obsolete_reason() const;
    // The tail of the old blocking switch: warning + load + failure path.
    void finish_switch(int target);
    // The end of a switch's bounded wait: warns if the player still plays.
    void warn_if_still_playing() const;

    // ── Channel Guide ────────────────────────────────────────────────────
    // Channel `c` of the current list (nullptr in List style / out of range).
    const Channel* channel_at(int c) const;
    // A tune's load, once its switch wait is over.
    void finish_tune(int channel, bool keep_overlay);
    // An item's natural end: load what is on the tuned channel now — but
    // never the airing just loaded again (app::boundary_join).
    void rejoin_schedule();
    // tuned_channel + last_channel_id, set when a tune's load runs.
    void land_on_channel(int channel);
    // Load channel `channel`'s scheduled item `oa` at its offset; true when
    // the load returned OK.
    bool load_on_air(int channel, const OnAir& oa, bool keep_overlay);
    // Scheduled end (UTC) of the channel item last loaded: when it fails,
    // the channel picks up again from there.
    double guide_item_ends_at_ = 0.0;
    // The airing the last channel load loaded, and on which channel: the
    // next boundary on that channel never rejoins it (boundary_join). A
    // reload that renumbers the channel drops the guard for that one
    // boundary (the channel number or the scheduled item no longer match).
    struct ChannelAiring {
        int channel = -1;
        LoadedAiring airing;
    };
    std::optional<ChannelAiring> loaded_airing_;
    // How far the playing channel load has got. A mid-item join starts far
    // past the failure policy's "healthy" mark (3 s) and kChannelPlaysS, so
    // on a channel both count from `anchor`: the FIRST position the stream
    // reports once it plays (video_active) — not the offset the load asked
    // for, since the start seek snaps to a keyframe (KEY_UNIT |
    // SNAP_NEAREST), seconds either side, and GstPlayer reports the
    // requested start until it lands. Before the anchor, nothing has played.
    struct ChannelProgress {
        bool loaded = false;           // the playing stream is a channel load
        std::optional<double> anchor;  // first position once playing
        bool proven = false;           // prove_channel() has run for it
    };
    ChannelProgress channel_progress_;
    // A channel load (a tune's or a boundary's) returned OK at this time
    // (steady clock) and its stream has not come up yet (playback_started_).
    // Dropped when it comes up, when anything else takes the pipeline
    // (channel_load_live), at the next tune, and when a failure is counted —
    // so it is never counted twice. Past kChannelStartTimeout it is a failed
    // item.
    std::optional<std::chrono::steady_clock::time_point> channel_load_at_;
    // The channel load's stream can still come up: its indexes are set and
    // nothing else (the intro, a game, the Media Browser) has the pipeline.
    // owns_pipeline() without the switch flag — a tune's load is waited for
    // while its switch flag is still up.
    bool channel_load_live() const;
    // Drops channel_load_at_ once the stream is up or the load was taken
    // away. Called at the top of both ticks that read it.
    void drop_settled_channel_load();
    // Seconds played since the anchor (taking it on the first playing
    // frame); the player's own position when the stream is not a channel
    // load (List style).
    double played_position(double player_pos);
    // The channel plays: clear guide_gave_up (once per load).
    void prove_channel();

    // A GStreamer error (corrupt/truncated file, unsupported codec) or a
    // stall the watchdog could not revive used to strand an unattended
    // kiosk on the dead item forever: an errored stream never reaches
    // position >= duration, so the natural-end auto-advance never fired.
    // video::PlaybackErrorPolicy decides skip vs. give up (bounded, so a
    // playlist where EVERY item is broken shows the UI instead of
    // spinning); act_on_failed_item executes the decision.
    video::PlaybackErrorPolicy error_policy_;

    // Watches for the pipeline stalling while the kiosk thinks it is playing.
    PlaybackStallWatchdog stall_watchdog_;
    uint64_t watchdog_generation_ = 0;

    // The auto-advance hold log ("NOT auto-advancing") prints once per
    // hold and again only when its reason changes — never per frame.
    struct HeldReason {
        int item = -1;
        int last_advanced = -1;
        bool playback_started = false;
        bool operator==(const HeldReason& o) const {
            return item == o.item && last_advanced == o.last_advanced &&
                   playback_started == o.playback_started;
        }
    };
    bool held_logged_ = false;
    HeldReason held_reason_;
};

}  // namespace app
