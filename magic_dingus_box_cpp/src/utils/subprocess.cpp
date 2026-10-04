#include "subprocess.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace utils::subprocess {

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// pipe2(O_CLOEXEC) where it exists (Linux); pipe + fcntl elsewhere (the Mac
// test build). The window between pipe() and fcntl() only matters if
// another thread forks in between, which the Mac tests never do.
bool make_pipe(int fds[2]) {
#if defined(__linux__)
    return ::pipe2(fds, O_CLOEXEC) == 0;
#else
    if (::pipe(fds) != 0) return false;
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
#endif
}

void close_fds_from_3() {
#if defined(__linux__) && defined(SYS_close_range)
    if (::syscall(SYS_close_range, 3U, ~0U, 0U) == 0) return;
#endif
    long max_fd = ::sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536) max_fd = 65536;
    for (int fd = 3; fd < max_fd; ++fd) ::close(fd);
}

int decode_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

// waitpid(WNOHANG), EINTR-safe. true once the child has been reaped.
bool try_reap(pid_t pid, int* status) {
    for (;;) {
        const pid_t r = ::waitpid(pid, status, WNOHANG);
        if (r == pid) return true;
        if (r < 0 && errno == EINTR) continue;
        // r == 0: still running. r < 0 (ECHILD): someone else reaped it —
        // nothing left to wait for; report as reaped with an unknown status.
        if (r < 0) { *status = 0; return true; }
        return false;
    }
}

void reap_blocking(pid_t pid, int* status) {
    while (::waitpid(pid, status, 0) < 0) {
        if (errno != EINTR) { *status = 0; return; }
    }
}

// Poll for exit until `until`, sleeping in short steps. true if reaped.
bool reap_until(pid_t pid, int* status, Clock::time_point until) {
    milliseconds step(1);
    for (;;) {
        if (try_reap(pid, status)) return true;
        const auto now = Clock::now();
        if (now >= until) return false;
        const auto left = std::chrono::duration_cast<milliseconds>(until - now);
        std::this_thread::sleep_for(std::min(step, left + milliseconds(1)));
        if (step < milliseconds(10)) step *= 2;
    }
}

}  // namespace

Result run(const std::vector<std::string>& argv, const Options& opts) {
    Result result;
    if (argv.empty() || argv[0].empty()) return result;   // exit_code -1

    const milliseconds timeout = opts.timeout > milliseconds(0) ? opts.timeout
                                                                : milliseconds(1);
    const auto deadline = Clock::now() + timeout;

    // Everything the child touches is prepared here, before fork: between
    // fork and exec only async-signal-safe calls are allowed.
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    const int devnull = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    if (devnull < 0) return result;

    int out_pipe[2] = {-1, -1};
    if (opts.capture_stdout && !make_pipe(out_pipe)) {
        ::close(devnull);
        return result;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(devnull);
        if (out_pipe[0] >= 0) { ::close(out_pipe[0]); ::close(out_pipe[1]); }
        return result;
    }

    if (pid == 0) {
        // ---- child: async-signal-safe calls only ----
        // Own process group, so a timeout can signal everything this
        // command spawned (sudo -> nmcli, sh -> its children).
        ::setpgid(0, 0);
        // dup2 clears FD_CLOEXEC on the target; the sources are CLOEXEC and
        // vanish at exec.
        ::dup2(devnull, STDIN_FILENO);
        const int out_fd = opts.capture_stdout ? out_pipe[1] : devnull;
        ::dup2(out_fd, STDOUT_FILENO);
        if (opts.stderr_mode == Stderr::MergeIntoStdout) {
            ::dup2(out_fd, STDERR_FILENO);
        } else {
            ::dup2(devnull, STDERR_FILENO);
        }
        // exec keeps SIG_IGN dispositions and the signal mask, and the
        // kiosk ignores SIGPIPE — reset both so helpers behave as they
        // would from a shell.
        struct sigaction dfl;
        std::memset(&dfl, 0, sizeof(dfl));
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        for (int sig = 1; sig < NSIG; ++sig) {
            if (sig == SIGKILL || sig == SIGSTOP) continue;
            ::sigaction(sig, &dfl, nullptr);   // EINVAL for odd numbers: fine
        }
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        // No DRM master / evdev grab / GStreamer fd may outlive into a
        // helper — most kiosk fds are opened without O_CLOEXEC.
        close_fds_from_3();
        ::execvp(cargv[0], cargv.data());
        _exit(127);
    }

    // ---- parent ----
    // Repeat setpgid to close the fork/exec race (EACCES: the child already
    // exec'd, having grouped itself first; ESRCH: it is already gone).
    ::setpgid(pid, pid);
    ::close(devnull);
    if (out_pipe[1] >= 0) ::close(out_pipe[1]);

    int status = 0;
    bool reaped = false;

    if (opts.capture_stdout) {
        const int rfd = out_pipe[0];
        char buf[16384];
        for (;;) {
            const auto now = Clock::now();
            if (now >= deadline) { result.timed_out = true; break; }
            // Wake at least every 50 ms to notice a child that exited while
            // a grandchild still holds the write end (no EOF would come).
            const auto left = std::chrono::duration_cast<milliseconds>(deadline - now);
            const int wait_ms = static_cast<int>(std::min<long long>(left.count() + 1, 50));
            struct pollfd pfd{rfd, POLLIN, 0};
            const int pr = ::poll(&pfd, 1, wait_ms);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;   // poll itself failed: stop reading, still reap below
            }
            if (pr == 0) {
                if (try_reap(pid, &status)) {
                    reaped = true;
                    // Take whatever is already buffered, without blocking.
                    const int fl = ::fcntl(rfd, F_GETFL);
                    ::fcntl(rfd, F_SETFL, fl | O_NONBLOCK);
                    for (;;) {
                        const ssize_t n = ::read(rfd, buf, sizeof(buf));
                        if (n > 0) {
                            const std::size_t room = opts.max_output > result.out.size()
                                ? opts.max_output - result.out.size() : 0;
                            result.out.append(buf, std::min<std::size_t>(room, static_cast<std::size_t>(n)));
                            continue;
                        }
                        if (n < 0 && errno == EINTR) continue;
                        break;
                    }
                    break;
                }
                continue;
            }
            const ssize_t n = ::read(rfd, buf, sizeof(buf));
            if (n > 0) {
                const std::size_t room = opts.max_output > result.out.size()
                    ? opts.max_output - result.out.size() : 0;
                result.out.append(buf, std::min<std::size_t>(room, static_cast<std::size_t>(n)));
                continue;
            }
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            break;   // EOF (or a read error): the child closed stdout
        }
        ::close(rfd);
    }

    if (!reaped && !result.timed_out) {
        reaped = reap_until(pid, &status, deadline);
        if (!reaped) result.timed_out = true;
    }

    if (!reaped) {
        // Deadline passed. TERM the group first — sudo relays it to its
        // root child, which SIGKILL to sudo would orphan — then KILL.
        ::kill(-pid, SIGTERM);
        ::kill(pid, SIGTERM);   // in case setpgid lost a race with exec
        if (!reap_until(pid, &status, Clock::now() + opts.kill_grace)) {
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
            reap_blocking(pid, &status);
        }
    }

    result.exit_code = decode_status(status);
    return result;
}

}  // namespace utils::subprocess
