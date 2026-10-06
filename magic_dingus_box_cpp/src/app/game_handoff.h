#pragma once

// The kiosk side of a RetroArch game session that is NOT drawing: what
// brackets every session (Controller::set_game_session_hooks), the systemd
// watchdog messages around it, quieting the torrent/media stack for the
// game, and resuming torrents a previous session left paused.
//
// Moved out of main.cpp verbatim (same order of side effects, same log
// lines); every external effect is an injected callback, so this runs on
// the Mac (tests/retroarch/test_game_handoff.cpp). The GL/DRM half — the
// loading-screen launch from the Settings game browser and the post-game
// display restore — is app/game_handoff_kiosk.h.

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "app_state.h"
#include "game_quiet_mode.h"
#include "post_game_gate.h"
#include "../retroarch/launch_contract.h"

namespace app {

// ── systemd watchdog ─────────────────────────────────────────────────────
// `notify` is sd_notify(0, msg) on a libsystemd build and EMPTY otherwise
// (then nothing is sent, as the old #else branch did nothing).
using SystemdNotify = std::function<void(const char*)>;

// The launcher's supervised-play-phase watchdog control
// (Controller::set_session_watchdog): Arm = WATCHDOG_USEC=10000000 (=
// WatchdogSec=10) + WATCHDOG=1, Ping = WATCHDOG=1, Disarm = WATCHDOG_USEC=0.
void notify_session_watchdog(retroarch::SessionWatchdog ev, const SystemdNotify& notify);

// ── Session bracketing ───────────────────────────────────────────────────
// Installed on the controller so EVERY route into an emulated_game item gets
// it — main-UI SELECT on a mixed playlist, NEXT/PREV, auto-advance at video
// end, Master Shuffle, and the Settings game browser. The Settings branch
// used to inline this and the other four routes had none: the watchdog
// stayed armed while load_playlist_item blocked in waitpid, so ~10s into any
// game launched outside Settings, systemd SIGABRT'd the kiosk
// (KillMode=mixed took RetroArch with it).
class GameSessionBracket {
public:
    struct Ops {
        SystemdNotify systemd_notify;
        // Media Browser builds: quiet the media stack for the whole session
        // (async — never delays launch) and drop poster textures while the
        // GL context is still current / the reverse on the way out. Empty
        // in a MEDIA_BROWSER=OFF build.
        std::function<void()> quiet_media_stack;
        std::function<void()> restore_media_stack;
        // One GPIO poll (restart button) — run every 100 ms on a helper
        // thread while the main thread is inside the session.
        std::function<void()> poll_gpio;
        // status_writer.write_now(state).
        std::function<void()> write_status_now;
    };

    GameSessionBracket(AppState& state, PostGameGate& post_game_gate, Ops ops);
    // Joins the GPIO thread if a session never ended (only reachable when
    // main() unwinds mid-session).
    ~GameSessionBracket();

    GameSessionBracket(const GameSessionBracket&) = delete;
    GameSessionBracket& operator=(const GameSessionBracket&) = delete;

    // Controller's game-session BEGIN hook.
    void begin(const PlaylistItem& item);
    // Controller's game-session END hook — every exit path, including
    // launch failures.
    void end();

    bool session_running() const { return game_session_running_.load(); }

private:
    AppState& state_;
    PostGameGate& post_game_gate_;
    Ops ops_;
    std::atomic<bool> game_session_running_{false};
    std::thread game_session_gpio_thread_;
};

// ── Track-1 quiet mode: the media stack during a game ────────────────────
// Silence the torrent/media stack for the whole game session, mirroring
// PlaybackScreen's movie behavior. Gated on the provisioning marker
// (services/.env) so unprovisioned Pis do exactly nothing (no docker
// errors, no qBit timeouts in the log).
struct GameQuietDeps {
    std::string services_env_path;     // the provisioning marker
    std::string torrent_pause_marker;  // data/qbit_paused_by_kiosk
    // Cross-ordering with the MOVIE executor: bounded wait for it to be idle
    // before acting (see main.cpp). May be empty.
    std::function<void()> wait_for_movie_quiet;
    // qBittorrent pause_all() / resume_all(). Empty = no qBit client.
    std::function<bool()> pause_torrents;
    std::function<bool()> resume_torrents;
    // std::system in the kiosk.
    std::function<int(const char*)> run_command;
};
GameQuietMode::Actions make_game_quiet_actions(GameQuietDeps deps);

// ── Torrents left paused by a previous session ───────────────────────────
// Torrents the kiosk paused (movie FullPause / game quiet mode) and never
// resumed — kiosk crashed, was stopped mid-movie, or an OTA restarted it.
// Resume them in the background: qBit is often still starting at kiosk
// start, so retry for ~2 minutes without ever touching the render thread.
// The marker is only written by the kiosk's own pause, so an operator's
// manual pause is never undone.
class TorrentResumeRecovery {
public:
    static constexpr int kAttempts = 12;
    static constexpr int kSecondsBetweenAttempts = 10;

    TorrentResumeRecovery() = default;
    ~TorrentResumeRecovery();

    TorrentResumeRecovery(const TorrentResumeRecovery&) = delete;
    TorrentResumeRecovery& operator=(const TorrentResumeRecovery&) = delete;

    // Starts the retry worker only when `marker` says the kiosk paused
    // torrents. `resume_all` empty = no qBit client (never succeeds).
    void start_if_needed(const std::string& marker, std::function<bool()> resume_all);

private:
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

}  // namespace app
