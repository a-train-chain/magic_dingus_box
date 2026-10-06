#include "game_handoff.h"

#include "game_launch_recovery.h"
#include "torrent_pause_marker.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <utility>

namespace app {

void notify_session_watchdog(retroarch::SessionWatchdog ev, const SystemdNotify& notify) {
    if (!notify) return;
    switch (ev) {
        case retroarch::SessionWatchdog::Arm:
            notify("WATCHDOG_USEC=10000000");  // = WatchdogSec=10
            notify("WATCHDOG=1");
            break;
        case retroarch::SessionWatchdog::Ping:
            notify("WATCHDOG=1");
            break;
        case retroarch::SessionWatchdog::Disarm:
            notify("WATCHDOG_USEC=0");
            break;
    }
}

// ── GameSessionBracket ───────────────────────────────────────────────────

GameSessionBracket::GameSessionBracket(AppState& state, PostGameGate& post_game_gate,
                                       Ops ops)
    : state_(state), post_game_gate_(post_game_gate), ops_(std::move(ops)) {}

GameSessionBracket::~GameSessionBracket() {
    game_session_running_.store(false);
    if (game_session_gpio_thread_.joinable()) {
        game_session_gpio_thread_.join();
    }
}

void GameSessionBracket::begin(const PlaylistItem& item) {
    // Raises is_loading_game, resets loading_alpha to opaque, and
    // cancels any in-flight post-game fade — a stale alpha of 0 from
    // the previous exit would make this launch's plate invisible.
    app::prepare_loading_state_for_launch(state_);
    // Media Browser: quiet the media stack for the whole session (async —
    // never delays launch) and drop poster textures while the GL context is
    // still current.
    if (ops_.quiet_media_stack) ops_.quiet_media_stack();
    // GPIO polling thread so the restart button works during
    // gameplay while the main thread is inside the game session
    // (teardown, supervision, restore). The button restarts the
    // service; the resulting SIGTERM stops the game gracefully.
    game_session_running_.store(true);
    game_session_gpio_thread_ = std::thread([this]() {
        while (game_session_running_.load()) {
            if (ops_.poll_gpio) ops_.poll_gpio();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    // Off for the launch teardown and the post-game DRM/input
    // restore (each several seconds of blocking work). The launcher
    // re-arms it for the supervised play phase (set_session_watchdog)
    // and disarms it again before the restore.
    if (ops_.systemd_notify) ops_.systemd_notify("WATCHDOG_USEC=0");
    // Phone-remote: the per-frame deriver never executes while the
    // main loop blocks, so set the mode explicitly and flush
    // status so the companion app sees "retroarch" immediately.
    state_.retroarch_rom_name = item.title;
    state_.retroarch_core     = item.emulator_core;
    state_.screen_mode.store(app::ScreenMode::RetroArch);
    if (ops_.write_status_now) ops_.write_status_now();
}

void GameSessionBracket::end() {
    // Re-enable watchdog after RetroArch exits (10s = 10000000 usec)
    if (ops_.systemd_notify) ops_.systemd_notify("WATCHDOG_USEC=10000000");
    game_session_running_.store(false);
    if (game_session_gpio_thread_.joinable()) {
        game_session_gpio_thread_.join();
    }
    // Reset loading state. loading_alpha is deliberately NOT reset
    // here — it is 0.0 (dissolved) and stays 0.0 until the next
    // launch's prepare_loading_state_for_launch, so nothing can
    // flash the plate between now and the menu fade-in.
    state_.is_loading_game = false;
    state_.loading_progress.store(0.0f);
    state_.loading_phase.clear();
    // Media Browser: artwork resume, then the quiet-mode resume.
    if (ops_.restore_media_stack) ops_.restore_media_stack();
    // Do NOT publish the menu from here. This hook runs while the
    // main loop is still inside the dispatch of the launching press:
    // the Settings fields in AppState are the pre-launch snapshot
    // (menu open, game list showing) that main.cpp is about to
    // force-close, and the post-game reset has not run. Publishing
    // "playlist" now let a client aim a SELECT at that stale game
    // list and land it on Master Shuffle (Pi 5, 2026-10-03). The
    // gate keeps "retroarch" published until the main loop's ready
    // edge, which clears the ROM/core fields and lets the live
    // screen through — see app/post_game_gate.h.
    post_game_gate_.session_ended();
}

// ── Game quiet mode actions ──────────────────────────────────────────────

GameQuietMode::Actions make_game_quiet_actions(GameQuietDeps deps) {
    auto shared = std::make_shared<const GameQuietDeps>(std::move(deps));
    return GameQuietMode::Actions{
        /*pause=*/[d = shared]() {
            if (d->wait_for_movie_quiet) d->wait_for_movie_quiet();
            if (!std::filesystem::exists(d->services_env_path)) {
                return;
            }
            if (d->pause_torrents) {
                if (d->pause_torrents()) {
                    app::mark_torrents_paused_by_kiosk(d->torrent_pause_marker, true);
                } else {
                    std::cout << "[quiet-mode] qbit pause_all failed "
                                 "(best-effort)" << std::endl;
                }
            }
            if (d->run_command) {
                (void)d->run_command(
                    "/usr/local/bin/playback_services_pause.sh pause "
                    ">/dev/null 2>&1");
            }
        },
        /*resume=*/[d = shared]() {
            if (d->wait_for_movie_quiet) d->wait_for_movie_quiet();
            if (!std::filesystem::exists(d->services_env_path)) {
                return;
            }
            if (d->run_command) {
                (void)d->run_command(
                    "/usr/local/bin/playback_services_pause.sh unpause "
                    ">/dev/null 2>&1");
            }
            if (d->resume_torrents) {
                if (d->resume_torrents()) {
                    app::mark_torrents_paused_by_kiosk(d->torrent_pause_marker, false);
                } else {
                    std::cout << "[quiet-mode] qbit resume_all failed; "
                                 "retried at next kiosk start" << std::endl;
                }
            }
        }};
}

// ── TorrentResumeRecovery ────────────────────────────────────────────────

TorrentResumeRecovery::~TorrentResumeRecovery() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
}

void TorrentResumeRecovery::start_if_needed(const std::string& marker,
                                            std::function<bool()> resume_all) {
    if (!app::torrents_paused_by_kiosk(marker)) return;
    worker_ = std::thread(
        [resume_all = std::move(resume_all), &stop = stop_, marker]() {
            for (int attempt = 0; attempt < kAttempts && !stop; ++attempt) {
                if (resume_all && resume_all()) {
                    app::mark_torrents_paused_by_kiosk(marker, false);
                    std::cout << "[quiet-mode] resumed torrents left "
                                 "paused by a previous session" << std::endl;
                    return;
                }
                for (int i = 0; i < kSecondsBetweenAttempts && !stop; ++i) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
        });
}

}  // namespace app
