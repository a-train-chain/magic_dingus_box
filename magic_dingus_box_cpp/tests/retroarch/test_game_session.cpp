// RetroArch game-session process control: the stop policy, the supervision
// loop, the child environment, and — with a stand-in "retroarch" — the real
// fork/exec + signal path, reproducing the SIGTERM-mid-save race off-Pi.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "retroarch/game_session.h"

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using retroarch::SessionOutcome;
using retroarch::StopResult;

// Scripted RetroArch for the pure policy. Time is the sum of sleeps.
struct FakeSession {
    bool alive = true;
    // Polls (after SIGTERM) until it exits on its own; -1 = ignores TERM.
    int exits_after_term = 0;
    bool termed = false;
    // Ready once this much time has been slept; -1 = never.
    std::chrono::milliseconds ready_at{0};
    bool never_ready = false;
    // Exits on its own once this much time has passed; -1 = never.
    std::chrono::milliseconds exits_at{-1};
    std::chrono::milliseconds stop_at{-1};

    std::chrono::milliseconds now{0};
    std::vector<std::pair<char, int>> signals;  // 'L'eader / 'G'roup
    int keepalives = 0;
    int ready_calls = 0;

    retroarch::SessionOps ops() {
        retroarch::SessionOps o;
        o.exited = [this] {
            if (alive && exits_at >= 0ms && now >= exits_at) alive = false;
            if (alive && termed && exits_after_term >= 0) {
                if (exits_after_term == 0) alive = false;
                else --exits_after_term;
            }
            return !alive;
        };
        o.ready = [this] { return !never_ready && now >= ready_at; };
        o.stop_requested = [this] { return stop_at >= 0ms && now >= stop_at; };
        o.signal_leader = [this](int sig) {
            signals.push_back({'L', sig});
            if (sig == SIGTERM) termed = true;
            if (sig == SIGKILL) alive = false;
        };
        o.signal_group = [this](int sig) {
            signals.push_back({'G', sig});
            if (sig == SIGKILL) alive = false;
        };
        o.keepalive = [this] { ++keepalives; };
        o.on_ready = [this] { ++ready_calls; };
        o.sleep = [this](std::chrono::milliseconds d) { now += d; };
        return o;
    }
};

const std::pair<char, int> kTermLeader{'L', SIGTERM};
const std::pair<char, int> kKillGroup{'G', SIGKILL};

std::string temp_path(const std::string& leaf) {
    return (fs::temp_directory_path() /
            ("mdb-gs-" + std::to_string(getpid()) + "-" + leaf))
        .string();
}

std::string slurp(const std::string& p) {
    std::ifstream f(p);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

}  // namespace

// ── stop_game_session ────────────────────────────────────────────────────

TEST_CASE("stop: a RetroArch that already quit gets only the group sweep",
          "[retroarch][session][stop]") {
    FakeSession s;
    s.alive = false;
    CHECK(retroarch::stop_game_session(s.ops(), 5000ms, 50ms) ==
          StopResult::AlreadyExited);
    CHECK(s.signals == std::vector<std::pair<char, int>>{kKillGroup});
    CHECK(s.now == 0ms);
}

TEST_CASE("stop: SIGTERM goes to RetroArch itself and it gets time to save",
          "[retroarch][session][stop]") {
    FakeSession s;
    s.exits_after_term = 20;  // ~1 s of auto-save at 50 ms polls
    CHECK(retroarch::stop_game_session(s.ops(), 5000ms, 50ms) ==
          StopResult::Terminated);
    // TERM to the leader (never the group: RetroArch's handler is the one
    // that saves), then only the post-exit sweep.
    CHECK(s.signals ==
          std::vector<std::pair<char, int>>{kTermLeader, kKillGroup});
    CHECK(s.now < 5000ms);
    CHECK(s.keepalives > 0);  // the watchdog keeps being fed while we wait
}

TEST_CASE("stop: a wedged RetroArch is group-SIGKILLed after the grace",
          "[retroarch][session][stop]") {
    FakeSession s;
    s.exits_after_term = -1;
    CHECK(retroarch::stop_game_session(s.ops(), 5000ms, 50ms) ==
          StopResult::Killed);
    CHECK(s.signals == std::vector<std::pair<char, int>>{
                           kTermLeader, kKillGroup, kKillGroup});
    // Bounded: the kiosk's whole stop must fit TimeoutStopSec=20.
    CHECK(s.now >= 5000ms);
    CHECK(s.now <= 5000ms + retroarch::kKillWait);
}

// ── supervise_session ────────────────────────────────────────────────────

TEST_CASE("supervise: normal play then quit is Exited, no TERM sent",
          "[retroarch][session][supervise]") {
    FakeSession s;
    s.ready_at = 800ms;
    s.exits_at = 60000ms;  // a minute of play
    const auto r = retroarch::supervise_session(s.ops(), {});
    CHECK(r.outcome == SessionOutcome::Exited);
    CHECK(r.ready);
    CHECK(r.stop == StopResult::AlreadyExited);
    CHECK(s.ready_calls == 1);
    CHECK(std::find(s.signals.begin(), s.signals.end(), kTermLeader) ==
          s.signals.end());
    // Pinged every tick for the whole game — the reason the watchdog no
    // longer has to be disabled during a session.
    CHECK(s.keepalives >= 60000 / 50);
}

TEST_CASE("supervise: a long game never trips the startup timeout",
          "[retroarch][session][supervise]") {
    FakeSession s;
    s.ready_at = 14900ms;   // just inside the 15 s window
    s.exits_at = 120000ms;
    const auto r = retroarch::supervise_session(s.ops(), {});
    CHECK(r.outcome == SessionOutcome::Exited);
}

TEST_CASE("supervise: dying before KMS is ExitedBeforeReady",
          "[retroarch][session][supervise]") {
    FakeSession s;
    s.never_ready = true;
    s.exits_at = 300ms;
    const auto r = retroarch::supervise_session(s.ops(), {});
    CHECK(r.outcome == SessionOutcome::ExitedBeforeReady);
    CHECK_FALSE(r.ready);
    CHECK(s.ready_calls == 0);
}

TEST_CASE("supervise: no KMS within the timeout stops it with the short grace",
          "[retroarch][session][supervise]") {
    FakeSession s;
    s.never_ready = true;
    s.exits_after_term = -1;
    retroarch::SupervisePolicy p;
    const auto r = retroarch::supervise_session(s.ops(), p);
    CHECK(r.outcome == SessionOutcome::StartupTimedOut);
    CHECK(r.stop == StopResult::Killed);
    CHECK(s.now >= p.startup_timeout + p.startup_stop_grace);
    CHECK(s.now <= p.startup_timeout + p.startup_stop_grace +
                       retroarch::kKillWait + p.poll);
}

TEST_CASE("supervise: a shutdown request mid-game stops with the full grace",
          "[retroarch][session][supervise]") {
    // systemctl stop / GPIO restart / reboot while playing: the handler only
    // sets the flag; the loop sees it within one poll and runs the stop.
    FakeSession s;
    s.ready_at = 500ms;
    s.stop_at = 30000ms;
    s.exits_after_term = 40;  // a 2 s save-state (N64-sized)
    const auto r = retroarch::supervise_session(s.ops(), {});
    CHECK(r.outcome == SessionOutcome::StopRequested);
    CHECK(r.stop == StopResult::Terminated);  // saved, not killed
    CHECK(s.signals.front() == kTermLeader);
    CHECK(s.now - 30000ms <= 2100ms);
}

TEST_CASE("supervise: a request that arrived before the game started stops it "
          "without waiting out the startup timeout",
          "[retroarch][session][supervise]") {
    FakeSession s;
    s.never_ready = true;
    s.stop_at = 0ms;
    s.exits_after_term = 2;
    const auto r = retroarch::supervise_session(s.ops(), {});
    CHECK(r.outcome == SessionOutcome::StopRequested);
    CHECK(s.now < 1000ms);
}

// ── stop flag ────────────────────────────────────────────────────────────

TEST_CASE("the stop request is sticky until reset", "[retroarch][session]") {
    retroarch::reset_session_stop_for_testing();
    CHECK_FALSE(retroarch::session_stop_requested());
    retroarch::request_session_stop();
    CHECK(retroarch::session_stop_requested());
    CHECK(retroarch::session_stop_requested());
    retroarch::reset_session_stop_for_testing();
    CHECK_FALSE(retroarch::session_stop_requested());
}

// ── environment / readiness helpers ─────────────────────────────────────

TEST_CASE("child environment: compositor hints, systemd and secrets removed",
          "[retroarch][session][env]") {
    const std::vector<std::string> parent = {
        "PATH=/usr/bin:/bin",
        "LANG=C.UTF-8",
        "DISPLAY=",
        "WAYLAND_DISPLAY=wayland-0",
        "XDG_SESSION_TYPE=tty",
        "SDL_VIDEODRIVER=x11",
        "NOTIFY_SOCKET=/run/systemd/notify",
        "WATCHDOG_USEC=10000000",
        "WATCHDOG_PID=123",
        "WIREGUARD_PRIVATE_KEY=abc",
        "RADARR_API_KEY=def",
        "MDB_QBIT_PASS=ghi",
        "QBITTORRENT_ADMIN_PASSWORD=jkl",
        "HOME=/wrong",
        "XDG_RUNTIME_DIR=/wrong",
        "MAGIC_DATA_DIR=/opt/x/data",
        "malformed",
    };
    const auto env = retroarch::build_child_environment(parent, "/home/magic", 1000);
    auto has = [&](const std::string& e) {
        return std::find(env.begin(), env.end(), e) != env.end();
    };
    auto has_name = [&](const std::string& n) {
        return std::any_of(env.begin(), env.end(), [&](const std::string& e) {
            return e.compare(0, n.size() + 1, n + "=") == 0;
        });
    };
    CHECK(has("PATH=/usr/bin:/bin"));
    CHECK(has("LANG=C.UTF-8"));
    CHECK(has("MAGIC_DATA_DIR=/opt/x/data"));
    CHECK(has("HOME=/home/magic"));
    CHECK(has("XDG_RUNTIME_DIR=/run/user/1000"));
    for (const char* gone : {"DISPLAY", "WAYLAND_DISPLAY", "XDG_SESSION_TYPE",
                             "SDL_VIDEODRIVER", "NOTIFY_SOCKET", "WATCHDOG_USEC",
                             "WATCHDOG_PID", "WIREGUARD_PRIVATE_KEY",
                             "RADARR_API_KEY", "MDB_QBIT_PASS",
                             "QBITTORRENT_ADMIN_PASSWORD"}) {
        INFO(gone);
        CHECK_FALSE(has_name(gone));
    }
    CHECK(std::count_if(env.begin(), env.end(), [](const std::string& e) {
              return e.rfind("HOME=", 0) == 0;
          }) == 1);
    CHECK_FALSE(has("malformed"));
}

TEST_CASE("secret-name filter does not eat ordinary variables",
          "[retroarch][session][env]") {
    for (const char* keep : {"PATH", "LANG", "HOME", "KEYBOARD_LAYOUT",
                             "MESA_LOADER_DRIVER_OVERRIDE", "PULSE_SERVER",
                             "DBUS_SESSION_BUS_ADDRESS", "XKB_DEFAULT_LAYOUT"}) {
        INFO(keep);
        CHECK_FALSE(retroarch::is_secret_env_name(keep));
    }
    for (const char* drop : {"SONARR_API_KEY", "TMDB_API_KEY", "MDB_QBIT_PASS",
                             "SOME_SECRET", "GITHUB_TOKEN", "A_KEY_FILE"}) {
        INFO(drop);
        CHECK(retroarch::is_secret_env_name(drop));
    }
}

TEST_CASE("readiness is a DRM card node, not the render node",
          "[retroarch][session][ready]") {
    CHECK(retroarch::is_drm_card_path("/dev/dri/card0"));
    CHECK(retroarch::is_drm_card_path("/dev/dri/card1"));
    CHECK_FALSE(retroarch::is_drm_card_path("/dev/dri/renderD128"));
    CHECK_FALSE(retroarch::is_drm_card_path("/dev/dri/card"));
    CHECK_FALSE(retroarch::is_drm_card_path("/dev/input/event3"));
    CHECK_FALSE(retroarch::is_drm_card_path("socket:[1234]"));
}

// ── real processes: a stand-in "retroarch" ───────────────────────────────

namespace {

// Spawns `script` under bash via the real spawn_session, with real ops
// except that readiness is "the stand-in wrote <ready>" (macOS has no /proc).
struct StandIn {
    pid_t pid = -1;
    std::string ready;
    std::string log;

    explicit StandIn(const std::string& leaf, const std::string& script) {
        ready = temp_path(leaf + "-ready");
        log = temp_path(leaf + ".log");
        fs::remove(ready);
        fs::remove(log);
        retroarch::SpawnSpec spec;
        spec.executable = "/bin/bash";
        spec.argv = {"retroarch", "-c", script};
        spec.envp = retroarch::build_child_environment(
            {"PATH=/usr/bin:/bin", "MDB_QBIT_PASS=secret"}, "/tmp/mdb-home", 4242);
        spec.envp.push_back("READY=" + ready);
        spec.log_path = log;
        std::string err;
        pid = retroarch::spawn_session(spec, &err);
        INFO(err);
        REQUIRE(pid > 0);
    }

    retroarch::SessionOps ops() {
        auto o = retroarch::real_session_ops(pid, "", nullptr);
        const std::string r = ready;
        o.ready = [r] { return fs::exists(r); };
        return o;
    }

    ~StandIn() {
        fs::remove(ready);
        fs::remove(log);
    }
};

void wait_for_file(const std::string& p) {
    for (int i = 0; i < 200 && !fs::exists(p); ++i) std::this_thread::sleep_for(10ms);
}

}  // namespace

TEST_CASE("real process: SIGTERM mid-game waits for RetroArch's save to land",
          "[retroarch][session][process]") {
    // The race 7266ac3 fixed in bash, now owned by C++: on a kiosk stop the
    // stand-in takes 0.4 s to write its save after SIGTERM. The session must
    // not be considered over (and the display reclaimed) before it lands.
    const std::string saved = temp_path("saved");
    fs::remove(saved);
    StandIn ra("term", "trap 'sleep 0.4; echo saved > " + saved +
                           "; exit 0' TERM; : > \"$READY\"; "
                           "while :; do sleep 0.05; done");
    wait_for_file(ra.ready);

    retroarch::reset_session_stop_for_testing();
    auto ops = ra.ops();
    retroarch::SupervisePolicy policy;
    policy.startup_timeout = 5000ms;
    policy.poll = 20ms;
    std::thread stopper([] {
        std::this_thread::sleep_for(200ms);
        retroarch::request_session_stop();  // what the SIGTERM handler does
    });
    const auto report = retroarch::supervise_session(ops, policy);
    stopper.join();
    retroarch::reset_session_stop_for_testing();

    CHECK(report.outcome == SessionOutcome::StopRequested);
    CHECK(report.stop == StopResult::Terminated);
    CHECK(fs::exists(saved));  // landed BEFORE supervise returned
    const int status = retroarch::reap_session(ra.pid);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    fs::remove(saved);
}

TEST_CASE("real process: a RetroArch ignoring SIGTERM is killed within bounds",
          "[retroarch][session][process]") {
    StandIn ra("wedged", "trap '' TERM; : > \"$READY\"; while :; do sleep 0.05; done");
    wait_for_file(ra.ready);
    auto ops = ra.ops();
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(retroarch::stop_game_session(ops, 300ms, 20ms) == StopResult::Killed);
    CHECK(std::chrono::steady_clock::now() - t0 < 300ms + retroarch::kKillWait);
    const int status = retroarch::reap_session(ra.pid);
    REQUIRE(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGKILL);
}

TEST_CASE("real process: anything RetroArch left in its group is swept",
          "[retroarch][session][process]") {
    const std::string child_pid_file = temp_path("grandchild");
    fs::remove(child_pid_file);
    StandIn ra("sweep", "sleep 30 & echo $! > " + child_pid_file +
                            "; : > \"$READY\"; sleep 0.2; exit 4");
    wait_for_file(child_pid_file);
    wait_for_file(ra.ready);
    pid_t grandchild = 0;
    for (int i = 0; i < 100 && grandchild == 0; ++i) {
        std::ifstream(child_pid_file) >> grandchild;
        if (grandchild == 0) std::this_thread::sleep_for(10ms);
    }
    REQUIRE(grandchild > 0);

    retroarch::reset_session_stop_for_testing();
    retroarch::SupervisePolicy policy;
    policy.poll = 20ms;
    const auto report = retroarch::supervise_session(ra.ops(), policy);
    CHECK(report.outcome == SessionOutcome::Exited);
    const int status = retroarch::reap_session(ra.pid);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 4);  // RetroArch's own code survives
    bool gone = false;
    for (int i = 0; i < 100 && !gone; ++i) {
        gone = ::kill(grandchild, 0) != 0 && errno == ESRCH;
        if (!gone) std::this_thread::sleep_for(10ms);
    }
    CHECK(gone);
    fs::remove(child_pid_file);
}

TEST_CASE("real process: env, log redirection and fd hygiene",
          "[retroarch][session][process]") {
    // An fd the kiosk holds open WITHOUT close-on-exec (think: a DRM or
    // evdev fd) must not reach RetroArch.
    const std::string leak_path = temp_path("leak");
    const int leaked = ::open(leak_path.c_str(), O_WRONLY | O_CREAT, 0644);
    REQUIRE(leaked >= 3);
    StandIn ra("env",
               "echo \"home=$HOME xdg=$XDG_RUNTIME_DIR pass=${MDB_QBIT_PASS:-none}\"; "
               "if [ -e /dev/fd/" + std::to_string(leaked) +
                   " ]; then echo leaked; else echo clean; fi; exit 0");
    ::close(leaked);
    retroarch::SupervisePolicy policy;
    policy.poll = 20ms;
    retroarch::supervise_session(ra.ops(), policy);
    retroarch::reap_session(ra.pid);
    const std::string out = slurp(ra.log);
    CHECK(out.find("home=/tmp/mdb-home xdg=/run/user/4242 pass=none") !=
          std::string::npos);
    CHECK(out.find("clean") != std::string::npos);
    CHECK(out.find("leaked") == std::string::npos);
    fs::remove(leak_path);
}

TEST_CASE("real process: exec failure is reported as exit 127, not a hang",
          "[retroarch][session][process]") {
    retroarch::SpawnSpec spec;
    spec.executable = "/nonexistent/retroarch";
    spec.argv = {"retroarch"};
    spec.envp = {"PATH=/usr/bin"};
    spec.log_path = temp_path("execfail.log");
    std::string err;
    const pid_t pid = retroarch::spawn_session(spec, &err);
    REQUIRE(pid > 0);
    retroarch::SupervisePolicy policy;
    policy.poll = 10ms;
    auto ops = retroarch::real_session_ops(pid, "", nullptr);
    const auto report = retroarch::supervise_session(ops, policy);
    CHECK(report.outcome == SessionOutcome::ExitedBeforeReady);
    const int status = retroarch::reap_session(pid);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 127);
    CHECK(slurp(spec.log_path).find("execve") != std::string::npos);
    fs::remove(spec.log_path);
}
