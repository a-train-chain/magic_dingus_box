// app/game_handoff.h: the non-drawing half of the game hand-off, moved out
// of main.cpp — the begin/end hooks every RetroArch session runs, the
// systemd watchdog messages, the game quiet-mode actions and the startup
// resume of torrents a previous session left paused. These pin the order
// of side effects (watchdog off BEFORE the blocking teardown, the status
// file saying "retroarch" before the main loop blocks, the post-game gate
// armed last) and the provisioning gate on the quiet actions.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/app_state.h"
#include "app/game_handoff.h"
#include "app/post_game_gate.h"
#include "app/torrent_pause_marker.h"

namespace fs = std::filesystem;

namespace {

struct Log {
    std::mutex m;
    std::vector<std::string> v;
    void add(const std::string& s) {
        std::lock_guard<std::mutex> l(m);
        v.push_back(s);
    }
    std::vector<std::string> get() {
        std::lock_guard<std::mutex> l(m);
        return v;
    }
};

fs::path temp_dir(const std::string& tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path d = fs::temp_directory_path() /
                 ("mdb_game_handoff_" + tag + "_" + std::to_string(stamp));
    fs::create_directories(d);
    return d;
}

template <class Pred>
bool wait_for(Pred p, std::chrono::milliseconds limit = std::chrono::milliseconds(3000)) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return p();
}

}  // namespace

// ── systemd watchdog ─────────────────────────────────────────────────────

TEST_CASE("session watchdog events map to sd_notify messages", "[game_handoff]") {
    std::vector<std::string> sent;
    app::SystemdNotify notify = [&](const char* m) { sent.emplace_back(m); };

    app::notify_session_watchdog(retroarch::SessionWatchdog::Arm, notify);
    CHECK(sent == std::vector<std::string>{"WATCHDOG_USEC=10000000", "WATCHDOG=1"});
    sent.clear();
    app::notify_session_watchdog(retroarch::SessionWatchdog::Ping, notify);
    CHECK(sent == std::vector<std::string>{"WATCHDOG=1"});
    sent.clear();
    app::notify_session_watchdog(retroarch::SessionWatchdog::Disarm, notify);
    CHECK(sent == std::vector<std::string>{"WATCHDOG_USEC=0"});

    // Without libsystemd the notifier is empty: nothing to call, no crash.
    app::notify_session_watchdog(retroarch::SessionWatchdog::Arm, app::SystemdNotify{});
}

// ── GameSessionBracket ───────────────────────────────────────────────────

TEST_CASE("the session bracket runs begin and end side effects in order",
          "[game_handoff]") {
    app::AppState state;
    state.post_game_fade_start_ms.store(1234);  // a fade still in flight
    state.loading_alpha.store(0.0f);
    app::PostGameGate gate;
    Log log;
    std::atomic<int> gpio_polls{0};
    app::ScreenMode mode_at_status_write = app::ScreenMode::Playlist;

    app::GameSessionBracket::Ops ops;
    ops.systemd_notify = [&](const char* m) { log.add(std::string("notify ") + m); };
    ops.quiet_media_stack = [&] { log.add("quiet"); };
    ops.restore_media_stack = [&] { log.add("restore"); };
    ops.poll_gpio = [&] { gpio_polls.fetch_add(1); };
    ops.write_status_now = [&] {
        mode_at_status_write = state.screen_mode.load();
        log.add("status");
    };
    app::GameSessionBracket bracket(state, gate, ops);

    app::PlaylistItem game;
    game.title = "Super Mario World";
    game.emulator_core = "snes9x2010_libretro";
    bracket.begin(game);

    CHECK(log.get() == std::vector<std::string>{
                           "quiet", "notify WATCHDOG_USEC=0", "status"});
    CHECK(state.is_loading_game);
    CHECK(state.loading_alpha.load() == 1.0f);
    CHECK(state.post_game_fade_start_ms.load() == 0);
    CHECK(state.retroarch_rom_name == "Super Mario World");
    CHECK(state.retroarch_core == "snes9x2010_libretro");
    CHECK(mode_at_status_write == app::ScreenMode::RetroArch);
    CHECK(bracket.session_running());
    // The restart button is polled while the main thread is inside the game.
    CHECK(wait_for([&] { return gpio_polls.load() > 0; }));
    CHECK_FALSE(gate.settling());

    state.loading_progress.store(0.7f);
    state.loading_phase = "Starting core";
    bracket.end();

    CHECK(log.get() == std::vector<std::string>{
                           "quiet", "notify WATCHDOG_USEC=0", "status",
                           "notify WATCHDOG_USEC=10000000", "restore"});
    CHECK_FALSE(bracket.session_running());
    CHECK_FALSE(state.is_loading_game);
    CHECK(state.loading_progress.load() == 0.0f);
    CHECK(state.loading_phase.empty());
    // Still "retroarch" until the main loop's ready edge.
    CHECK(gate.settling());
    CHECK_FALSE(gate.accepts_input());
    // The ROM fields are cleared at the ready edge, not here.
    CHECK(state.retroarch_rom_name == "Super Mario World");

    // The GPIO thread is joined: no polls after end().
    const int polls_after_end = gpio_polls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(gpio_polls.load() == polls_after_end);
}

TEST_CASE("the session bracket works with no optional ops (MEDIA_BROWSER=OFF, "
          "no libsystemd)", "[game_handoff]") {
    app::AppState state;
    app::PostGameGate gate;
    app::GameSessionBracket bracket(state, gate, {});
    app::PlaylistItem game;
    game.title = "Sonic";
    bracket.begin(game);
    CHECK(state.screen_mode.load() == app::ScreenMode::RetroArch);
    bracket.end();
    CHECK(gate.settling());
    // A second session reuses the bracket.
    bracket.begin(game);
    CHECK(bracket.session_running());
    bracket.end();
    CHECK_FALSE(bracket.session_running());
}

// ── Game quiet-mode actions ──────────────────────────────────────────────

TEST_CASE("game quiet actions pause/resume the stack in order on a provisioned box",
          "[game_handoff]") {
    const fs::path dir = temp_dir("quiet");
    const fs::path env = dir / ".env";
    std::ofstream(env) << "WIREGUARD_PRIVATE_KEY=x\n";
    const std::string marker = (dir / "qbit_paused_by_kiosk").string();

    Log log;
    bool torrents_ok = true;
    app::GameQuietDeps deps;
    deps.services_env_path = env.string();
    deps.torrent_pause_marker = marker;
    deps.wait_for_movie_quiet = [&] { log.add("wait"); };
    deps.pause_torrents = [&] { log.add("pause_all"); return torrents_ok; };
    deps.resume_torrents = [&] { log.add("resume_all"); return torrents_ok; };
    deps.run_command = [&](const char* cmd) { log.add(cmd); return 0; };
    auto actions = app::make_game_quiet_actions(deps);

    actions.pause();
    CHECK(log.get() == std::vector<std::string>{
                           "wait", "pause_all",
                           "/usr/local/bin/playback_services_pause.sh pause >/dev/null 2>&1"});
    CHECK(app::torrents_paused_by_kiosk(marker));

    actions.resume();
    const auto all = log.get();
    CHECK(std::vector<std::string>(all.begin() + 3, all.end()) ==
          std::vector<std::string>{
              "wait",
              "/usr/local/bin/playback_services_pause.sh unpause >/dev/null 2>&1",
              "resume_all"});
    CHECK_FALSE(app::torrents_paused_by_kiosk(marker));

    SECTION("a failed qBit pause writes no marker; the containers still pause") {
        torrents_ok = false;
        actions.pause();
        CHECK_FALSE(app::torrents_paused_by_kiosk(marker));
        CHECK(log.get().back() ==
              "/usr/local/bin/playback_services_pause.sh pause >/dev/null 2>&1");
    }
    SECTION("a failed qBit resume keeps the marker for the next start") {
        actions.pause();
        torrents_ok = false;
        actions.resume();
        CHECK(app::torrents_paused_by_kiosk(marker));
    }

    fs::remove_all(dir);
}

TEST_CASE("game quiet actions do nothing on an unprovisioned box", "[game_handoff]") {
    const fs::path dir = temp_dir("unprovisioned");
    Log log;
    app::GameQuietDeps deps;
    deps.services_env_path = (dir / "missing.env").string();
    deps.torrent_pause_marker = (dir / "marker").string();
    deps.wait_for_movie_quiet = [&] { log.add("wait"); };
    deps.pause_torrents = [&] { log.add("pause_all"); return true; };
    deps.resume_torrents = [&] { log.add("resume_all"); return true; };
    deps.run_command = [&](const char* cmd) { log.add(cmd); return 0; };
    auto actions = app::make_game_quiet_actions(deps);
    actions.pause();
    actions.resume();
    // Only the cross-ordering wait runs (it precedes the gate).
    CHECK(log.get() == std::vector<std::string>{"wait", "wait"});
    fs::remove_all(dir);
}

TEST_CASE("game quiet actions without a qBit client still drive the containers",
          "[game_handoff]") {
    const fs::path dir = temp_dir("noqbit");
    const fs::path env = dir / ".env";
    std::ofstream(env) << "\n";
    Log log;
    app::GameQuietDeps deps;
    deps.services_env_path = env.string();
    deps.torrent_pause_marker = (dir / "marker").string();
    deps.run_command = [&](const char* cmd) { log.add(cmd); return 0; };
    auto actions = app::make_game_quiet_actions(deps);
    actions.pause();
    actions.resume();
    CHECK(log.get().size() == 2);
    CHECK_FALSE(app::torrents_paused_by_kiosk((dir / "marker").string()));
    fs::remove_all(dir);
}

// ── TorrentResumeRecovery ────────────────────────────────────────────────

TEST_CASE("startup resumes torrents only when the kiosk left them paused",
          "[game_handoff]") {
    const fs::path dir = temp_dir("recovery");
    const std::string marker = (dir / "qbit_paused_by_kiosk").string();
    std::atomic<int> resumes{0};

    {
        app::TorrentResumeRecovery recovery;
        recovery.start_if_needed(marker, [&] { resumes.fetch_add(1); return true; });
    }
    CHECK(resumes.load() == 0);  // no marker: an operator's pause is not ours

    app::mark_torrents_paused_by_kiosk(marker, true);
    {
        app::TorrentResumeRecovery recovery;
        recovery.start_if_needed(marker, [&] { resumes.fetch_add(1); return true; });
        CHECK(wait_for([&] { return !app::torrents_paused_by_kiosk(marker); }));
    }
    CHECK(resumes.load() == 1);
    fs::remove_all(dir);
}

TEST_CASE("a pending torrent resume retry stops promptly at shutdown",
          "[game_handoff]") {
    const fs::path dir = temp_dir("recovery_stop");
    const std::string marker = (dir / "qbit_paused_by_kiosk").string();
    app::mark_torrents_paused_by_kiosk(marker, true);
    std::atomic<int> attempts{0};
    const auto t0 = std::chrono::steady_clock::now();
    {
        app::TorrentResumeRecovery recovery;
        // qBit still down: every attempt fails.
        recovery.start_if_needed(marker, [&] { attempts.fetch_add(1); return false; });
        CHECK(wait_for([&] { return attempts.load() > 0; }));
    }
    // Destructor interrupts the 10 s back-off within ~1 s.
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
    CHECK(app::torrents_paused_by_kiosk(marker));  // still recorded for next start
    fs::remove_all(dir);
}
