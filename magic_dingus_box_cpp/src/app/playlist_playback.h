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

    // std::this_thread::sleep_for in the kiosk; the fake records it.
    virtual void sleep_for(std::chrono::milliseconds d) = 0;
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
    // After controller.update_state: clear is_switching_playlist if a
    // switch has been stuck for more than 2 s.
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
