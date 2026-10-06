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

#include <chrono>
#include <cstdint>
#include <string>

#include "app_state.h"
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
    virtual void load_next_item(AppState& state, const std::string& playlist_directory) = 0;
    virtual void load_previous_item(AppState& state, const std::string& playlist_directory) = 0;
    virtual void play_random_global_video(AppState& state,
                                          const std::string& playlist_directory) = 0;
    virtual void master_shuffle_advance(AppState& state,
                                        const std::string& playlist_directory) = 0;
    virtual void master_shuffle_back(AppState& state,
                                     const std::string& playlist_directory) = 0;

    // GstPlayer (the shared pipeline)
    virtual uint64_t stream_generation() const = 0;
    virtual bool has_error() const = 0;
    virtual double player_position() const = 0;

    // steady_clock::now() in the kiosk; a hand-advanced clock in the tests.
    // Every switch deadline and the stuck-switch timeout read this.
    virtual std::chrono::steady_clock::time_point now() const = 0;
};

class PlaylistPlayback {
public:
    // playlist_directory is held by reference: the runtime playlist reload
    // may resolve it for the first time on a box that booted without one.
    PlaylistPlayback(AppState& state, PlaylistTransport& transport,
                     const std::string& playlist_directory);

    // ── Main-menu input (called on the PRESS edge only) ──────────────────
    // SELECT on the main menu: show the hidden UI, toggle it over the
    // playing playlist, or switch to / start the highlighted playlist
    // (index 0 = Master Shuffle).
    void on_select();
    void on_play_pause();
    // NEXT/PREV: step the playlist (or Master Shuffle history) while one
    // plays; seek +/-10 s otherwise.
    void on_next();
    void on_prev();

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

    // ── Failed-item handling (exposed for the tests) ─────────────────────
    // True while a main-menu playlist item owns the shared pipeline (not
    // the intro, a switch, a game launch, or Media Browser playback).
    bool owns_pipeline() const;
    int failure_budget() const;
    void act_on_failed_item(video::PlaybackErrorPolicy::Decision d, const char* why);

private:
    AppState& state_;
    PlaylistTransport& transport_;
    const std::string& playlist_directory_;

    // A mid-playback switch between its stop() and its load.
    enum class SwitchPhase { Idle, Settling, Polling };
    struct PendingSwitch {
        SwitchPhase phase = SwitchPhase::Idle;
        int target = -1;  // the selected_index SELECT was pressed on
        std::chrono::steady_clock::time_point next_check{};
        int repolls = 0;  // 50 ms re-polls used so far
    };
    PendingSwitch pending_;
    // Ends the 200 ms settle after the stuck-switch timeout's stop();
    // SELECT is ignored until then. Default (epoch) = no settle.
    std::chrono::steady_clock::time_point timeout_settle_until_{};

    // One step of the pending switch's wait; runs the load when it is due.
    void advance_pending_switch();
    // Why a pending switch can no longer load (nullptr = it still can).
    const char* pending_switch_obsolete_reason() const;
    // The tail of the old blocking switch: warning + load + failure path.
    void finish_switch(int target);

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
