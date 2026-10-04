#include "game_session.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace retroarch {

namespace {

// Lock-free on every target we build for; checked so a port to something
// exotic fails to compile instead of silently becoming signal-unsafe.
std::atomic<int> g_stop_requested{0};
static_assert(std::atomic<int>::is_always_lock_free,
              "the stop flag is written from a signal handler");

bool name_in(const std::string& name, std::initializer_list<const char*> set) {
    for (const char* s : set) {
        if (name == s) return true;
    }
    return false;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Async-signal-safe write of a C string to fd 2 (child side of the fork).
void child_say(const char* msg) {
    const size_t len = std::strlen(msg);
    ssize_t ignored = ::write(STDERR_FILENO, msg, len);
    (void)ignored;
}

void close_fds_from_3() {
#if defined(__linux__) && defined(SYS_close_range)
    if (::syscall(SYS_close_range, 3U, ~0U, 0U) == 0) return;
#endif
    long max_fd = ::sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536) max_fd = 65536;
    for (int fd = 3; fd < max_fd; ++fd) ::close(fd);
}

}  // namespace

void request_session_stop() noexcept {
    g_stop_requested.store(1, std::memory_order_relaxed);
}

bool session_stop_requested() noexcept {
    return g_stop_requested.load(std::memory_order_relaxed) != 0;
}

void reset_session_stop_for_testing() noexcept {
    g_stop_requested.store(0, std::memory_order_relaxed);
}

bool is_secret_env_name(const std::string& name) {
    return name.find("PASS") != std::string::npos ||
           name.find("SECRET") != std::string::npos ||
           name.find("TOKEN") != std::string::npos ||
           name.find("_KEY_") != std::string::npos ||
           ends_with(name, "_KEY");
}

std::vector<std::string> build_child_environment(
    const std::vector<std::string>& parent_env, const std::string& home,
    uid_t uid) {
    std::vector<std::string> env;
    env.reserve(parent_env.size() + 2);
    for (const auto& entry : parent_env) {
        const auto eq = entry.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string name = entry.substr(0, eq);
        if (name_in(name, {"DISPLAY", "WAYLAND_DISPLAY", "XDG_SESSION_TYPE",
                           "SDL_VIDEODRIVER", "NOTIFY_SOCKET", "WATCHDOG_USEC",
                           "WATCHDOG_PID", "HOME", "XDG_RUNTIME_DIR"})) {
            continue;
        }
        if (is_secret_env_name(name)) continue;
        env.push_back(entry);
    }
    env.push_back("HOME=" + home);
    env.push_back("XDG_RUNTIME_DIR=/run/user/" + std::to_string(uid));
    return env;
}

pid_t spawn_session(const SpawnSpec& spec, std::string* error) {
    auto fail = [&](const std::string& what) -> pid_t {
        if (error) *error = what + ": " + std::strerror(errno);
        return -1;
    };
    if (spec.argv.empty() || spec.executable.empty()) {
        errno = EINVAL;
        return fail("empty command");
    }

    // Everything the child needs is built HERE, before fork: the child may
    // only make async-signal-safe calls.
    std::vector<char*> argv;
    argv.reserve(spec.argv.size() + 1);
    for (const auto& a : spec.argv) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    std::vector<char*> envp;
    envp.reserve(spec.envp.size() + 1);
    for (const auto& e : spec.envp) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);

    int log_fd = -1;
    if (!spec.log_path.empty()) {
        log_fd = ::open(spec.log_path.c_str(),
                        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        // Not fatal: RetroArch then logs to the kiosk's journal instead.
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        if (log_fd >= 0) ::close(log_fd);
        return fail("fork");
    }
    if (pid == 0) {
        // ---- child: async-signal-safe calls only ----
        if (::setpgid(0, 0) != 0) _exit(126);
        if (log_fd >= 0) {
            ::dup2(log_fd, STDOUT_FILENO);  // dup2 clears FD_CLOEXEC
            ::dup2(log_fd, STDERR_FILENO);
        }
        struct sigaction dfl;
        std::memset(&dfl, 0, sizeof(dfl));
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        for (int sig = 1; sig < NSIG; ++sig) {
            if (sig == SIGKILL || sig == SIGSTOP) continue;
            ::sigaction(sig, &dfl, nullptr);  // EINVAL for odd numbers: fine
        }
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        close_fds_from_3();
        ::execve(spec.executable.c_str(), argv.data(), envp.data());
        child_say("game_session: execve of RetroArch failed\n");
        _exit(127);
    }

    // Parent. Repeat setpgid to close the fork/exec race; EACCES means the
    // child already exec'd (after grouping itself), ESRCH that it is gone.
    if (::setpgid(pid, pid) != 0 && errno != EACCES && errno != ESRCH) {
        const int saved = errno;
        ::kill(pid, SIGKILL);
        int st = 0;
        while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        if (log_fd >= 0) ::close(log_fd);
        errno = saved;
        return fail("setpgid");
    }
    if (log_fd >= 0) ::close(log_fd);
    return pid;
}

bool is_drm_card_path(const std::string& fd_target) {
    static const std::string prefix = "/dev/dri/card";
    return fd_target.size() > prefix.size() &&
           fd_target.compare(0, prefix.size(), prefix) == 0;
}

bool process_has_drm_card_open(pid_t pid) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::path("/proc") / std::to_string(pid) / "fd";
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code link_ec;
        const fs::path target = fs::read_symlink(it->path(), link_ec);
        if (!link_ec && is_drm_card_path(target.string())) return true;
    }
    return false;
}

StopResult stop_game_session(const SessionOps& ops,
                             std::chrono::milliseconds grace,
                             std::chrono::milliseconds poll) {
    if (poll <= std::chrono::milliseconds::zero()) poll = std::chrono::milliseconds(1);
    StopResult result = StopResult::AlreadyExited;
    if (!ops.exited()) {
        // TERM the real RetroArch process, not the group: its handler is
        // what writes the auto save-state and SRAM.
        ops.signal_leader(SIGTERM);
        result = StopResult::Killed;
        for (std::chrono::milliseconds waited{0}; waited < grace; waited += poll) {
            ops.sleep(poll);
            if (ops.keepalive) ops.keepalive();
            if (ops.exited()) {
                result = StopResult::Terminated;
                break;
            }
        }
        if (result == StopResult::Killed) {
            ops.signal_group(SIGKILL);
            for (std::chrono::milliseconds waited{0};
                 waited < kKillWait && !ops.exited(); waited += poll) {
                ops.sleep(poll);
                if (ops.keepalive) ops.keepalive();
            }
        }
    }
    // Sweep: the leader is a terminated-but-unreaped zombie here, so the
    // pgid cannot have been recycled; anything RetroArch spawned goes now.
    ops.signal_group(SIGKILL);
    return result;
}

SessionReport supervise_session(const SessionOps& ops,
                                const SupervisePolicy& policy) {
    const auto poll = policy.poll > std::chrono::milliseconds::zero()
                          ? policy.poll
                          : std::chrono::milliseconds(1);
    SessionReport report;
    std::chrono::milliseconds startup_waited{0};
    for (;;) {
        if (ops.keepalive) ops.keepalive();
        if (ops.exited()) {
            report.outcome = report.ready ? SessionOutcome::Exited
                                          : SessionOutcome::ExitedBeforeReady;
            report.stop = stop_game_session(ops, policy.stop_grace, poll);
            return report;
        }
        if (ops.stop_requested()) {
            report.outcome = SessionOutcome::StopRequested;
            report.stop = stop_game_session(
                ops, report.ready ? policy.stop_grace : policy.startup_stop_grace,
                poll);
            return report;
        }
        if (!report.ready) {
            if (ops.ready()) {
                report.ready = true;
                if (ops.on_ready) ops.on_ready();
            } else if (startup_waited >= policy.startup_timeout) {
                report.outcome = SessionOutcome::StartupTimedOut;
                report.stop =
                    stop_game_session(ops, policy.startup_stop_grace, poll);
                return report;
            }
        }
        ops.sleep(poll);
        if (!report.ready) startup_waited += poll;
    }
}

SessionOps real_session_ops(pid_t pid, const std::string& ready_marker,
                            std::function<void()> keepalive) {
    SessionOps ops;
    ops.exited = [pid] {
        siginfo_t info;
        std::memset(&info, 0, sizeof(info));
        int r;
        do {
            r = ::waitid(P_PID, static_cast<id_t>(pid), &info,
                         WEXITED | WNOHANG | WNOWAIT);
        } while (r < 0 && errno == EINTR);
        if (r < 0) return errno == ECHILD;  // already reaped: certainly gone
        return info.si_pid == pid;
    };
    ops.ready = [pid] { return process_has_drm_card_open(pid); };
    ops.stop_requested = [] { return session_stop_requested(); };
    ops.signal_leader = [pid](int sig) { ::kill(pid, sig); };
    ops.signal_group = [pid](int sig) { ::kill(-pid, sig); };
    ops.keepalive = std::move(keepalive);
    ops.on_ready = [pid, ready_marker] {
        if (ready_marker.empty()) return;
        std::ofstream marker(ready_marker, std::ios::trunc);
        marker << pid << "\n";
    };
    ops.sleep = [](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); };
    return ops;
}

int reap_session(pid_t pid) {
    int status = 0;
    pid_t r;
    do {
        r = ::waitpid(pid, &status, 0);
    } while (r < 0 && errno == EINTR);
    return r == pid ? status : -1;
}

}  // namespace retroarch
