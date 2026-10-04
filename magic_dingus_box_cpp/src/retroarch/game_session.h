#pragma once

// RetroArch game-session process control: spawn RetroArch directly (no bash
// wrapper), supervise it without blocking the kiosk's main thread, and stop
// it through ONE policy no matter who asks — the SIGTERM handler (systemctl
// stop / OTA / reboot / poweroff / the GPIO restart button, which restarts
// the service), the startup timeout, or RetroArch quitting on its own.
//
// Everything that decides WHEN to signal what is pure and takes its I/O
// through SessionOps, so tests/retroarch drives it with a fake clock. The
// real ops (waitid/kill/proc scan) live in real_session_ops().

#include <chrono>
#include <functional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace retroarch {

// ── Stop request (async-signal-safe) ────────────────────────────────────
// Set from the kiosk's SIGTERM/SIGINT handler: it only flips a lock-free
// flag, never signals anything itself. The supervisor polls the flag and
// runs stop_game_session(). Sticky for the life of the process — a signal
// means the kiosk is shutting down, so a request that lands before the fork
// (during GStreamer/input teardown) must still abort the launch.
void request_session_stop() noexcept;
bool session_stop_requested() noexcept;
// Tests only: the flag is otherwise never cleared.
void reset_session_stop_for_testing() noexcept;

// ── Child environment ───────────────────────────────────────────────────
// The environment RetroArch is exec'd with, derived from the kiosk's own:
//   - DISPLAY, WAYLAND_DISPLAY, XDG_SESSION_TYPE, SDL_VIDEODRIVER removed
//     (a compositor hint makes RetroArch's context drivers look for a
//     session that does not exist; the old launcher `unset` the same four);
//   - NOTIFY_SOCKET / WATCHDOG_USEC / WATCHDOG_PID removed — those belong
//     to the kiosk's systemd contract, not to a child;
//   - secrets the unit loads from services/.env (anything named *PASS*,
//     *SECRET*, *TOKEN*, *_KEY / *_KEY_*) removed — an emulator has no use
//     for the VPN private key or the *arr API keys;
//   - HOME and XDG_RUNTIME_DIR forced to `home` and /run/user/<uid>
//     (the old launcher exported exactly these two).
std::vector<std::string> build_child_environment(
    const std::vector<std::string>& parent_env, const std::string& home,
    uid_t uid);

// True for environment variable NAMES build_child_environment() drops as
// secrets. Exposed for the unit test.
bool is_secret_env_name(const std::string& name);

// ── Spawn ───────────────────────────────────────────────────────────────
struct SpawnSpec {
    std::string executable;          // absolute path, exec'd directly
    std::vector<std::string> argv;   // argv[0] included
    std::vector<std::string> envp;   // complete KEY=VALUE list
    std::string log_path;            // stdout+stderr appended here ("" = inherit)
};

// fork + execve. The child: becomes its own process-group leader (so the
// pid IS the pgid and a group SIGKILL reaches anything RetroArch spawns),
// gets every signal disposition reset to SIG_DFL and an empty signal mask
// (exec keeps SIG_IGN and the mask — the kiosk ignores SIGPIPE), has fds >= 3
// closed (no DRM/evdev/GStreamer fd leaks into the emulator), and appends
// stdout/stderr to log_path. Only async-signal-safe calls run between fork
// and exec — the kiosk is multithreaded (GPIO poll thread, Media Browser
// workers), so malloc/setenv there could deadlock. Returns the pid, or -1
// with `error` set.
pid_t spawn_session(const SpawnSpec& spec, std::string* error);

// "/dev/dri/card0" etc. — the readiness signal: RetroArch opened the KMS
// node, i.e. it is taking the display over. renderD* (render-only) does
// not count, matching the old launcher's /dev/dri/card* glob.
bool is_drm_card_path(const std::string& fd_target);

// Scan /proc/<pid>/fd for a DRM card node. false on any error (including
// "no /proc", i.e. macOS).
bool process_has_drm_card_open(pid_t pid);

// ── Supervision policy ──────────────────────────────────────────────────
struct SessionOps {
    // Has the child terminated? Must NOT reap it (waitid WNOWAIT): while
    // the leader is an unreaped zombie its pid — hence the pgid — cannot be
    // reused, which is what makes the final group sweep safe.
    std::function<bool()> exited;
    std::function<bool()> ready;            // RetroArch has the KMS node open
    std::function<bool()> stop_requested;   // session_stop_requested()
    std::function<void(int sig)> signal_leader;
    std::function<void(int sig)> signal_group;
    std::function<void()> keepalive;        // systemd WATCHDOG=1 ping
    std::function<void()> on_ready;         // once, when ready first holds
    std::function<void(std::chrono::milliseconds)> sleep;
};

enum class StopResult {
    AlreadyExited,  // RetroArch had quit on its own; only the sweep ran
    Terminated,     // quit within the grace period after SIGTERM (saved)
    Killed,         // ignored SIGTERM for the whole grace; group SIGKILLed
};

// THE stop policy. If RetroArch is still running: SIGTERM the RetroArch
// process itself (its handler runs the auto save-state/SRAM flush), poll up
// to `grace` for it to exit, then SIGKILL the whole process group and wait
// (bounded by kKillWait) for the exit. Always finishes with a group SIGKILL
// sweep — harmless when the group is empty, and it takes out anything
// RetroArch left behind. Keeps calling keepalive while it waits. Does not
// reap; the caller does.
inline constexpr std::chrono::milliseconds kKillWait{2000};
StopResult stop_game_session(const SessionOps& ops,
                             std::chrono::milliseconds grace,
                             std::chrono::milliseconds poll);

enum class SessionOutcome {
    Exited,             // quit on its own after taking over KMS (normal)
    ExitedBeforeReady,  // died before ever opening the KMS node
    StartupTimedOut,    // never opened the KMS node within the timeout
    StopRequested,      // shutdown/restart requested; stopped gracefully
};

struct SessionReport {
    SessionOutcome outcome = SessionOutcome::Exited;
    StopResult stop = StopResult::AlreadyExited;
    bool ready = false;
};

struct SupervisePolicy {
    std::chrono::milliseconds startup_timeout{15000};
    std::chrono::milliseconds poll{50};
    // Mid-game stop: long enough for an N64/Dreamcast save-state to land,
    // short enough that the whole kiosk stop fits TimeoutStopSec=20.
    std::chrono::milliseconds stop_grace{5000};
    // A RetroArch that never reached KMS has nothing worth saving.
    std::chrono::milliseconds startup_stop_grace{1000};
};

// Poll until RetroArch exits, a stop is requested, or startup times out,
// pinging keepalive every tick. Time is counted from the sleeps, not a
// clock, so tests run instantly and the bounds hold exactly. Always ends
// with stop_game_session() (which also sweeps the group), so on return the
// child has terminated and is waiting to be reaped — unless it survived
// SIGKILL for kKillWait (uninterruptible sleep), which the caller's
// blocking reap then absorbs.
SessionReport supervise_session(const SessionOps& ops,
                                const SupervisePolicy& policy);

// Real ops for a spawned child (pid == pgid). `ready_marker` (may be "")
// is written with the pid when the KMS node first opens — kept for
// scripts/emulator_smoke_test.py, which reads it.
SessionOps real_session_ops(pid_t pid, const std::string& ready_marker,
                            std::function<void()> keepalive);

// Blocking reap (EINTR-safe). Returns the wait status, or -1.
int reap_session(pid_t pid);

}  // namespace retroarch
